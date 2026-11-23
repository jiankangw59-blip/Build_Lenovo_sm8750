"""Validate only the probe process's own pages on an already configured main ZRAM."""
import argparse
import hashlib
import json
from pathlib import Path, PurePosixPath
import re
import shlex
import subprocess
import uuid

from run_probe import Target, named_stats


def preflight_owned(root, allowed_notes, backend, codec, expected_backing_file, expected_sddc,
                    minimum_budget=64):
    """All device operations here are reads; do not stage or execute before return."""
    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("Owned-pages test applies only to PLQ110")
    notes_hash = root("sha256sum /sys/kernel/notes").split()[0]
    if notes_hash not in allowed_notes:
        raise RuntimeError("Unexpected kernel notes; no device writes attempted")
    notes_hex = "".join(root("od -An -v -t x1 /sys/kernel/notes").split())
    if not re.fullmatch(r"[0-9a-f]{2,1024}", notes_hex) or len(notes_hex) % 2:
        raise RuntimeError("Malformed raw kernel-notes hexadecimal dump")
    if hashlib.sha256(bytes.fromhex(notes_hex)).hexdigest() != notes_hash:
        raise RuntimeError("Independent notes dump/hash does not match")
    zms = root("test -r /sys/block/zram0/zms_stat && echo yes || echo no") == "yes"
    marker = root("test -r /sys/block/zram0/hybridswap_crystal_stat && echo yes || echo no") == "yes"
    if (backend == "crystal" and not (zms and marker)) or (backend == "native" and (zms or marker)):
        raise RuntimeError("The expected main ZRAM provider is absent")
    algorithms = root("cat /sys/block/zram0/comp_algorithm")
    if f"[{codec}]" not in algorithms:
        raise RuntimeError("Unexpected active main codec; never change it in this probe")
    swaps = root("awk 'NR>1 {print $1}' /proc/swaps").splitlines()
    if swaps not in [["/dev/block/zram0"], ["/dev/zram0"]]:
        raise RuntimeError("Require the sole active main zram0")
    backing = root("cat /sys/block/zram0/backing_dev")
    if not re.fullmatch(r"/dev/block/loop\d+", backing):
        raise RuntimeError("Require the reviewed private loop backing, not a vendor partition")
    owner = root(f"cat /sys/block/{backing.rsplit('/', 1)[-1]}/loop/backing_file")
    if owner != expected_backing_file:
        raise RuntimeError("Main backing owner differs from the explicitly reviewed file")
    quoted = shlex.quote(owner)
    if root(f"test -f {quoted} -a ! -L {quoted} && echo yes || echo no") != "yes":
        raise RuntimeError("Expected an existing regular, non-symlink backing file")
    metadata = root(f"stat -c '%u:%g %a %s' {quoted}").split()
    if len(metadata) != 3 or metadata[:2] != ["0:0", "600"] or int(metadata[2]) < 4096:
        raise RuntimeError("Backing owner/permissions/size differ from the private profile")
    if "u:object_r:ksu_file:s0" not in root(f"ls -Zd {quoted}"):
        raise RuntimeError("Backing SELinux label differs from the tested private profile")
    limit_enabled = int(root("cat /sys/block/zram0/writeback_limit_enable"))
    limit = int(root("cat /sys/block/zram0/writeback_limit"))
    if limit_enabled and limit < minimum_budget:
        raise RuntimeError("Existing writeback budget is insufficient; never replenish it")
    if int(root("awk '$1==\"MemAvailable:\" {print $2}' /proc/meminfo")) < 524288:
        raise RuntimeError("Preserve at least 512 MiB available RAM")
    if backend == "crystal":
        sddc = named_stats(root("cat /sys/block/zram0/sddc_stat"))
        zms_stats = named_stats(root("cat /sys/block/zram0/zms_stat"))
        if sddc.get("enabled") != expected_sddc or zms_stats.get("enabled") != 1 or zms_stats.get("ret") != 0:
            raise RuntimeError("Main SDDC/ZMS runtime state differs from the expected initialized profile")
    return {"notes_sha256": notes_hash, "notes_hex": notes_hex,
            "boot_id": root("cat /proc/sys/kernel/random/boot_id"),
            "backing_node": backing, "backing_owner": owner,
            "backing_metadata": metadata, "codec": codec,
            "writeback_limit_enabled": bool(limit_enabled), "existing_limit_logical_slots": limit,
            "disksize": int(root("cat /sys/block/zram0/disksize"))}


