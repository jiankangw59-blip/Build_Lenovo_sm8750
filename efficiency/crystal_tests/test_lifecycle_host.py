"""Host real-C lifecycle patterns/classifier checks; no Android operations."""
import argparse
import ctypes
import hashlib
import json
from pathlib import Path

from test_owned_host import python_pairs


def test(library):
    dll = ctypes.CDLL(str(library.resolve()))
    dll.lifecycle_private_index.argtypes = [ctypes.c_char_p]
    dll.lifecycle_private_index.restype = ctypes.c_uint
    dll.lifecycle_pattern_fill.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint64,
                                           ctypes.c_uint64, ctypes.c_uint]
    dll.lifecycle_pattern_fill.restype = None
    dll.lifecycle_known_version.argtypes = [ctypes.c_void_p, ctypes.c_uint, ctypes.c_uint64, ctypes.c_uint64]
    dll.lifecycle_known_version.restype = ctypes.c_int
    dll.lifecycle_safe_seed.argtypes = [ctypes.c_uint64]
    dll.lifecycle_safe_seed.restype = ctypes.c_int
    parser_cases = {b"/dev/block/zram0": 0, b"/dev/block/zram1": 1, b"/dev/block/zram1023": 1023,
                    b"/dev/block/zram1024": 0, b"/dev/block/zram01": 0, b"/dev/block/zram-1": 0,
                    b"/dev/block/boot_b": 0, b"/dev/zram1": 0, b"/dev/block/zram1x": 0,
                    b"/dev/block/zram": 0, b"/dev/block/zram9999999999999999999999999999999999": 0}
    for value, expected in parser_cases.items():
        if dll.lifecycle_private_index(value) != expected:
            raise RuntimeError("Real C private-device parser guard failed: " + repr(value))
    seed_cases = [(0x1234ABCD, 1), (0x78EF019B, 1), (0x9E3779B97F4A7C15, 0),
                  ((1024 * 0x9E3779B97F4A7C15) & ((1 << 64) - 1), 0)]
    for value, expected in seed_cases:
        if dll.lifecycle_safe_seed(value) != expected:
            raise RuntimeError("Real C zero-page source-seed guard failed")
    old_seed, new_seed = 0x1234ABCD, 0x78EF019B
    out = (ctypes.c_ubyte * 4096)()
    reports = []
    for state, label in [(0, "filled-old"), (1, "first-pair-overwritten"), (2, "first-pair-discarded")]:
        digest = hashlib.sha256()
        old_pages = changed_pages = 0
        for page in range(8192):
            source = page % 8 < 2
            if source and state == 2:
                expected = bytes(4096)
            else:
                expected = python_pairs(page, new_seed if source and state == 1 else old_seed)
            dll.lifecycle_pattern_fill(out, page, old_seed, new_seed, state)
            actual = bytes(out)
            if actual != expected:
                raise RuntimeError(f"Real C lifecycle pattern differs: state={state} page={page}")
            digest.update(actual)
            changed_pages += source and state != 0
            old_pages += not source or state == 0
        reports.append({"state": label, "bytes_verified": 33554432,
                        "old_content_pages": old_pages, "changed_or_zero_pages": changed_pages,
                        "sha256": digest.hexdigest()})
    classifier_cases = 0
    for page in (0, 1, 2, 7, 8, 511, 8191):
        a, b = python_pairs(page, old_seed), python_pairs(page, new_seed)
        choices = [(a, 0), (b, 1), (bytes(4096), -1), (a[:2048] + b[2048:], -1)]
        damaged = bytearray(a)
        damaged[73] ^= 1
        choices.append((bytes(damaged), -1))
        for value, expected in choices:
            ctypes.memmove(out, value, 4096)
            if dll.lifecycle_known_version(out, page, old_seed, new_seed) != expected:
                raise RuntimeError("Real C complete-A/B classifier accepted mixed/zero/damaged data")
            classifier_cases += 1
    ctypes.memset(out, 0, 4096)
    if dll.lifecycle_known_version(out, 0, 0x9E3779B97F4A7C15, new_seed) != -1:
        raise RuntimeError("Zero page must fail even when an unsafe seed could otherwise describe it")
    classifier_cases += 1
    return {"passed": True, "host_only": True, "android_device_used": False,
            "kernel_runtime_verified": False, "kernel_page_atomicity_verified": False,
            "private_parser_cases": len(parser_cases), "complete_a_b_classifier_cases": classifier_cases,
            "zero_source_seed_guard_cases": len(seed_cases),
            "full_32mib_lifecycle_states": reports,
            "old_seed": old_seed, "new_seed": new_seed,
            "compiled_library_sha256": hashlib.sha256(library.read_bytes()).hexdigest()}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = test(args.library)
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
