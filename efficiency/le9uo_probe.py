"""Validate the isolated le9uo kernel and its live sysctl contract without pressure."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess


EXPECTED = {"workingset_protection": 1, "anon_min_ratio": 0,
            "clean_low_ratio": 5, "clean_min_ratio": 0}


def inspect(args):
    def root(code):
        p = subprocess.run(["adb", "-s", args.serial, "shell", "su -c " + shlex.quote(code)],
                           capture_output=True, check=True, timeout=30)
        return p.stdout.decode("utf-8", "replace").replace("\r", "").strip()

    result = {"passed": False, "controls_exercised": False,
              "protection_branch_observed": False, "performance_benefit_verified": False}
    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("This probe applies only to PLQ110")
    result["boot_id"] = root("cat /proc/sys/kernel/random/boot_id")
    result["kernel_notes_sha256"] = root("sha256sum /sys/kernel/notes").split()[0]
    if result["kernel_notes_sha256"] != args.expected_notes:
        result["reason"] = "Unexpected kernel; no control writes were attempted"
        return result
    controls = {name: int(root(f"cat /proc/sys/vm/{name}")) for name in EXPECTED}
    result["controls"] = controls
    result["expected_controls"] = EXPECTED
    if controls != EXPECTED:
        result["reason"] = "Unexpected policy; leave the user's settings untouched"
        return result
    checks = {"expected_kernel": True, "configured_defaults": True}
    if args.test_controls:
        tests = []
        # Rewriting the current value invalidates cached thresholds without
        # changing policy. Rejected writes must leave the value unchanged.
        for name, original in controls.items():
            knob = f"/proc/sys/vm/{name}"
            observed = int(root(f"printf '%s\\n' {original} > {knob} && cat {knob}"))
            tests.append({"name": name, "input": original, "accepted": True,
                          "unchanged": observed == original})
            if name == "workingset_protection":
                continue
            for invalid in (-1, 101):
                output = root(f"""if printf '%s\\n' {invalid} > {knob} 2>/dev/null; then
    echo accepted
else
    echo rejected
fi
cat {knob}
""").splitlines()
                unchanged = int(output[-1]) == original
                tests.append({"name": name, "input": invalid,
                              "rejected": output[0] == "rejected", "unchanged": unchanged})
                if not unchanged:
                    # A broken control implementation is not a successful
                    # test; restore the original before stopping immediately.
                    root(f"printf '%s\\n' {original} > {knob}")
                    raise RuntimeError("An invalid sysctl write changed the live policy")
        result["control_tests"] = tests
        result["controls_exercised"] = True
        checks["range_validation_and_no_policy_change"] = all(
            t.get("accepted", t.get("rejected", False)) and t["unchanged"] for t in tests)
    meminfo = root("cat /proc/meminfo")
    total_kb = int(re.search(r"^MemTotal:\s+(\d+)", meminfo, re.M)[1])
    page_kb = int(root("getconf PAGESIZE")) // 1024
    zone = root("cat /proc/zoneinfo")
    stats = {}
    for name in ("nr_active_file", "nr_inactive_file", "nr_isolated_file", "nr_dirty"):
        # This device has one populated node. Each per-node counter appears
        # once, beneath its populated zone, and is not a zone-page counter.
        values = re.findall(r"^\s+" + name + r"\s+(\d+)\s*$", zone, re.M)
        if not values:
            raise RuntimeError("Missing node counter: " + name)
        stats[name] = sum(map(int, values))
    clean = max(0, stats["nr_active_file"] + stats["nr_inactive_file"] +
                stats["nr_isolated_file"] - stats["nr_dirty"])
    floor = (total_kb // page_kb) * controls["clean_low_ratio"] // 100
    result["threshold_snapshot"] = {"unit": "pages", "page_kb": page_kb,
                                    "clean_file_pages": clean, "clean_low_pages": floor,
                                    "clean_low_mib": floor * page_kb / 1024,
                                    "below_soft_floor_in_snapshot": clean < floor,
                                    "node_counters": stats}
    result["reason"] = ("Live controls validated; a threshold snapshot does not prove "
                        "that the reclaim protection branch was executed or improved performance")
    result["controls_after"] = {name: int(root(f"cat /proc/sys/vm/{name}")) for name in EXPECTED}
    checks["policy_preserved"] = result["controls_after"] == controls
    checks["same_boot_after_test"] = root("cat /proc/sys/kernel/random/boot_id") == result["boot_id"]
    result.update(checks=checks, passed=all(checks.values()))
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--expected-notes", required=True)
    parser.add_argument("--test-controls", action="store_true")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = inspect(args)
    except Exception as error:
        result = {"passed": False, "reason": str(error)}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
