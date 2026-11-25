"""Exercise early-boot control flow with a private filesystem and mocked I/O.

These checks do not run an Android kernel, allocate an 8-GiB file, or access ADB.
"""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess


def check(args):
    if args.output.exists():
        raise RuntimeError("Preserve existing evidence; choose a fresh output directory")
    args.output.mkdir(parents=True)
    here = Path(__file__).resolve().parent
    cases = {
        "marker_absent": {"marker": False, "inactive": True},
        "native_backend": {"zms": False, "inactive": True},
        "le9uo_notes": {"notes_match": False, "inactive": True},
        "wrong_model": {"model": "OTHER", "inactive": True},
        "duplicate_enrollment": {"duplicate": True, "inactive": True},
        "config_disabled": {"zram": "false", "reason": "configuration_not_requested_or_unsupported"},
        "config_codec_mismatch": {"requested_codec": "lz4", "reason": "configuration_not_requested_or_unsupported"},
        "config_size_zero": {"size": 0, "reason": "configuration_not_requested_or_unsupported"},
        "config_size_overflow": {"size": "9999999999999999999999999999", "reason": "configuration_not_requested_or_unsupported"},
        "foreign_backing": {"backing": "foreign", "reason": "foreign_backing"},
        "busy_swap": {"used": 65537, "reason": "busy_swap_or_ram_headroom"},
        "low_headroom": {"available": 1048576, "reason": "busy_swap_or_ram_headroom"},
        "already_matches": {"initial_size": 17179869184, "backing": "ours", "reason": "already_matches"},
        "initialize_lz4kd": {"reason": "configured"},
        "initialize_lz4kds": {"codec": "lz4kds", "reason": "configured"},
        "writeback_false": {"writeback": "false", "reason": "configured"},
        "configured_capacity": {"size": 8192, "reason": "configured"},
        "bind_failure_ram_recovery": {"fail": "backing_dev", "reason": "initialization_failed", "recovered": "yes"},
        "size_failure_original_recovery": {"fail": "disksize", "persistent_size_failure": True,
                                             "reason": "initialization_failed", "recovered": "yes"},
    }
    results = []
    for name, case in cases.items():
        root = (args.output / name).resolve()
        state = root / "state"
        node = root / "sys/block/zram0"
        for path in [state, node, root / "sys/block/loop49/loop", root / "dev/block", root / "proc/sys/kernel/random"]:
            path.mkdir(parents=True, exist_ok=True)
        codec = case.get("codec", "lz4kd")
        notes = "a" * 64
        known = (notes if case.get("notes_match", True) else "b" * 64) + " " + codec + "\n"
        (state / "known-crystal-kernels").write_text(known * (2 if case.get("duplicate") else 1))
        if case.get("marker", True):
            (state / "crystal-writeback.enabled").touch()
        for filename in ["backing_dev", "hybridswap_enable", "hybridswap_core_enable", "reset", "writeback_limit", "writeback_limit_enable"]:
            (node / filename).write_text("none\n" if filename == "backing_dev" else "0\n")
        if case.get("zms", True):
            (node / "zms_stat").touch()
        (node / "comp_algorithm").write_text("lz4 [" + codec + "]\n")
        (node / "disksize").write_text(str(case.get("initial_size", 4294967296)) + "\n")
        loop = (root / "dev/block/loop49").as_posix()
        (root / "dev/block/loop49").touch()
        backing = (state / "zms-backing.img").as_posix()
        (root / "sys/block/loop49/loop/backing_file").write_text(backing + "\n")
        if case.get("backing"):
            (node / "backing_dev").write_text(loop + "\n" if case["backing"] == "ours" else "/dev/block/mapper/unowned\n")
        config = (f"zram={case.get('zram', 'true')}\nzram_size={case.get('size', 16384)}\n"
                  f"comp_algorithm={case.get('requested_codec', codec)}\n"
                  + (f"zram_writeback={case['writeback']}\n" if "writeback" in case else ""))
        (root / "swap_config.conf").write_text(config)
        config_before = hashlib.sha256((root / "swap_config.conf").read_bytes()).digest()
        (root / "proc/version").write_text("Linux version 6.6.89-host-model\n")
        (root / "proc/sys/kernel/random/boot_id").write_text("host-mocked-boot\n")
        (root / "proc/meminfo").write_text(f"MemAvailable: {case.get('available', 5000000)} kB\n")
        swaps = "Filename Type Size Used Priority\n" + (root / "dev/block/zram0").as_posix() + f" partition 4194300 {case.get('used', 0)} 0\n"
        (root / "proc/swaps").write_text(swaps)
        helpers = (here / "crystal_writeback.sh").read_text(encoding="utf-8")
        for original, replacement in [
            ("/data/adb/ace6-kernel-efficiency", state.as_posix()),
            ("/data/swap_config.conf", (root / "swap_config.conf").as_posix()),
            ("/sys/block", (root / "sys/block").as_posix()),
            ("/dev/block/zram0", (root / "dev/block/zram0").as_posix()),
            ("/dev/zram0", (root / "dev/zram0").as_posix()),
            ("/proc/swaps", (root / "proc/swaps").as_posix()),
            ("/proc/meminfo", (root / "proc/meminfo").as_posix()),
            ("/proc/version", (root / "proc/version").as_posix()),
            ("/proc/sys/kernel/random/boot_id", (root / "proc/sys/kernel/random/boot_id").as_posix()),
            ("/system/bin/swapoff", "test_swapoff"),
            ("/system/bin/swapon", "test_swapon"),
            ("/system/bin/mkswap", "test_mkswap"),
        ]:
            helpers = helpers.replace(original, replacement)
        # Only syscall/storage boundaries are mocked. Enrollment, configuration,
        # transition gates, idempotence, reset ordering and recovery run as shipped.
        helpers += f"""
getprop() {{ printf '%s\\n' '{case.get('model', 'PLQ110')}'; }}
sha256sum() {{ printf '%s  %s\\n' '{notes}' "$1"; }}
dmesg() {{ printf 'host mock; no real kernel observed\\n'; }}
flock() {{ return 0; }}
ace6_crystal_loop_path() {{ [ "$1" = '{loop}' ] && printf '%s\\n' "$1"; }}
ace6_crystal_validate_backing() {{ return 0; }}
ace6_crystal_prepare_backing() {{ _ACE6_CRYSTAL_LOOP='{loop}'; }}
test_mkswap() {{ printf 'mkswap\\n' >> '{root.as_posix()}/operations'; }}
test_swapoff() {{
 printf 'swapoff\\n' >> '{root.as_posix()}/operations'
 printf 'Filename Type Size Used Priority\\n' > '{root.as_posix()}/proc/swaps'
}}
test_swapon() {{
 printf 'swapon\\n' >> '{root.as_posix()}/operations'
 printf 'Filename Type Size Used Priority\\n%s partition 16777212 0 0\\n' '{root.as_posix()}/dev/block/zram0' > '{root.as_posix()}/proc/swaps'
}}
_test_fail='{case.get('fail', '')}'
ace6_crystal_write_node() {{
 printf '%s=%s\\n' "$2" "$1" >> '{root.as_posix()}/operations'
 if [ "$2" = "$_test_fail" ]; then
   _test_fail=''
   return 1
 fi
 if [ '{str(case.get('persistent_size_failure', False))}' = True ] &&
    [ "$2" = disksize ] && [ "$1" != 4294967296 ]; then return 1; fi
 case "$2" in
  reset) printf '0\\n' > "$ACE6_CRYSTAL_NODE/disksize"; printf 'none\\n' > "$ACE6_CRYSTAL_NODE/backing_dev" ;;
  comp_algorithm) printf '[%s]\\n' "$1" > "$ACE6_CRYSTAL_NODE/comp_algorithm" ;;
  *) printf '%s\\n' "$1" > "$ACE6_CRYSTAL_NODE/$2" ;;
 esac
}}
"""
        (root / "crystal_writeback.sh").write_text(helpers, encoding="utf-8", newline="\n")
        (root / "early_crystal_writeback.sh").write_text((here / "early_crystal_writeback.sh").read_text(), newline="\n")
        subprocess.run([str(args.bash), (root / "early_crystal_writeback.sh").as_posix()], check=True, timeout=15,
                       stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        status_file = state / "early-crystal-writeback.status"
        operations = (root / "operations").read_text().splitlines() if (root / "operations").exists() else []
        if case.get("inactive"):
            assert not status_file.exists() and not operations and not (state / "early-kernel-startup.raw").exists(), name
            status = {"result": "inactive"}
        else:
            status = dict(line.split("=", 1) for line in status_file.read_text().splitlines())
            assert status["reason"] == case["reason"], (name, status)
            assert status["auto_policy"] == "disabled", name
            assert "comp_algorithm=" not in "\n".join(operations), (name, operations)
            if case["reason"] != "configured" and "failure" not in name:
                assert "swapoff" not in operations and "reset=1" not in operations, (name, operations)
            if case.get("recovered"):
                assert status["ram_swap_recovered"] == case["recovered"], (name, status)
                assert (node / "backing_dev").read_text().strip() == "none", name
            if case.get("writeback") == "false":
                assert not any(x.startswith("backing_dev=") for x in operations), (name, operations)
            if case["reason"] == "configured":
                assert (node / "disksize").read_text().strip() == str(int(case.get("size", 16384)) * 1048576), name
        assert hashlib.sha256((root / "swap_config.conf").read_bytes()).digest() == config_before, name
        results.append({"case": name, "passed": True, "status": status, "operations": operations})
    report = {"passed": True, "cases": results, "checks": len(results),
              "kernel_io_mocked": True, "android_runtime_verified": False, "adb_used": False}
    (args.output / "host-checks.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps({"passed": True, "checks": len(results), "android_runtime_verified": False}))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bash", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    check(parser.parse_args())
