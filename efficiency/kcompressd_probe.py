"""Prove native ZRAM writes by kcompressd, correlated with owned swap slots."""
import argparse
import json
from pathlib import Path
import re
import shlex
import subprocess
import uuid


def correlate(trace, device, owned):
    found = set()
    writes = 0
    for major, minor, rwbs, sector, sectors, comm in re.findall(
            r"block_bio_queue:\s+(\d+),(\d+)\s+(\S+)\s+(\d+)\s+\+\s+(\d+)\s+\[([^]]+)\]", trace):
        if (int(major), int(minor)) != device or "W" not in rwbs or not comm.startswith("kcompressd"):
            continue
        writes += 1
        begin, end = (int(sector) + 7) // 8, (int(sector) + int(sectors)) // 8
        found.update(slot for slot in range(begin, end) if slot in owned)
    return writes, found


def inspect(args):
    def root(code, timeout=30):
        p = subprocess.run(["adb", "-s", args.serial, "shell", "su -c " + shlex.quote(code)],
                           capture_output=True, check=True, timeout=timeout)
        return p.stdout.decode("utf-8", "replace").replace("\r", "").strip()

    result = {"passed": False, "phase": "preflight", "trace_instance_created": False}
    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("Probe applies only to PLQ110")
    result["kernel_notes_sha256"] = root("sha256sum /sys/kernel/notes").split()[0]
    if result["kernel_notes_sha256"] != args.expected_notes:
        result["reason"] = "Unexpected running kernel"
        return result
    depth = root("if [ -f /proc/sys/vm/kcompressd ]; then cat /proc/sys/vm/kcompressd; else echo absent; fi")
    result["queue_depth"] = depth
    if depth == "absent" or not int(depth):
        result["reason"] = "Kcompressd is absent or disabled; no workload or tracing was started"
        return result
    processes = root("ps -A -o PID,NAME").splitlines()[1:]
    pids = [int(line.split()[0]) for line in processes
            if len(line.split()) == 2 and re.fullmatch(r"kcompressd\d+", line.split()[1].strip("[]"))]
    if not pids:
        result["reason"] = "No dedicated kcompressd thread"
        return result
    threads = []
    for pid in pids:
        stat = root(f"cat /proc/{pid}/stat").rpartition(") ")[2].split()
        mask = root(f"grep '^Cpus_allowed:' /proc/{pid}/status").split()[1]
        threads.append({"pid": pid, "nice": int(stat[16]), "cpu_mask": mask,
                        "kernel_thread": int(stat[1]) == 2})
    result["threads"] = threads
    if any(not t["kernel_thread"] or t["nice"] != 19 or int(t["cpu_mask"], 16) & ~0x3F for t in threads):
        result["reason"] = "Unexpected kcompressd priority or affinity"
        return result
    swaps = root("awk 'NR>1 {print $1}' /proc/swaps").splitlines()
    if swaps not in [["/dev/block/zram0"], ["/dev/zram0"]] or "[lz4kd]" not in root("cat /sys/block/zram0/comp_algorithm"):
        result["reason"] = "Expected sole native lz4kd ZRAM"
        return result
    if not args.binary:
        result["reason"] = "Runtime feature detected; provide the bounded workload binary to prove offload"
        return result
    args.output.parent.mkdir(parents=True, exist_ok=True)
    before = root("cat /sys/kernel/tracing/current_tracer /sys/kernel/tracing/tracing_on")
    instance = "/sys/kernel/tracing/instances/ace6_kcomp_" + uuid.uuid4().hex[:12]
    stage = "/data/local/tmp/ace6-kcomp-workload-" + uuid.uuid4().hex[:12]
    subprocess.run(["adb", "-s", args.serial, "push", str(args.binary), stage], check=True, capture_output=True)
    major, minor = map(int, root("cat /sys/block/zram0/dev").split(":"))
    trace_filter = "(" + " || ".join(f"common_pid == {pid}" for pid in pids) + f") && dev == {(major << 20) | minor}"
    try:
        root(f"mkdir {instance}")
        result["trace_instance_created"] = True
        root(f"""set -e
printf '0\\n' > {instance}/tracing_on
printf 'nop\\n' > {instance}/current_tracer
printf '2048\\n' > {instance}/buffer_size_kb
printf '%s\\n' {shlex.quote(trace_filter)} > {instance}/events/block/block_bio_queue/filter
printf '1\\n' > {instance}/events/block/block_bio_queue/enable
printf '1\\n' > {instance}/tracing_on
chmod 755 {stage}
""")
        command = f"{stage} {args.seconds}"
        if args.watermark_scale_factor is not None:
            # Keep the test finite even if the host disappears. The device-side
            # trap restores on ordinary exit/signals; a second process handles
            # a killed shell. Never replace a value changed by another tuner.
            knob = "/proc/sys/vm/watermark_scale_factor"
            original = int(root(f"cat {knob}"))
            target = args.watermark_scale_factor
            if target <= original:
                raise RuntimeError("The test watermark must exceed the current value")
            result["temporary_watermark"] = {"before": original, "during": target}
            command = f"""set -e
restore_test_watermark() {{
    if [ "$(cat {knob})" = {target} ]; then
        printf '%s\\n' {original} > {knob}
    fi
}}
trap restore_test_watermark EXIT
trap 'exit 130' HUP INT TERM
printf '%s\\n' {target} > {knob}
(
    trap - EXIT HUP INT TERM
    sleep {args.seconds + 20}
    restore_test_watermark
) </dev/null >/dev/null 2>&1 &
watchdog=$!
status=0
{stage} {args.seconds} || status=$?
restore_test_watermark
kill "$watchdog" 2>/dev/null || true
wait "$watchdog" 2>/dev/null || true
exit "$status"
"""
        payload = json.loads(root(command, timeout=args.seconds + 30))
        root(f"printf '0\\n' > {instance}/tracing_on")
        trace = root(f"cat {instance}/trace")
        trace_path = args.output.with_suffix(".trace.txt")
        trace_path.write_text(trace + "\n", encoding="utf-8")
        writes, matched = correlate(trace, (major, minor), set(payload.pop("swap_slots")))
        counts = re.search(r"entries-in-buffer/entries-written:\s+(\d+)/(\d+)", trace)
        result.update(phase="observed", workload=payload, worker_write_events=writes,
                      background_swap_writes_observed=writes > 0,
                      owned_page_offload_proven=len(matched) >= 64,
                      trace_events_overwritten=int(counts[2]) - int(counts[1]) if counts else None,
                      matched_owned_pages=len(matched), matched_slot_samples=sorted(matched)[:64],
                      trace=str(trace_path), trace_filter=trace_filter,
                      passed=payload["checksum_verified"] and len(matched) >= 64)
        if not result["passed"]:
            result["reason"] = ("Background writes observed but not correlated with owned pages; no performance conclusion"
                                if writes else "Insufficient observed background offload of owned pages; no performance conclusion")
    finally:
        if result["trace_instance_created"]:
            root(f"""set -e
printf '0\\n' > {instance}/events/block/block_bio_queue/enable
printf '0\\n' > {instance}/tracing_on
rmdir {instance}
""")
            result["trace_instance_cleaned"] = True
        root(f"rm -f {stage}")
        result["global_trace_configuration_unchanged"] = root(
            "cat /sys/kernel/tracing/current_tracer /sys/kernel/tracing/tracing_on") == before
        if "temporary_watermark" in result:
            observed = int(root("cat /proc/sys/vm/watermark_scale_factor"))
            result["temporary_watermark"]["after"] = observed
            result["temporary_watermark"]["restored"] = observed == original
    result["passed"] = result["passed"] and result["global_trace_configuration_unchanged"]
    if "temporary_watermark" in result:
        result["passed"] = result["passed"] and result["temporary_watermark"]["restored"]
    return result


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--expected-notes", required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--seconds", type=int, choices=range(10, 121), default=40)
    parser.add_argument("--watermark-scale-factor", type=int, choices=range(100, 601),
                        help="Temporarily request more background free pages; restored after this test")
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    try:
        result = inspect(args)
    except Exception as error:
        result = {"passed": False, "phase": "error", "reason": str(error)}
        if isinstance(error, subprocess.CalledProcessError):
            result["device_stderr"] = (error.stderr or b"").decode("utf-8", "replace")[-4000:]
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
