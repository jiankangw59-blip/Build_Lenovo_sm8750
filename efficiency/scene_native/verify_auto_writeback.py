"""Verify one controlled automatic round against an independently recorded counter."""
import argparse
import datetime
import json
from pathlib import Path
import re
import shlex
import subprocess

STATE = "/data/adb/ace6-kernel-efficiency"


def inspect(args):
    def root(code):
        p = subprocess.run(["adb", "-s", args.serial, "shell", "su -c " + shlex.quote(code)],
                           capture_output=True, check=True, timeout=30)
        return p.stdout.decode("utf-8", "replace").replace("\r", "").strip()

    if root("getprop ro.product.model") != "PLQ110":
        raise RuntimeError("This acceptance applies only to PLQ110")
    boot_id = root("cat /proc/sys/kernel/random/boot_id")
    notes = root("sha256sum /sys/kernel/notes").split()[0]
    status = dict(line.split("=", 1) for line in root(f"cat {STATE}/native-writeback.status").splitlines())
    pid = int(status["pid"])
    command = root(f"tr '\\000' ' ' < /proc/{pid}/cmdline")
    proc = root(f"cat /proc/{pid}/stat").rpartition(") ")[2].split()
    hz = int(root("getconf CLK_TCK"))
    start = int(proc[19]) / hz
    last_boot, last_round = root(f"cat {STATE}/native-writeback.last-round").split()
    last_round = int(last_round)
    uptime = float(root("cat /proc/uptime").split()[0])
    log = root(f"cat {STATE}/native-writeback.log")
    done = [line for line in log.splitlines() if " DONE trigger=" in line]
    fields = dict(re.findall(r"(\w+)=([^\s]+)", done[-1])) if done else {}
    written = int(fields.get("pages", "0"))
    before_done = log[:log.rfind(" DONE trigger=")] if done else ""
    marks = [line for line in before_done.splitlines() if " MARK trigger=" in line]
    mark = marks[-1] if marks else ""
    elapsed = 0
    if mark and done:
        def timestamp(line):
            return datetime.datetime.fromisoformat(line.split()[0].replace("Z", "+00:00"))
        elapsed = (timestamp(done[-1]) - timestamp(mark)).total_seconds()
    counters = list(map(int, root("cat /sys/block/zram0/bd_stat").split()))
    io_stat = list(map(int, root("cat /sys/block/zram0/io_stat").split()))
    limit = int(root("cat /sys/block/zram0/writeback_limit"))
    daily_date, daily_pages = root(f"cat {STATE}/native-writeback.daily").split()
    affinity = root(f"grep '^Cpus_allowed:' /proc/{pid}/status").split()[1]
    checks = {
        "expected_kernel": notes == args.expected_notes,
        "controller_alive": "native_writeback_daemon.sh" in command,
        "same_boot": last_boot == boot_id and root(f"cat {STATE}/native-writeback.boot-id") == boot_id,
        "round_since_current_process_started": start <= last_round <= uptime,
        "returned_to_event_wait": status.get("phase") == "waiting_events",
        "corrected_controller_completed": fields.get("cap_reached") in ("yes", "no"),
        "real_screen_trigger": fields.get("trigger") == "screen_off" and "trigger=screen_off" in mark,
        "cold_grace_at_least_five_minutes": elapsed >= 300,
        "counter_delta_matches_automatic_round": written > 0 and counters[2] - args.before_writes == written,
        "kernel_budget_not_refilled": limit + counters[2] == 2097152,
        "daily_budget_recorded": int(daily_pages) >= written,
        "no_failed_zram_data_io": io_stat[0] == 0 and io_stat[1] == 0,
        "background_priority": int(proc[16]) == 19 and int(affinity, 16) & ~0x3F == 0,
    }
    return {"passed": all(checks.values()), "checks": checks, "boot_id": boot_id,
            "kernel_notes_sha256": notes, "controller_pid": pid,
            "before_writes": args.before_writes, "bd_stat": counters,
            "automatic_pages": written, "automatic_bytes": written * 4096,
            "cold_wait_seconds_including_write": elapsed, "last_mark": mark,
            "last_done": done[-1] if done else None, "io_stat": io_stat,
            "boot_budget_pages_left": limit, "daily_date": daily_date,
            "daily_pages": int(daily_pages), "power_savings_verified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--before-writes", type=int, required=True)
    parser.add_argument("--expected-notes", required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = inspect(args)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))
    raise SystemExit(0 if result["passed"] else 1)