def run(args):
    result = {"passed": False, "device_mutations_started": False, "helper_executed": False,
              "backend": args.backend, "codec": args.codec, "serial": args.serial,
              "raw_main_block_read_or_write_performed": False, "budget_modified_by_probe": False,
              "kcompressd_path_proven": False, "power_savings_verified": False,
              "global_counters_may_include_concurrent_io": True}
    target = Target(args.serial)
    root = target.root
    stage = None
    try:
        result["preflight"] = preflight_owned(root, args.expected_notes, args.backend, args.codec,
                                               args.expected_backing_file, args.expected_sddc)
        if args.check_only:
            result.update(passed=True, phase="read_only_preflight")
            return result
        blob = args.binary.read_bytes()
        if blob[:6] != b"\x7fELF\x02\x01" or int.from_bytes(blob[18:20], "little") != 183:
            raise RuntimeError("Require the compiled Android ARM64 owned-pages helper")
        result["helper_sha256"] = hashlib.sha256(blob).hexdigest()
        result["device_mutations_started"] = True
        stage = "/data/adb/ace6own-" + uuid.uuid4().hex[:20]
        root(f"umask 077; mkdir -m 700 {stage}")
        program = stage + "/probe"
        target.adb("push", str(args.binary), program)
        if root(f"sha256sum {program}").split()[0] != result["helper_sha256"]:
            raise RuntimeError("Staged owned-pages helper hash differs")
        root(f"set -e; chmod 700 {program}; chcon u:object_r:ksu_file:s0 {program}")
        before = result["preflight"]
        # Recheck after staging; the C helper pins these exact notes bytes and
        # backing-file identity again before every individual page-index write.
        recheck = preflight_owned(root, args.expected_notes, args.backend, args.codec,
                                   args.expected_backing_file, args.expected_sddc)
        if any(recheck[key] != before[key] for key in ("boot_id", "notes_sha256", "backing_node", "backing_owner", "disksize")):
            raise RuntimeError("Protected main configuration changed during staging")
        command = [program, args.backend, args.codec, before["backing_node"], before["backing_owner"],
                   before["notes_hex"], str(args.seed), str(args.expected_sddc)]
        result["helper_executed"] = True
        # Preserve partial stdout and stderr even on a failing kernel readback.
        process = subprocess.run(["adb", "-s", args.serial, "shell",
                                  "su -c " + shlex.quote(" ".join(shlex.quote(x) for x in command))],
                                 capture_output=True, timeout=120)
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.with_suffix(".helper.stdout.txt").write_bytes(process.stdout)
        args.output.with_suffix(".helper.stderr.txt").write_bytes(process.stderr)
        result["helper_exit_code"] = process.returncode
        if process.returncode:
            raise RuntimeError(process.stderr.decode("utf-8", "replace")[-4000:])
        data = json.loads(process.stdout.decode("utf-8", "replace").replace("\r", ""))
        result["owned_result"] = data
        result["passed"] = (data.get("passed") is True and data.get("bytes_verified") == 4194304
                            and data.get("madv_pageout_bytes") == 262144
                            and data.get("logical_page_index_requests_accepted") == 64
                            and data.get("explicit_post_writeback_block_sync") is True
                            and data.get("raw_main_block_read_or_write_performed") is False
                            and data.get("budget_modified_by_probe") is False)
    except Exception as error:
        result["error"] = str(error)
    finally:
        if result["helper_executed"]:
            try:
                after = preflight_owned(root, args.expected_notes, args.backend, args.codec,
                                          args.expected_backing_file, args.expected_sddc, minimum_budget=0)
                result["after"] = after
                # Budget may legitimately be low after requests; the dedicated
                # final-state reader below does not demand another 64-page run.
                result["main_configuration_unchanged"] = all(after[key] == result["preflight"][key]
                    for key in ("boot_id", "notes_sha256", "backing_node", "backing_owner", "codec", "disksize", "writeback_limit_enabled"))
                result["passed"] = result["passed"] and result["main_configuration_unchanged"]
            except Exception as error:
                result["after_readback_error"] = str(error)
                result["passed"] = False
        if stage:
            try:
                root(f"set -e; test ! -L {stage}; rm -f {stage}/probe; rmdir {stage}")
                result["staging_cleanup"] = "complete"
            except Exception as error:
                result["staging_cleanup"] = str(error)
                result["passed"] = False
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--backend", choices=["native", "crystal"], required=True)
    parser.add_argument("--codec", choices=["lz4kd", "lz4kds"], required=True)
    parser.add_argument("--expected-sddc", type=int, choices=[0, 1], default=0)
    parser.add_argument("--expected-notes", action="append", required=True)
    parser.add_argument("--expected-backing-file", required=True)
    parser.add_argument("--seed", type=lambda x: int(x, 0), default=0x1234ABCD)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--check-only", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.check_only and not args.binary:
        parser.error("Require --binary for the owned-pages test")
    if any(not re.fullmatch(r"[0-9a-f]{64}", value) for value in args.expected_notes):
        parser.error("Notes entries must be full lowercase SHA256 values")
    path = PurePosixPath(args.expected_backing_file)
    if not path.is_absolute() or not str(path).startswith("/data/adb/") or ".." in path.parts:
        parser.error("Explicit backing owner must be a root-private /data/adb file")
    if not 0 <= args.seed < 1 << 64:
        parser.error("Seed must fit uint64")
    if args.backend == "native" and (args.codec != "lz4kd" or args.expected_sddc):
        parser.error("Native control uses lz4kd with SDDC absent")
    if args.codec == "lz4kds" and args.expected_sddc != 1:
        parser.error("LZ4KDS requires explicit SDDC=1")
    result = run(args)
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
