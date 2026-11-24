"""Read-only runtime gates; never infer boot success from a CI build alone."""
import argparse
import hashlib
import json
import pathlib
import re
import shlex
import subprocess


def capture_lines(raw):
    # Match the strict KTAP verifier's line numbering, including older captures
    # containing CRCRLF. Never strip printk timestamps or caller fields.
    return raw.decode("utf-8", "replace").replace("\r\n", "\n").replace("\r", "\n").split("\n")


def memory_error_reasons(line):
    """Recognize data-path failures, preserving genuine errors beside zero stats."""
    context = re.search(r"zram|lz4kd|crystal|zms|sddc", line, re.I)
    cleaned = line
    reasons = []
    numeric = re.compile(r"\b([A-Za-z_][A-Za-z0-9_]*)\s*[=:]\s*([+-]?(?:0[xX][0-9a-fA-F]+|[0-9]+))(?![\w.])")
    spans = []
    hashes = {}
    for match in numeric.finditer(line):
        key, value = match[1].lower(), match[2]
        integer = int(value, 16 if value.lstrip("+-").lower().startswith("0x") else 10)
        if key in ("expected_hash", "actual_hash"):
            hashes[key] = integer
        error_key = bool(re.search(r"fail|error|corrupt|mismatch|errno", key) or key == "err")
        return_key = key in ("ret", "rc") or key.endswith(("_ret", "_rc"))
        if error_key or return_key:
            if integer == 0:
                spans.append(match.span())
            elif context and (error_key or integer < 0):
                reasons.append("nonzero_error_stat_or_status:" + key)
    for first, last in reversed(spans):
        cleaned = cleaned[:first] + " " + cleaned[last:]
    if re.search(r"\bBUG:|\bOops:|\bI/O errors?\b", cleaned, re.I):
        reasons.append("kernel_or_io_error")
    if context:
        if re.search(r"fail|error|corrupt|mismatch|bad[ _-]+(?:hash|checksum|header|payload)|\bnot ok\b", cleaned, re.I):
            reasons.append("memory_data_path_error")
        if len(hashes) == 2 and hashes["expected_hash"] != hashes["actual_hash"]:
            reasons.append("page_hash_differs")
    return reasons


def review_memory_logs(live_raw, startup_raw, notes, boot_id):
    """Only exact timestamped startup fixture records can gain an exemption."""
    startup_lines = capture_lines(startup_raw) if startup_raw is not None else []
    approved = {}
    proof = {"attempted": False, "passed": False, "errors": []}
    if startup_raw is not None and b"sddc_integrity mismatch" in startup_raw:
        try:
            from crystal.verify_kunit import strip_prefix, verify_log
            verified = verify_log(startup_raw, notes, boot_id)
            proof = {"attempted": True, "passed": verified["passed"],
                     "target_case_count": verified["target_case_count"], "log_sha256": verified["log_sha256"],
                     "verifier_sha256": verified["verifier_sha256"], "declarations": verified["declarations"],
                     "errors": verified["errors"], "controlled_selftest_logs": verified["controlled_selftest_logs"]}
            if verified["passed"] and verified["target_case_count"] == 32:
                for record in verified["controlled_selftest_logs"]["controlled"]:
                    number = record["line"]
                    raw_line = startup_lines[number - 1]
                    if (re.match(r"^(?:<\d+>)?\[\s*\d+(?:\.\d+)?\]", raw_line) and
                            strip_prefix(raw_line) == record["message"]):
                        approved[raw_line] = {"startup_line": number, "raw_line": raw_line,
                                              "fixture_path": record["fields"]["path"]}
        except (RuntimeError, ValueError, OSError, IndexError, KeyError) as error:
            proof = {"attempted": True, "passed": False, "errors": [str(error)]}
    # Deduplicate exact raw records across the two sources while retaining every
    # source/line reference. A later timestamp is a different record and fails.
    records = {}
    for source, lines in (("live_dmesg", capture_lines(live_raw)), ("startup", startup_lines)):
        for number, line in enumerate(lines, 1):
            reasons = memory_error_reasons(line)
            if not reasons:
                continue
            record = records.setdefault(line, {"raw_line": line, "reasons": reasons, "origins": []})
            record["origins"].append({"source": source, "line": number})
    errors, exemptions = [], []
    for line, record in records.items():
        if line in approved:
            exemptions.append({**record, **approved[line]})
        else:
            errors.append(record)
    return {"errors": [row["raw_line"] for row in errors], "error_records": errors,
            "controlled_selftest_exemptions": exemptions, "kunit_startup_proof": proof,
            "scope": "Exact timestamped controlled fixture lines only; no blanket KUnit/SDDC exemption"}


