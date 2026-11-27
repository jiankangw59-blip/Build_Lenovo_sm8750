"""Host-only refusal, real-C pagemap decoding and 4 MiB payload checks."""
import argparse
import ctypes
import hashlib
import json
import re
from pathlib import Path
import unittest

from run_owned_probe import preflight_owned


class OwnedFixture:
    def __init__(self, overrides=None):
        self.calls = []
        self.notes = bytes(range(84))
        self.notes_hash = hashlib.sha256(self.notes).hexdigest()
        self.owner = "/data/adb/ace6-kernel-efficiency/zram-backing.img"
        self.values = {
            "getprop ro.product.model": "PLQ110",
            "sha256sum /sys/kernel/notes": self.notes_hash + "  /sys/kernel/notes",
            "od -An -v -t x1 /sys/kernel/notes": self.notes.hex(" "),
            "test -r /sys/block/zram0/zms_stat && echo yes || echo no": "no",
            "test -r /sys/block/zram0/hybridswap_crystal_stat && echo yes || echo no": "no",
            "cat /sys/block/zram0/comp_algorithm": "lzo lz4 [lz4kd]",
            "awk 'NR>1 {print $1}' /proc/swaps": "/dev/block/zram0",
            "cat /sys/block/zram0/backing_dev": "/dev/block/loop49",
            "cat /sys/block/loop49/loop/backing_file": self.owner,
            f"test -f {self.owner} -a ! -L {self.owner} && echo yes || echo no": "yes",
            f"stat -c '%u:%g %a %s' {self.owner}": "0:0 600 8589934592",
            f"ls -Zd {self.owner}": "u:object_r:ksu_file:s0 " + self.owner,
            "cat /sys/block/zram0/writeback_limit_enable": "1",
            "cat /sys/block/zram0/writeback_limit": "1000",
            "awk '$1==\"MemAvailable:\" {print $2}' /proc/meminfo": "4194304",
            "cat /proc/sys/kernel/random/boot_id": "host-fixture-only",
            "cat /sys/block/zram0/disksize": "17179869184",
        }
        self.values.update(overrides or {})

    def __call__(self, code):
        self.calls.append(code)
        if code not in self.values:
            raise AssertionError("Unexpected command in host-only fixture: " + code)
        return self.values[code]

    def check(self, **kwargs):
        return preflight_owned(self, [self.notes_hash], "native", "lz4kd", self.owner, 0, **kwargs)


class OwnedSafetyTests(unittest.TestCase):
    def test_wrong_kernel_refuses_before_staging(self):
        root = OwnedFixture()
        with self.assertRaisesRegex(RuntimeError, "Unexpected kernel"):
            preflight_owned(root, ["0" * 64], "native", "lz4kd", root.owner, 0)
        self.assertEqual(len(root.calls), 2)

    def test_raw_notes_are_independently_hashed(self):
        root = OwnedFixture({"od -An -v -t x1 /sys/kernel/notes": "00"})
        with self.assertRaisesRegex(RuntimeError, "dump/hash"):
            root.check()

    def test_no_other_swap_device_allowed(self):
        root = OwnedFixture({"awk 'NR>1 {print $1}' /proc/swaps": "/dev/block/zram0\n/dev/block/zram1"})
        with self.assertRaisesRegex(RuntimeError, "sole active"):
            root.check()

    def test_foreign_backing_refuses(self):
        root = OwnedFixture({"cat /sys/block/loop49/loop/backing_file": "/data/foreign.img"})
        with self.assertRaisesRegex(RuntimeError, "backing owner"):
            root.check()

    def test_vendor_partition_refuses(self):
        root = OwnedFixture({"cat /sys/block/zram0/backing_dev": "/dev/block/mapper/hybridswap_crypto"})
        with self.assertRaisesRegex(RuntimeError, "private loop"):
            root.check()

    def test_insufficient_budget_is_not_refilled(self):
        root = OwnedFixture({"cat /sys/block/zram0/writeback_limit": "63"})
        with self.assertRaisesRegex(RuntimeError, "insufficient"):
            root.check()
        self.assertFalse(any(re.search(r">\s*/", command) or "push" in command for command in root.calls))

    def test_postcheck_allows_consumed_budget_without_refill(self):
        root = OwnedFixture({"cat /sys/block/zram0/writeback_limit": "0"})
        data = root.check(minimum_budget=0)
        self.assertEqual(data["existing_limit_logical_slots"], 0)
        self.assertFalse(any(re.search(r">\s*/", command) or "push" in command for command in root.calls))


def python_pairs(page, seed):
    mask = (1 << 64) - 1
    state = seed ^ (((page // 8 + 1) * 0x9E3779B97F4A7C15) & mask)
    out = bytearray(2048)
    for offset in range(0, 2048, 8):
        state ^= (state << 13) & mask
        state ^= state >> 7
        state ^= (state << 17) & mask
        out[offset:offset + 8] = state.to_bytes(8, "little")
    variant = (page % 8) // 2
    if variant:
        for edit in range(4):
            out[(variant * 149 + edit * 67) % 2048] ^= 0x31 + variant * 17 + edit
    return bytes(out + out)


def actual_c_checks(library):
    dll = ctypes.CDLL(str(library.resolve()))
    dll.owned_entry_offset.argtypes = [ctypes.c_uint64, ctypes.c_uint64]
    dll.owned_entry_offset.restype = ctypes.c_uint64
    dll.owned_pattern_checksum.argtypes = [ctypes.c_uint64]
    dll.owned_pattern_checksum.restype = ctypes.c_uint64
    cases = [
        ((1 << 62) | (1234 << 5) | 0, 4096, 1234),
        ((1 << 62) | (55 << 5) | 31 | (1 << 55), 4096, 55),
        ((1 << 62), 4096, 0),
        ((1 << 62) | (4096 << 5), 4096, 0),
        ((1 << 62) | (1 << 63) | (123 << 5), 4096, 0),
        ((1 << 63) | (123 << 5), 4096, 0),
    ]
    for entry, slots, expected in cases:
        if dll.owned_entry_offset(entry, slots) != expected:
            raise RuntimeError("Real C pagemap decoding/guard mismatch")
    patterns = []
    for seed in (0x1234ABCD, 0x78EF019B):
        fnv = 14695981039346656037
        sha = hashlib.sha256()
        for page in range(1024):
            data = python_pairs(page, seed)
            sha.update(data)
            for byte in data:
                fnv = ((fnv ^ byte) * 1099511628211) & ((1 << 64) - 1)
        if dll.owned_pattern_checksum(seed) != fnv:
            raise RuntimeError("Real C owned 4 MiB pattern checksum mismatch")
        patterns.append({"seed": seed, "bytes_verified": 4194304,
                         "fnv1a64": f"{fnv:016x}", "sha256": sha.hexdigest()})
    return {"actual_c_pagemap_cases": len(cases), "actual_c_4mib_patterns": patterns}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    suite = unittest.defaultTestLoader.loadTestsFromTestCase(OwnedSafetyTests)
    tests = unittest.TextTestRunner(verbosity=2).run(suite)
    if not tests.wasSuccessful():
        raise SystemExit(1)
    report = {"passed": True, "host_only": True, "android_device_used": False,
              "kernel_runtime_verified": False, "host_refusal_contract_tests": tests.testsRun,
              "library_sha256": hashlib.sha256(args.library.read_bytes()).hexdigest(),
              **actual_c_checks(args.library)}
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
