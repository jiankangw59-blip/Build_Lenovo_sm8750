"""Private-device SDDC source rewrite/discard/cold-read and optional A/B test."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shlex
import subprocess
import time
import uuid

from run_probe import INPUT_BYTES, Target, PrivateRound, preflight, cold_read_reached_backend


def require_alias_and_delta(snapshot):
    stats = snapshot["sddc"]
    return stats["aliases"] + stats["wb_aliases"] > 0 and stats["deltas"] + stats["wb_deltas"] > 0


def run(args):
    target = Target(args.serial)
    root = target.root
    result = {"passed": False, "device_mutations_started": False, "runtime_test_performed": False,
              "runtime_test_attempted": False, "devices_created": 0,
              "ref_slots_directly_identified": False, "generation_ids_directly_observed": False,
              "every_ref_source_overwritten_proven": False, "all_races_proven": False,
              "ufs_nand_writes_measured": False, "power_savings_verified": False,
              "old_seed": args.old_seed, "new_seed": args.new_seed, "cases": []}
    stage = None
    cleanup_ok = True

    def main_config():
        return root("cat /sys/block/zram0/disksize /sys/block/zram0/comp_algorithm /sys/block/zram0/backing_dev; cat /sys/block/zram0/sddc_stat | head -1")

    try:
        result["preflight"] = preflight(root, args.expected_notes, "crystal", "lz4kds",
                                          args.expected_main_codec, args.expected_main_sddc)
        if args.check_only:
            result.update(passed=True, phase="read_only_preflight")
            return result
        blob = args.binary.read_bytes()
        if blob[:6] != b"\x7fELF\x02\x01" or int.from_bytes(blob[18:20], "little") != 183:
            raise RuntimeError("Require the compiled ARM64 lifecycle helper")
        result["helper_sha256"] = hashlib.sha256(blob).hexdigest()
        result["main_configuration_before"] = main_config()
        result["device_mutations_started"] = True
        stage = "/data/adb/ace6lc-" + uuid.uuid4().hex[:20]
        root(f"umask 077; mkdir -m 700 {stage}")
        program = stage + "/probe"
        target.adb("push", str(args.binary), program)
        if root(f"sha256sum {program}").split()[0] != result["helper_sha256"]:
            raise RuntimeError("Staged lifecycle helper checksum mismatch")
        root(f"set -e; chmod 700 {program}; chcon u:object_r:ksu_file:s0 {program}")
        cases = ["sequential"] + (["concurrent"] if args.concurrent_seconds else [])
        for number, kind in enumerate(cases):
            private = PrivateRound(root, stage, number, "crystal", "lz4kds", 1)
            entry = {"kind": kind, "passed": False, "commands": {}, "snapshots": {}}
            result["cases"].append(entry)

            def snap(label):
                value = private.snapshot()
                entry["snapshots"][label] = value
                if value["io_stat"][:2] != [0, 0] or value["sddc"]["decode_failures"]:
                    raise RuntimeError("Private data I/O or SDDC decode failure")
                return value

            def command(mode, seconds=None, label=None):
                private.assert_owned(require_backing=True)
                words = [program, private.device, mode, str(args.old_seed), str(args.new_seed)]
                if seconds is not None:
                    words.append(str(seconds))
                code = " ".join(shlex.quote(word) for word in words)
                p = subprocess.run(["adb", "-s", args.serial, "shell", "su -c " + shlex.quote(code)],
                                   capture_output=True, timeout=120)
                args.output.parent.mkdir(parents=True, exist_ok=True)
                label = label or mode
                prefix = args.output.with_suffix(f".{kind}-{label}")
                Path(str(prefix) + ".stdout.txt").write_bytes(p.stdout)
                Path(str(prefix) + ".stderr.txt").write_bytes(p.stderr)
                entry["commands"][label] = {"exit_code": p.returncode, "raw_stdout": str(prefix) + ".stdout.txt",
                                            "raw_stderr": str(prefix) + ".stderr.txt"}
                if p.returncode:
                    raise RuntimeError(p.stderr.decode("utf-8", "replace")[-4000:])
                data = json.loads(p.stdout.decode("utf-8", "replace").replace("\r", ""))
                entry["commands"][label]["result"] = data
                if not data.get("passed"):
                    raise RuntimeError("Lifecycle helper did not complete successfully")
                return data

            def wait_settled(label, require_conversions=False):
                observations = []
                for attempt in range(20):
                    value = private.snapshot()
                    observations.append(value["sddc"])
                    if value["io_stat"][:2] != [0, 0] or value["sddc"]["decode_failures"]:
                        raise RuntimeError("Data I/O or decoder failure while waiting for SDDC work")
                    if not value["sddc"]["pending"] and (not require_conversions or require_alias_and_delta(value)):
                        entry["snapshots"][label] = value
                        entry[label + "_sddc_observations"] = observations
                        return value
                    time.sleep(0.1)
                entry["snapshots"][label] = value
                entry[label + "_sddc_observations"] = observations
                raise RuntimeError("Bounded SDDC settle/alias-and-delta coverage requirement was not met")

            try:
                private.prepare()
                entry["device"] = private.device
                snap("initial")
                result["runtime_test_attempted"] = True
                command("fill")
                conversions = wait_settled("after_fill", require_conversions=True)
                entry["actual_alias_observed"] = conversions["sddc"]["aliases"] + conversions["sddc"]["wb_aliases"] > 0
                entry["actual_delta_observed"] = conversions["sddc"]["deltas"] + conversions["sddc"]["wb_deltas"] > 0
                if command("verify-filled")["bytes_processed"] != INPUT_BYTES:
                    raise RuntimeError("Initial known-data verification was incomplete")
                if kind == "concurrent":
                    data = command("concurrent", args.concurrent_seconds)
                    snap("after_concurrent")
                    entry["checks"] = {"actual_alias_and_delta": entry["actual_alias_observed"] and entry["actual_delta_observed"],
                        "bounded_threads_completed": data["seconds"] == args.concurrent_seconds,
                        "reads_only_complete_a_or_b": data["reads_accepting_only_complete_a_or_b"] >= 64,
                        "each_writer_progressed": data["writer_even_pages"] >= 64 and data["writer_odd_pages"] >= 64,
                        "final_per_page_versions_verified": data["bytes_verified_after_join"] == INPUT_BYTES,
                        "no_concurrent_discard": data["discard_performed"] is False}
                else:
                    overwritten = command("overwrite-sources")
                    wait_settled("after_overwrite")
                    verified = command("verify-overwrite")
                    snap("after_verify_overwrite")
                    discarded = command("discard-sources")
                    wait_settled("after_discard")
                    discard_verified = command("verify-discard", label="verify-discard-before-writeback")
                    before = snap("before_writeback")
                    private.assert_owned(require_backing=True)
                    root(f"set -e; printf 'all\\n' > {private.sysfs}/idle; printf 'idle\\n' > {private.sysfs}/writeback", timeout=120)
                    snap("after_writeback_before_flush")
                    command("flush")
                    flushed = snap("after_flush_before_first_cold_read")
                    cold_verified = command("verify-discard", label="first-cold-read")
                    after = snap("after_first_cold_read")
                    writes = flushed["zms"]["physical_write_pages"] - before["zms"]["physical_write_pages"]
                    reads = after["zms"]["physical_read_pages"] - flushed["zms"]["physical_read_pages"]
                    entry["checks"] = {"actual_alias_and_delta": entry["actual_alias_observed"] and entry["actual_delta_observed"],
                        "only_first_pair_overwritten": overwritten["pages_processed"] == 2048,
                        "all_pages_correct_after_overwrite": verified["bytes_processed"] == INPUT_BYTES,
                        "only_first_pair_discarded": discarded["pages_discarded"] == 2048,
                        "zeros_and_old_targets_correct_after_discard": discard_verified["bytes_processed"] == INPUT_BYTES,
                        "explicit_flush_completed": entry["commands"]["flush"]["result"]["fsync"] is True,
                        "zms_dirty_pages_zero_after_flush": flushed["zms"]["dirty_pages"] == 0,
                        "real_backend_writes": writes > 0,
                        "first_cold_read_reached_backend": cold_read_reached_backend(flushed, after, "crystal"),
                        "full_known_cold_read": cold_verified["bytes_processed"] == INPUT_BYTES,
                        "cold_hash_matches_before_writeback": cold_verified["fnv1a64"] == discard_verified["fnv1a64"],
                        "no_failed_backend_io": not after["zms"]["physical_write_failed_pages"] and not after["zms"]["physical_read_failed_pages"]}
                    entry["measurements"] = {"zms_physical_write_pages_delta": writes, "zms_physical_read_pages_delta": reads,
                        "foreground_bd_read_pages_delta": after["bd_stat"][1] - flushed["bd_stat"][1],
                        "loop_write_sectors_including_internal_io": after["loop"]["write_sectors"] - before["loop"]["write_sectors"],
                        "loop_read_sectors_during_first_cold_read": after["loop"]["read_sectors"] - flushed["loop"]["read_sectors"],
                        "packed_bytes_after_flush": flushed["zms"]["packed_bytes"],
                        "stored_bytes_after_flush": flushed["zms"]["stored_bytes"],
                        "native_sddc_wire_writeback_observed": flushed["sddc"]["wb_aliases"] + flushed["sddc"]["wb_deltas"] > 0}
                entry["passed"] = all(entry["checks"].values())
                result["runtime_test_performed"] = True
                if not entry["passed"]:
                    raise RuntimeError("SDDC lifecycle data/coverage checks failed")
            except Exception as error:
                entry["error"] = str(error)
                raise
            finally:
                if private.index is not None and 0 < private.index < 1024:
                    result["devices_created"] += 1
                try:
                    private.cleanup()
                    entry["cleanup"] = "complete"
                except Exception as error:
                    cleanup_ok = False
                    entry["cleanup"] = str(error)
                    raise
        result["passed"] = all(case["passed"] for case in result["cases"])
    except Exception as error:
        result["error"] = str(error)
    finally:
        if "main_configuration_before" in result:
            try:
                result["main_configuration_after"] = main_config()
                result["main_configuration_unchanged"] = result["main_configuration_after"] == result["main_configuration_before"]
                result["same_boot"] = root("cat /proc/sys/kernel/random/boot_id") == result["preflight"]["boot_id"]
                result["passed"] = result["passed"] and result["main_configuration_unchanged"] and result["same_boot"]
            except Exception as error:
                result["main_readback_error"] = str(error)
                result["passed"] = False
        if stage and cleanup_ok:
            try:
                root(f"set -e; test ! -L {stage}; rm -f {stage}/probe; rmdir {stage}")
                result["staging_cleanup"] = "complete"
            except Exception as error:
                result["staging_cleanup"] = str(error)
                result["passed"] = False
        elif stage:
            result["preserved_stage"] = stage
            result["passed"] = False
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--expected-notes", action="append", required=True)
    parser.add_argument("--expected-main-codec", choices=["lz4kd", "lz4kds"], default="lz4kd")
    parser.add_argument("--expected-main-sddc", type=int, choices=[0, 1], default=0)
    parser.add_argument("--old-seed", type=lambda x: int(x, 0), default=0x1234ABCD)
    parser.add_argument("--new-seed", type=lambda x: int(x, 0), default=0x78EF019B)
    parser.add_argument("--concurrent-seconds", type=int, choices=[0, 2, 3, 4, 5], default=0)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.check_only and not args.binary:
        parser.error("Provide the compiled lifecycle helper")
    if any(not re.fullmatch(r"[0-9a-f]{64}", entry) for entry in args.expected_notes):
        parser.error("Notes entries must be full lowercase SHA256 values")
    if not 0 <= args.old_seed < 1 << 64 or not 0 <= args.new_seed < 1 << 64 or args.old_seed == args.new_seed:
        parser.error("Provide two distinct uint64 seeds")
    unsafe = {(group * 0x9E3779B97F4A7C15) & ((1 << 64) - 1) for group in range(1, 1025)}
    if args.old_seed in unsafe or args.new_seed in unsafe:
        parser.error("Seeds must not generate an all-zero A/B source page")
    report = run(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
    raise SystemExit(0 if report["passed"] else 1)
