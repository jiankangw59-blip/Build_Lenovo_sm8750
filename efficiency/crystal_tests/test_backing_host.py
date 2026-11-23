"""Focused real-C path admission and Python DIO/cache refusal checks; no ADB."""
import argparse
import copy
import ctypes
import json
from pathlib import Path
import unittest

from run_probe import BACKING_BYTES, PrivateRound, measure_round, INPUT_BYTES, SCHEMA
from compare import compare, STORAGE_READ_PROTOCOL


def reply(mode="check", **updates):
    value = {"passed": True, "loop_status_direct_io": True, "sysfs_dio": 1,
             "resident_pages": 0, "backing_device": 123, "backing_inode": 456,
             "loop_logical_block_bytes": 4096,
             "backing_bytes": BACKING_BYTES, "cache_eviction_requested": mode == "sync",
             "fdatasync": mode == "sync", "fsync": mode == "sync"}
    value.update(updates)
    return value


class BackingGuards(unittest.TestCase):
    def private(self, response):
        calls = []
        private = PrivateRound(lambda command, **kwargs: calls.append(command) or json.dumps(response),
            "/data/adb/ace6ct-" + "a" * 20, 0, "native", "lz4kd", 0, "/private/backing-probe")
        private.assert_owned = lambda **kwargs: None
        private.device, private.loop = "/dev/block/zram1", "/dev/block/loop13"
        private.backing_device, private.backing_inode = 123, 456
        return private, calls

    def test_dio_not_enabled_refuses(self):
        for updates in ({"sysfs_dio": 0}, {"loop_status_direct_io": False}, {"loop_logical_block_bytes": 512}):
            private, _ = self.private(reply(**updates))
            with self.assertRaises(RuntimeError):
                private.backing_check("check")

    def test_nonzero_cache_refuses(self):
        private, _ = self.private(reply(resident_pages=1))
        with self.assertRaises(RuntimeError):
            private.backing_check("check")

    def test_setup_cache_is_recorded_before_later_explicit_eviction(self):
        private, _ = self.private(reply("enable", resident_pages=4))
        self.assertEqual(private.backing_check("enable")["resident_pages"], 4)

    def test_owner_changed_refuses(self):
        for key in ("backing_device", "backing_inode", "backing_bytes"):
            private, _ = self.private(reply(**{key: 999}))
            with self.assertRaises(RuntimeError):
                private.backing_check("sync")

    def test_observation_must_not_evict(self):
        private, _ = self.private(reply(cache_eviction_requested=True))
        with self.assertRaisesRegex(RuntimeError, "never evict"):
            private.backing_check("check")

    def test_sync_requires_both_syncs_and_eviction(self):
        for key in ("fdatasync", "fsync", "cache_eviction_requested"):
            private, _ = self.private(reply("sync", **{key: False}))
            with self.assertRaisesRegex(RuntimeError, "sync/eviction"):
                private.backing_check("sync")

    def test_unsupported_ioctl_propagates(self):
        for message in ("LOOP_SET_DIRECT_IO unsupported", "LOOP_SET_BLOCK_SIZE unsupported"):
            private, calls = self.private(reply())
            def root(command, **kwargs):
                calls.append(command)
                raise RuntimeError(message)
            private.root = root
            with self.assertRaisesRegex(RuntimeError, "unsupported"):
                private.backing_check("enable")
            self.assertEqual(len(calls), 1)

    def test_helper_missing_refuses_before_hot_add(self):
        calls = []
        private = PrivateRound(lambda code: calls.append(code), "/short", 0, "native", "lz4kd", 0)
        with self.assertRaisesRegex(RuntimeError, "before creating"):
            private.prepare()
        self.assertEqual(calls, [])

    def test_unknown_mode_never_invokes_helper(self):
        private, calls = self.private(reply())
        with self.assertRaises(RuntimeError):
            private.backing_check("evict-after-read")
        self.assertEqual(calls, [])

    def test_sync_and_observation_commands_are_distinct(self):
        for mode in ("enable", "sync", "check"):
            private, calls = self.private(reply(mode))
            self.assertTrue(private.backing_check(mode)["passed"])
            self.assertIn(f"/private/backing-probe {mode} /dev/block/zram1 /dev/block/loop13", calls[0])

    def test_old_buffered_comparison_refuses(self):
        old = {"passed": True, "runtime_test_performed": True, "backend": "native",
               "main_zram_configuration_unchanged": True, "same_boot_after_test": True}
        with self.assertRaisesRegex(RuntimeError, "do not mix"):
            compare(old, {})

    def test_comparison_requires_round_cache_evidence(self):
        data = {"passed": True, "runtime_test_performed": True, "backend": "native",
                "main_zram_configuration_unchanged": True, "same_boot_after_test": True,
                "storage_read_protocol": STORAGE_READ_PROTOCOL, "rounds": [{"passed": True, "checks": {}}]}
        with self.assertRaisesRegex(RuntimeError, "Every compared round"):
            compare(data, {})

    def test_comparison_accepts_matching_new_protocol_fixture(self):
        native = {"passed": True, "runtime_test_performed": True, "backend": "native", "codec": "lz4kd",
            "main_zram_configuration_unchanged": True, "same_boot_after_test": True, "pattern_schema": SCHEMA,
            "rounds": [{"seed": 123, "passed": True, "logical_input_bytes": INPUT_BYTES,
                "fill": {"fnv1a64": "fixture-only"},
                "checks": {"owned_backing_file_synced_and_evicted": True,
                    "direct_io_and_zero_file_cache_before_and_after_read": True},
                "measurements": {"private_loop_write_bytes_including_internal_io": 4096}}]}
        crystal_fixture = copy.deepcopy(native)
        crystal_fixture["backend"] = "crystal"
        self.assertEqual(compare(native, crystal_fixture)["storage_read_protocol"], STORAGE_READ_PROTOCOL)

    def test_read_sequence_observes_after_read_without_eviction(self):
        events = []
        private, _ = self.private(reply())
        private.backing_metadata, private.backing_label = "root0600", "ksu_file"
        private.backing_direct_io_enabled = reply("enable")
        private.sysfs = "/sys/block/zram1"
        snapshots = iter([
            {"bd_stat": [0, 0, 0], "mm_stat": [0, 1], "io_stat": [0, 0], "loop": {"read_sectors": 0, "write_sectors": 0, "flush_ios": None}},
            {"bd_stat": [0, 0, 0], "mm_stat": [0, 1], "io_stat": [0, 0], "loop": {"read_sectors": 0, "write_sectors": 0, "flush_ios": None}},
            {"bd_stat": [1, 0, 1], "mm_stat": [0, 1], "io_stat": [0, 0], "loop": {"read_sectors": 0, "write_sectors": 8, "flush_ios": None}},
            {"bd_stat": [1, 0, 1], "mm_stat": [0, 1], "io_stat": [0, 0], "loop": {"read_sectors": 0, "write_sectors": 8, "flush_ios": None}},
            {"bd_stat": [1, 1, 1], "mm_stat": [0, 1], "io_stat": [0, 0], "loop": {"read_sectors": 8, "write_sectors": 8, "flush_ios": None}},
        ])
        private.snapshot = lambda: next(snapshots)
        def root(code, **kwargs):
            if code.startswith("/private/backing-probe"):
                mode = code.split()[1]
                events.append("backing-" + mode)
                return json.dumps(reply(mode))
            if " verify " in code:
                events.append("verify")
            return json.dumps({"passed": True, "bytes": INPUT_BYTES, "pattern_schema": SCHEMA, "fnv1a64": "same"})
        private.root = root
        entry = measure_round(private, "/probe", 123, {})
        self.assertTrue(entry["passed"])
        self.assertEqual(events, ["backing-sync", "backing-check", "verify", "backing-check"])


