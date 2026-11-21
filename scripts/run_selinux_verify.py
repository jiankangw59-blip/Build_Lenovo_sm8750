#!/usr/bin/env python3
"""Run the post-flash SELinux generation, mmap ABI and AVC regression checks."""
import argparse
import hashlib
import json
import re
import shlex
import subprocess
import time
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--probe", type=Path, required=True,
                        help="Static aarch64 build of selinux_probe.c")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--app-uid", type=int, default=10000)
    parser.add_argument("--boot", type=Path, help="Require active boot partition to match this image")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=True)
    remote = "/data/local/tmp/selinux-verify-probe"
    results = {"serial": args.serial, "passed": False, "checks": {}}

    def adb(*command, timeout=30):
        process = subprocess.run(["adb", "-s", args.serial, *command], capture_output=True, timeout=timeout)
        output = (process.stdout + process.stderr).decode("utf-8", "replace").replace("\r", "")
        if process.returncode:
            raise RuntimeError(f"adb failed ({process.returncode}): {output[-1200:]}")
        return output.strip()

    def root(command):
        return adb("shell", "su -c " + shlex.quote(command))

    def save(name, output):
        (args.output / name).write_text(output + "\n", encoding="utf-8")
        print(output, flush=True)

    def cache_snapshot():
        output = root("cat /sys/fs/selinux/avc/cache_stats")
        lines = output.splitlines()
        if not lines or lines[0].split()[:3] != ["lookups", "hits", "misses"]:
            raise RuntimeError("Unexpected AVC cache_stats format")
        return [list(map(int, line.split())) for line in lines[1:] if line.strip()]

    def cache_check(label):
        adb("shell", "am start -a android.settings.SETTINGS")
        before = cache_snapshot()
        start = time.monotonic()
        # Generate checks through visible system UI while sampling the cache.
        while time.monotonic() - start < 5:
            adb("shell", "input keyevent KEYCODE_HOME")
            adb("shell", "am start -a android.settings.SETTINGS")
            time.sleep(0.3)
        after = cache_snapshot()
        if len(before) != len(after):
            raise RuntimeError("CPU count changed during AVC sampling")
        totals = [0, 0, 0]
        for old, new in zip(before, after):
            for index in range(3):
                # Kernel cache counters can wrap at 32 bits independently per CPU.
                totals[index] += new[index] - old[index] if new[index] >= old[index] else (
                    new[index] - old[index]) % (1 << (64 if old[index] > 0xffffffff else 32))
        hits, misses = totals[1], totals[2]
        rate = hits / (hits + misses) if hits + misses else 0
        check = {"delta_hits": hits, "delta_misses": misses, "hit_rate": rate,
                 "seconds": time.monotonic() - start, "passed": rate > 0.95}
        results["checks"][label + "_avc"] = check
        save(label + "-avc.json", json.dumps(check, indent=2))
        if not check["passed"]:
            raise RuntimeError(f"AVC delta hit rate is {rate:.2%}, requires >95%")

    def probes(label):
        for name, suffix in (("root", ""), ("app-zygote", f" {args.app_uid} u:r:app_zygote:s0"),
                             ("secondary-user", f" {100000 + args.app_uid} u:r:app_zygote:s0"),
                             ("isolated-uid-carrier", " 99000 u:r:app_zygote:s0")):
            output = root(remote + suffix + "; probe_rc=$?; echo PROBE_EXIT=$probe_rc")
            save(label + "-" + name + ".txt", output)
            passed = "[FAIL]" not in output and "PROBE_EXIT=0" in output
            status = re.search(r"status mmap\(off=0\): ver=(\d+) seq=(\d+) enf=(\d+) pload=(\d+) deny=(\d+)", output)
            check = {"passed": passed}
            if status:
                check.update({"status_sequence": int(status[2]), "policyload": int(status[4]),
                              "deny_unknown": int(status[5])})
            results["checks"][label + "_" + name] = check
            if not passed:
                raise RuntimeError(f"{label} {name} probe failed")
        real_deny = results["checks"][label + "_root"]["deny_unknown"]
        for name in ("app-zygote", "secondary-user", "isolated-uid-carrier"):
            if results["checks"][label + "_" + name]["deny_unknown"] != real_deny:
                raise RuntimeError("Hidden status deny_unknown differs from the real policy")

    try:
        identity = root("getprop ro.product.model; getprop ro.boot.slot_suffix; uname -a; cat /proc/sys/kernel/random/boot_id")
        save("device.txt", identity)
        if args.boot:
            slot = adb("shell", "getprop ro.boot.slot_suffix")
            if slot not in ("_a", "_b"):
                raise RuntimeError(f"Unexpected active slot: {slot}")
            expected = hashlib.sha256(args.boot.read_bytes()).hexdigest()
            actual = root("sha256sum /dev/block/by-name/boot" + slot).split()[0]
            results["checks"]["boot_readback"] = {"expected": expected, "actual": actual,
                                                    "passed": actual == expected}
            if actual != expected:
                raise RuntimeError("Active boot partition differs from the CI verified image")
        previous_dmesg = root("dmesg")
        timestamps = re.findall(r"^\[\s*(\d+\.\d+)\]", previous_dmesg, re.M)
        last_time = max(map(float, timestamps), default=0)
        adb("push", str(args.probe), remote)
        root("chmod 755 " + remote)
        probes("baseline")
        cache_check("baseline")
        mutation = root("/data/adb/ksu/bin/ksud sepolicy patch 'allow untrusted_app untrusted_app process getattr'")
        save("sepolicy-patch.txt", mutation)
        probes("post-policy")
        initial = results["checks"]["baseline_root"]
        updated = results["checks"]["post-policy_root"]
        if (updated["policyload"] <= initial["policyload"] or
                updated["status_sequence"] <= initial["status_sequence"]):
            raise RuntimeError("Real policy generation or status sequence did not advance after the policy update")
        cache_check("post-policy")
        recent = []
        for line in root("dmesg").splitlines():
            stamp = re.match(r"^\[\s*(\d+\.\d+)\]", line)
            if stamp and float(stamp[1]) > last_time:
                recent.append(line)
        errors = [line for line in recent if re.search(r"seqno.*latest_notif", line, re.I)]
        save("dmesg-seqno-errors.txt", "\n".join(errors) or "No new seqno/latest_notif errors")
        logcat = root("logcat -d -t 1500")
        daemon_errors = [line for line in logcat.splitlines() if re.search(
            r"(?:selinux_status|servicemanager.*selinux).*(?:fail|error)|avc.*seqno", line, re.I)]
        save("daemon-errors.txt", "\n".join(daemon_errors) or "No SELinux synchronization errors in recent daemon logs")
        if errors or daemon_errors:
            raise RuntimeError("SELinux synchronization errors were found")
        results["passed"] = True
    except (OSError, RuntimeError, subprocess.TimeoutExpired) as error:
        results["error"] = str(error)
        print(str(error), flush=True)
    finally:
        (args.output / "acceptance.json").write_text(json.dumps(results, indent=2) + "\n", encoding="utf-8")
    return 0 if results["passed"] else 1


if __name__ == "__main__":
    raise SystemExit(main())