def inspect(args):
    def root(code, raw=False):
        result = subprocess.run(
            ["adb", "-s", args.serial, "exec-out", "su -c " + shlex.quote(code)],
            capture_output=True, timeout=45, check=True)
        if raw:
            return result.stdout
        return result.stdout.decode("utf-8", "replace").replace("\r", "").strip()

    def current_notes():
        fields = root("sha256sum /sys/kernel/notes").split()
        if len(fields) != 2 or not re.fullmatch(r"[0-9a-f]{64}", fields[0]) or fields[1] != "/sys/kernel/notes":
            raise RuntimeError("Unexpected kernel-notes identity response")
        return fields[0]

    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("Runtime baseline applies only to PLQ110")
    result = {"serial": args.serial, "boot_id": root("cat /proc/sys/kernel/random/boot_id"),
              "kernel_version": root("cat /proc/version"), "gates": {}}
    notes = current_notes()
    result["kernel_notes_sha256"] = notes
    result["expected_kernel_notes_match"] = (
        notes == args.expected_notes_sha256 if args.expected_notes_sha256 else None)
    startup = ""
    raw_startup = None
    if args.startup_snapshot:
        raw_startup = args.startup_snapshot.read_bytes()
        startup = "\n".join(capture_lines(raw_startup))
        if result["boot_id"] not in startup.splitlines() or not any(
                line.split() == [notes, "/sys/kernel/notes"] for line in startup.splitlines()):
            raise RuntimeError("Startup snapshot belongs to a different boot or kernel")
        result["startup_snapshot"] = {"path": str(args.startup_snapshot),
                                       "sha256": hashlib.sha256(raw_startup).hexdigest()}

    modules = {line.split()[0] for line in root("cat /proc/modules").splitlines() if line}
    critical = {"sched_walt", "qti_thermal_vendor_hooks", "qcom_cpufreq_thermal",
                "thermal_config", "oplus_charger_present", "oplus_network_tuning"}
    live_raw = root("dmesg", raw=True)
    log = "\n".join(capture_lines(live_raw)) + "\n" + startup
    args.output.parent.mkdir(parents=True, exist_ok=True)
    live_path = args.output.with_suffix(".dmesg.raw")
    live_path.write_bytes(live_raw)
    result["live_dmesg_capture"] = {"path": str(live_path), "sha256": hashlib.sha256(live_raw).hexdigest()}
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
    memory_review = review_memory_logs(live_raw, raw_startup, args.expected_notes_sha256 or notes, result["boot_id"])
    memory_errors = memory_review["errors"]
    result["gates"]["zram_writeback"] = {
        "passed": f"[{args.expected_compressor}]" in algorithm and bool(counts)
                  and counts[1] > 0 and counts[2] > 0 and not memory_errors,
        "algorithm": algorithm, "expected_compressor": args.expected_compressor,
        "backing_device": backing, "bd_stat": bd,
        "errors": memory_errors,
        "log_review": memory_review,
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
    end_boot = root("cat /proc/sys/kernel/random/boot_id")
    end_notes = current_notes()
    result["identity_recheck"] = {"boot_id": end_boot, "kernel_notes_sha256": end_notes,
                                  "same_boot": end_boot == result["boot_id"], "same_notes": end_notes == notes}
    result["identity_guard_passed"] = result["identity_recheck"]["same_boot"] and result["identity_recheck"]["same_notes"]
    result["all_runtime_gates_passed"] = result["identity_guard_passed"] and all(g["passed"] for g in result["gates"].values())
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
    parser.add_argument("--expected-compressor", choices=["lz4kd", "lz4kds"], default="lz4kd")
    parser.add_argument("--startup-snapshot", type=pathlib.Path,
                        help="Earlier startup capture containing this boot ID, kernel-notes hash and init logs")
    args = parser.parse_args()
    raise SystemExit(0 if inspect(args) else 1)