def compiled_path_cases(library):
    dll = ctypes.CDLL(str(library.resolve()))
    fn = dll.backing_path_valid
    fn.argtypes, fn.restype = [ctypes.c_char_p], ctypes.c_int
    good = "/data/adb/ace6ct-" + "a" * 20 + "/round-0.img"
    cases = {good: 1, good.replace("ace6ct", "ace6lc"): 1,
        "/data/adb/ace6-kernel-efficiency/zms-backing.img": 0,
        good.replace("round-0", "round-10"): 0, good + ".foreign": 0,
        good.replace("a" * 20, "b" * 19): 0, good.replace("a" * 20, "A" * 20): 0,
        good.replace("/round-", "/../round-"): 0, "/dev/block/zram0": 0, "": 0}
    for i in range(len(good)):
        cases.setdefault(good[:i], 0)
    for path, expected in cases.items():
        if fn(path.encode()) != expected:
            raise RuntimeError("Actual C backing path admission mismatch: " + repr(path))
    return len(cases)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    count = compiled_path_cases(args.library)
    result = unittest.TextTestRunner(verbosity=2).run(unittest.defaultTestLoader.loadTestsFromTestCase(BackingGuards))
    report = {"passed": result.wasSuccessful(), "host_only": True, "device_used": False,
              "actual_c_path_cases": count, "python_refusal_and_sequence_tests": result.testsRun}
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf8")
    raise SystemExit(0 if report["passed"] else 1)
