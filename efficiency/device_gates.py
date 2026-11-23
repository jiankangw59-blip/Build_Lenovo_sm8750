"""Read-only runtime gates; never infer boot success from a CI build alone."""
import argparse
import json
import pathlib
import re
import shlex
import subprocess


def inspect(args):
    def root(code):
        result = subprocess.run(
            ["adb", "-s", args.serial, "shell", "su -c " + shlex.quote(code)],
            capture_output=True, timeout=45, check=True)
        return result.stdout.decode("utf-8", "replace").replace("\r", "").strip()

    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("Runtime baseline applies only to PLQ110")
    result = {"serial": args.serial, "boot_id": root("cat /proc/sys/kernel/random/boot_id"),
              "kernel_version": root("cat /proc/version"), "gates": {}}
    notes = root("sha256sum /sys/kernel/notes").split()[0]
    result["kernel_notes_sha256"] = notes
    result["expected_kernel_notes_match"] = (
        notes == args.expected_notes_sha256 if args.expected_notes_sha256 else None)
    startup = ""
    if args.startup_snapshot:
        raw_startup = args.startup_snapshot.read_bytes()
        startup = raw_startup.decode("utf-8")
        if result["boot_id"] not in startup.splitlines() or not any(
                line.split() == [notes, "/sys/kernel/notes"] for line in startup.splitlines()):
            raise RuntimeError("Startup snapshot belongs to a different boot or kernel")
        import hashlib
        result["startup_snapshot"] = {"path": str(args.startup_snapshot),
                                       "sha256": hashlib.sha256(raw_startup).hexdigest()}

    modules = {line.split()[0] for line in root("cat /proc/modules").splitlines() if line}
    critical = {"sched_walt", "qti_thermal_vendor_hooks", "qcom_cpufreq_thermal",
                "thermal_config", "oplus_charger_present", "oplus_network_tuning"}
    log = root("dmesg") + "\n" + startup
    candidates = [line for line in log.splitlines() if re.search(
        r"disagrees about version|unknown symbol|failed to load", line, re.I)]
    firmware = [line for line in candidates if re.search(
        r"cfg80211:\s+failed to load regulatory\.db\s*$", line, re.I)]
    errors = [line for line in candidates if line not in firmware]
    result["gates"]["factory_modules"] = {
        "passed": not (critical - modules) and not errors,
        "missing_critical_modules": sorted(critical - modules), "load_errors": errors,
        "loaded_modules": sorted(modules), "firmware_warnings": firmware}

    lazy = root("cat /sys/module/rcutree/parameters/enable_rcu_lazy")
    threads = root("ps -A -T -o PID,TID,NAME")
    rcu = set(re.findall(r"\brcuop/(\d+)\b", threads))
    result["gates"]["rcu_lazy"] = {
        "passed": lazy == "Y" and set(map(str, range(8))).issubset(rcu),
        "enabled": lazy, "offload_cpus": sorted(map(int, rcu))}

    mask = root("cat /sys/devices/virtual/workqueue/cpumask")
    efficient = root("cat /sys/module/workqueue/parameters/power_efficient")
    workers = root('''for p in /proc/[0-9]*; do
        read -r name < "$p/comm" 2>/dev/null || continue
        case "$name" in kworker/u*)
            allowed=$(sed -n 's/^Cpus_allowed:[[:space:]]*//p' "$p/status" 2>/dev/null)
            printf '%s %s %s\n' "${p##*/}" "$name" "$allowed" ;;
        esac
    done
    true''')
    violations = []
    observed = []
    for line in workers.splitlines():
        fields = line.split()
        if len(fields) != 3:
            continue
        pid, name, allowed = fields
        entry = {"pid": int(pid), "name": name, "cpus_allowed": allowed}
        observed.append(entry)
        if int(allowed.replace(",", ""), 16) & ~0x3f:
            violations.append(entry)
    result["gates"]["workqueues"] = {
        "passed": int(mask.replace(",", ""), 16) == 0x3f and efficient == "Y"
                  and bool(observed) and not violations,
        "global_mask": mask, "power_efficient": efficient,
        "unbound_workers": observed, "affinity_violations": violations,
        "scope": "Snapshot of kworker/u affinity; CPU-bound queues retain their own policy."}

    algorithm = root("cat /sys/block/zram0/comp_algorithm")
    backing = root("cat /sys/block/zram0/backing_dev 2>/dev/null || true")
    bd = root("cat /sys/block/zram0/bd_stat 2>/dev/null || true")
    counts = list(map(int, bd.split())) if re.fullmatch(r"\d+\s+\d+\s+\d+", bd) else []
    memory_errors = [line for line in log.splitlines() if re.search(
        r"(?:zram|lz4kd).*(?:fail|error|corrupt)|(?:\bBUG:|\bOops:|I/O error)", line, re.I)]
    result["gates"]["zram_writeback"] = {
        "passed": "[lz4kd]" in algorithm and bool(counts)
                  and counts[1] > 0 and counts[2] > 0 and not memory_errors,
        "algorithm": algorithm, "backing_device": backing, "bd_stat": bd,
        "errors": memory_errors,
        "scope": "Counters require backing reads and writes; data integrity needs the separate isolated readback probe."}

    probe = root("/data/local/tmp/ace6-rekernel-probe 2>&1 || true")
    hook_lines = [line for line in log.splitlines() if re.search(
        r"Re.?Kernel|binder async cleanup|register kprobe hooks failed", line, re.I)]
    result["gates"]["builtin_rekernel"] = {
        "passed": "rekernel" not in modules and "version_response=11.7" in probe
                  and "Re-Kernel hooked!" in log,
        "duplicate_module_loaded": "rekernel" in modules, "version_query": probe,
        "initialization_log": hook_lines,
        "transport": "Official Generic Netlink; /proc/rekernel is not created."}

    result["appopt"] = root('''for pid in $(pidof AppOpt); do
        for t in /proc/$pid/task/*; do
            printf '%s ' "${t##*/}"
            cat "$t/comm"
            grep Cpus_allowed_list "$t/status"
        done
    done
    true''')
    result["runtime_network"] = root(
        "cat /proc/sys/net/ipv4/tcp_congestion_control /proc/sys/net/ipv4/tcp_ecn "
        "/proc/sys/net/core/default_qdisc 2>/dev/null; true")
    result["power"] = root('''for n in status capacity current_now voltage_now charge_counter; do
        printf '%s=' "$n"; cat /sys/class/power_supply/battery/$n 2>/dev/null
    done
    true''')
    result["all_runtime_gates_passed"] = all(g["passed"] for g in result["gates"].values())
    if result["expected_kernel_notes_match"] is False:
        result["all_runtime_gates_passed"] = False
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"boot_id": result["boot_id"], "kernel_notes_sha256": notes,
                      "expected_kernel_notes_match": result["expected_kernel_notes_match"],
                      "gates": {k: v["passed"] for k, v in result["gates"].items()},
                      "passed": result["all_runtime_gates_passed"]}, indent=2))
    return result["all_runtime_gates_passed"]


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--expected-notes-sha256")
    parser.add_argument("--startup-snapshot", type=pathlib.Path,
                        help="Earlier startup capture containing this boot ID, kernel-notes hash and init logs")
    args = parser.parse_args()
    raise SystemExit(0 if inspect(args) else 1)
