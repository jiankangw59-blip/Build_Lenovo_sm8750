"""Host-only pattern and refusal-path tests. These never validate a kernel."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path
import unittest

from run_probe import PrivateRound, loop_stats, named_stats, preflight, SEEDS, INPUT_BYTES


class FakeReadOnlyRoot:
    def __init__(self, overrides=None):
        self.calls = []
        self.values = {
            "getprop ro.product.model": "PLQ110",
            "sha256sum /sys/kernel/notes": "a" * 64 + "  /sys/kernel/notes",
            "test -r /sys/block/zram0/zms_stat && echo yes || echo no": "no",
            "test -r /sys/block/zram0/hybridswap_crystal_stat && echo yes || echo no": "no",
            "cat /sys/block/zram0/comp_algorithm": "lzo lz4 [lz4kd]",
        }
        self.values.update(overrides or {})

    def __call__(self, code):
        self.calls.append(code)
        if code not in self.values:
            raise AssertionError("Unexpected command in refusal-path test: " + code)
        return self.values[code]


class SafetyTests(unittest.TestCase):
    def test_wrong_kernel_stops_before_any_hot_add(self):
        root = FakeReadOnlyRoot()
        with self.assertRaisesRegex(RuntimeError, "allowlist"):
            preflight(root, ["b" * 64], "crystal", "lz4kd", "lz4kd", 0)
        self.assertEqual(len(root.calls), 2)

    def test_native_cannot_satisfy_crystal_marker(self):
        root = FakeReadOnlyRoot()
        with self.assertRaisesRegex(RuntimeError, "markers missing"):
            preflight(root, ["a" * 64], "crystal", "lz4kd", "lz4kd", 0)
        self.assertFalse(any("hot_add" in call for call in root.calls))

    def test_main_codec_is_independent_of_private_requested_codec(self):
        root = FakeReadOnlyRoot()
        with self.assertRaisesRegex(RuntimeError, "not compiled"):
            preflight(root, ["a" * 64], "native", "lz4kds", "lz4kd", 0)
        self.assertIn("cat /sys/block/zram0/comp_algorithm", root.calls)

    def test_mismatched_main_codec_refuses(self):
        root = FakeReadOnlyRoot()
        with self.assertRaisesRegex(RuntimeError, "protected baseline"):
            preflight(root, ["a" * 64], "native", "lz4kd", "lz4kds", 0)

    def test_zram0_cleanup_never_executes(self):
        root = FakeReadOnlyRoot()
        private = PrivateRound(root, "/private-test", 0, "crystal", "lz4kd", 0)
        private.index = 0
        with self.assertRaisesRegex(RuntimeError, "safe private"):
            private.cleanup()
        self.assertEqual(root.calls, [])

    def test_too_long_loop_path_refuses_before_creating_device(self):
        root = FakeReadOnlyRoot()
        private = PrivateRound(root, "/private-" + "a" * 64, 0, "native", "lz4kd", 0)
        with self.assertRaisesRegex(RuntimeError, "shorter than 64"):
            private.prepare()
        self.assertEqual(root.calls, [])

    def test_foreign_backing_is_preserved(self):
        root = FakeReadOnlyRoot({"stat -Lc %i /sys/block/zram1": "123",
            "cat /sys/block/zram1/dev": "254:1",
            "for d in $(awk 'NR>1 {print $1}' /proc/swaps); do stat -Lc '%t:%T' \"$d\"; done; true": "fe:0",
            "cat /sys/block/zram1/backing_dev": "/dev/block/loop999"})
        private = PrivateRound(root, "/private-test", 0, "crystal", "lz4kd", 0)
        private.index, private.inode, private.identity = 1, "123", "254:1"
        private.sysfs = "/sys/block/zram1"
        with self.assertRaisesRegex(RuntimeError, "Foreign backing"):
            private.cleanup()
        self.assertFalse(any("reset" in call or "hot_remove" in call or "rm -f" in call for call in root.calls))

    def test_active_swap_alias_is_preserved(self):
        root = FakeReadOnlyRoot({"stat -Lc %i /sys/block/zram1": "123",
            "cat /sys/block/zram1/dev": "254:1",
            "for d in $(awk 'NR>1 {print $1}' /proc/swaps); do stat -Lc '%t:%T' \"$d\"; done; true": "fe:1"})
        private = PrivateRound(root, "/private-test", 0, "native", "lz4kd", 0)
        private.index, private.inode, private.identity = 1, "123", "254:1"
        private.sysfs = "/sys/block/zram1"
        with self.assertRaisesRegex(RuntimeError, "active swap"):
            private.cleanup()

    def test_zms_field_parser_rejects_duplicates(self):
        with self.assertRaisesRegex(RuntimeError, "Duplicate"):
            named_stats("dirty_pages: 0\ndirty_pages: 1")

    def test_loop_sectors_are_512_bytes_not_pages(self):
        self.assertEqual(loop_stats("1 0 8 1 2 0 16 1 0 0 0")["write_sectors"], 16)


def python_pattern(page, seed):
    mask = (1 << 64) - 1
    state = seed ^ (((page + 1) * 0x9E3779B97F4A7C15) & mask)
    mode = page % 4
    if mode == 0:
        return bytes(4096)
    if mode == 1:
        return bytes([(page + seed) % 251 + 1]) * 4096
    out = bytearray(4096)
    for offset in range(0, 4096, 8 if mode == 2 else 128):
        state ^= (state << 13) & mask
        state ^= state >> 7
        state ^= (state << 17) & mask
        out[offset:offset + 8] = state.to_bytes(8, "little")
    return out


def check_patterns(library):
    dll = ctypes.CDLL(str(library.resolve()))
    dll.pattern_fill.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint64]
    dll.pattern_fill.restype = None
    dll.pattern_checksum.argtypes = [ctypes.c_uint64]
    dll.pattern_checksum.restype = ctypes.c_uint64
    dll.pattern_pairs_fill.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint64]
    dll.pattern_pairs_fill.restype = None
    out = (ctypes.c_ubyte * 4096)()
    reports = []
    for seed in SEEDS:
        digest = hashlib.sha256()
        fnv = 14695981039346656037
        for page in range(8192):
            expected = python_pattern(page, seed)
            dll.pattern_fill(out, page, seed)
            actual = bytes(out)
            if actual != expected:
                raise RuntimeError(f"Real C/Python pattern mismatch seed={seed} page={page}")
            digest.update(actual)
            for byte in actual:
                fnv = ((fnv ^ byte) * 1099511628211) & ((1 << 64) - 1)
        if dll.pattern_checksum(seed) != fnv:
            raise RuntimeError("Real C/Python full-stream FNV mismatch")
        reports.append({"seed": seed, "bytes_verified": INPUT_BYTES,
                        "sha256": digest.hexdigest(), "fnv1a64": f"{fnv:016x}"})
    pair_reports = []
    for seed in SEEDS:
        digest = hashlib.sha256()
        previous = None
        for page in range(8192):
            state = seed ^ (((page // 8 + 1) * 0x9E3779B97F4A7C15) & ((1 << 64) - 1))
            block = bytearray(2048)
            for offset in range(0, 2048, 8):
                state ^= (state << 13) & ((1 << 64) - 1)
                state ^= state >> 7
                state ^= (state << 17) & ((1 << 64) - 1)
                block[offset:offset + 8] = state.to_bytes(8, "little")
            variant = (page % 8) // 2
            if variant:
                for edit in range(4):
                    position = (variant * 149 + edit * 67) % 2048
                    block[position] ^= 0x31 + variant * 17 + edit
            expected = bytes(block + block)
            dll.pattern_pairs_fill(out, page, seed)
            actual = bytes(out)
            if actual != expected or actual[:2048] != actual[2048:]:
                raise RuntimeError(f"SDDC pair pattern mismatch page={page}")
            if page % 2 and actual != previous:
                raise RuntimeError("The exact-alias page pair differs")
            previous = actual
            digest.update(actual)
        pair_reports.append({"seed": seed, "bytes_verified": INPUT_BYTES,
                             "pattern_schema": "sddc-pairs-2k-v1", "sha256": digest.hexdigest()})
    return {"mixed": reports, "sddc_pairs": pair_reports}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(SafetyTests)
    result = unittest.TextTestRunner(verbosity=2).run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
    report = {"passed": True, "host_only": True, "android_device_used": False,
              "kernel_runtime_verified": False, "refusal_tests": result.testsRun,
              "real_compiled_c_patterns": check_patterns(args.library)}
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
