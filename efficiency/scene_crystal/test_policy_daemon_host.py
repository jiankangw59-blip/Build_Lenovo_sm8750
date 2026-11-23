"""Focused final daemon regression with source hashes fixed before and after."""
import argparse
from concurrent.futures import ThreadPoolExecutor
import hashlib
import json
from pathlib import Path

from test_policy_host import Fixture, HERE


def sources():
    return {path.name: hashlib.sha256(path.read_bytes()).hexdigest()
            for path in HERE.iterdir() if path.suffix in (".sh", ".c", ".conf")}


def run(args):
    args.output.mkdir(parents=True, exist_ok=False)
    before = sources()
    cases = [
        ("quota_stop", "32768 4096 0 none 0 1", "lz4kd", 0),
        ("partial_error", "10000 1000 0 none 0 1", "lz4kd", 1),
        ("physical_error", "32768 4096 1 none 0 1", "lz4kd", 1),
        ("gc_read_error_at_quota", "32768 4096 0 physical_read_failure 1 1", "lz4kd", 1),
        ("gc_read_error_success_rc", "32768 4096 0 physical_read_failure 1 0", "lz4kd", 1),
        ("integrity_error", "32768 4096 0 integrity_failures 1 1", "lz4kds", 1),
        ("decode_error", "32768 4096 0 decode_failures 1 1", "lz4kds", 1),
        ("flatten_error", "32768 4096 0 flatten_failures 1 1", "lz4kds", 1),
        ("proof_error", "32768 4096 0 delta_proof_failures 1 1", "lz4kds", 1),
        ("io_stat_error", "32768 4096 0 io_write_failure 1 1", "lz4kd", 1),
        ("screen_on_cancel", "32768 4096 0 none 0 1", "lz4kd", 0),
    ]
    def check(case):
        name, result, codec, rc = case
        fixture = Fixture(args.output / name, args.bash, codec)
        (fixture.root / "mock-result").write_text(result + "\n")
        if name == "screen_on_cancel":
            (fixture.root / "grace_rc").write_text("1\n")
        fixture.run("", rc=rc, daemon=True)
        if name == "screen_on_cancel":
            assert not (fixture.root / "submissions").exists()
            assert (fixture.node / "writeback_limit").read_text().strip() == "100000"
        else:
            assert (fixture.root / "submissions").read_text().splitlines() == ["submitted"]
            assert (fixture.state / "crystal-writeback.blocked").exists() == bool(rc)
            if not rc:
                assert fixture.daily() == "20261124 4096"
                assert "quota_stop=yes" in (fixture.state / "crystal-writeback.log").read_text()
                (fixture.root / "proc/uptime").write_text("10500.00 20000.00\n")
                fixture.run("", daemon=True)
                assert (fixture.root / "submissions").read_text().splitlines() == ["submitted"]
            else:
                assert "DONE" not in (fixture.state / "crystal-writeback.log").read_text()
                if name.startswith("gc_read_error"):
                    assert "quota_stop=yes" not in (fixture.state / "crystal-writeback.log").read_text()
                    fields = (fixture.state / "crystal-writeback.pending").read_text().split()
                    assert fields[0] == "v2" and fields[6] == "restored" and fields[-1] == "0"
                    assert fixture.daily() == "20261124 4096"
                    assert (fixture.node / "writeback_limit").read_text().strip() == "67232"
        return {"case": name, "passed": True}
    with ThreadPoolExecutor(max_workers=2) as executor:
        results = list(executor.map(check, cases))
    assert sources() == before, "Production sources changed during the regression"
    report = {"passed": True, "checks": len(results), "cases": results,
              "source_sha256": before, "source_hashes_stable": True,
              "kernel_io_mocked": True, "event_timing_mocked": True,
              "android_runtime_verified": False, "device_used": False}
    (args.output / "host-checks.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"passed": True, "checks": len(results)}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--bash", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    run(parser.parse_args())
