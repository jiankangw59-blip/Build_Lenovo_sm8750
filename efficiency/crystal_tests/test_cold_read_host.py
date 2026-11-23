"""Focused real-Python cold-read predicate checks; no device or kernel model."""
import argparse
import hashlib
import json
from pathlib import Path
import unittest

from run_probe import cold_read_reached_backend


def snapshot(front=10, physical=20, sectors=160):
    return {"bd_stat": [0, front, 0], "zms": {"physical_read_pages": physical},
            "loop": {"read_sectors": sectors}}


class ColdReadTests(unittest.TestCase):
    def test_gc_only_physical_and_loop_growth_is_rejected(self):
        self.assertFalse(cold_read_reached_backend(snapshot(), snapshot(10, 30, 240), "crystal"))

    def test_real_frontend_physical_loop_growth_passes(self):
        self.assertTrue(cold_read_reached_backend(snapshot(), snapshot(11, 21, 168), "crystal"))

    def test_frontend_without_physical_growth_is_rejected(self):
        self.assertFalse(cold_read_reached_backend(snapshot(), snapshot(11, 20, 168), "crystal"))

    def test_frontend_without_loop_growth_is_rejected(self):
        self.assertFalse(cold_read_reached_backend(snapshot(), snapshot(11, 21, 160), "crystal"))

    def test_counter_regression_is_rejected(self):
        self.assertFalse(cold_read_reached_backend(snapshot(), snapshot(9, 21, 168), "crystal"))
        self.assertFalse(cold_read_reached_backend(snapshot(), snapshot(11, 19, 168), "crystal"))

    def test_native_uses_native_frontend_and_loop_counters(self):
        before, after = snapshot(), snapshot(11, 20, 168)
        before.pop("zms")
        after.pop("zms")
        self.assertTrue(cold_read_reached_backend(before, after, "native"))
        after["bd_stat"][1] = 10
        self.assertFalse(cold_read_reached_backend(before, after, "native"))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(ColdReadTests))
    here = Path(__file__).resolve().parent
    report = {"passed": result.wasSuccessful(), "tests": result.testsRun,
              "device_used": False, "kernel_runtime_verified": False,
              "scope": "Actual shared Python predicate; supplied counter values only",
              "source_sha256": {name: hashlib.sha256((here / name).read_bytes()).hexdigest()
                                for name in ("run_probe.py", "run_lifecycle.py", "test_cold_read_host.py")}}
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    raise SystemExit(0 if report["passed"] else 1)
