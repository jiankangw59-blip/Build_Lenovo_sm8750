"""Apply the pinned integrity patch and execute its exact pure C helpers.

This is a host check, not a substitute for booting the 32 kernel KUnit cases.
The page-hash source is injected to isolate error propagation from jhash.
"""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import random
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
TARGET = Path("drivers/crystalfrostwork/crystal_hybridswap/sddc/crystal_sddc.c")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def function(source, name):
    start = re.search(r"^static (?:bool|int) " + name + r"\(", source, re.M)
    if not start:
        raise AssertionError("Function not found: " + name)
    brace = source.index("{", start.start())
    level = 1
    end = brace + 1
    while level:
        level += (source[end] == "{") - (source[end] == "}")
        end += 1
    return source[start.start():end] + "\n"


PREFIX = r"""
typedef unsigned int u32;
typedef unsigned long long u64;
typedef unsigned long long size_t;
typedef _Bool bool;
#define true 1
#define false 0
#define PAGE_SIZE 4096U
#define EIO 5
#define CHS_LOG_ERR 0
#define chs_log_ratelimited(...) ((void)0)
enum crystal_sddc_kind { CRYSTAL_SDDC_REF, CRYSTAL_SDDC_ALIAS, CRYSTAL_SDDC_DELTA };
struct crystal_sddc_ref { struct { u32 id, generation; } cookie; u32 refs; };
struct crystal_sddc { struct { u64 integrity_checks, integrity_failures,
    integrity_skipped; } stats; };
static void atomic64_inc(u64 *value) { ++*value; }
static u32 injected_actual_hash;
static u32 crystal_sddc_page_hash(const void *page) {
    (void)page;
    return injected_actual_hash;
}
static int memcmp(const void *left, const void *right, size_t length) {
    const unsigned char *a = left, *b = right;
    while (length--) { if (*a != *b) return *a < *b ? -1 : 1; ++a; ++b; }
    return 0;
}
#if defined(_WIN32)
#define EXPORT __declspec(dllexport)
#else
#define EXPORT __attribute__((visibility("default")))
#endif
"""

SUFFIX = r"""
EXPORT int test_proof(int ret, u32 size, const void *proof,
                     const void *original, u32 original_size) {
    return crystal_sddc_delta_proof_valid(ret, size, proof, original, original_size);
}
EXPORT int test_hash(u32 expected, u32 actual, int null_page,
                     int null_manager, u64 *counters) {
    struct crystal_sddc sddc = {0};
    unsigned char page = 0;
    int ret;
    injected_actual_hash = actual;
    ret = crystal_sddc_check_page_hash(null_manager ? 0 : &sddc, 0, 0, 0,
        CRYSTAL_SDDC_REF, PAGE_SIZE, PAGE_SIZE, 0, expected,
        null_page ? 0 : &page, "host-injected-hash");
    counters[0] = sddc.stats.integrity_checks;
    counters[1] = sddc.stats.integrity_failures;
    counters[2] = sddc.stats.integrity_skipped;
    return ret;
}
"""


def run(args):
    metadata = json.loads((HERE / "sddc-integrity.json").read_text(encoding="utf-8"))
    patch = HERE.parent / "sddc-integrity.patch"
    original = args.upstream / TARGET
    assert sha(original) == metadata["source_sha256"], "Unreviewed upstream"
    assert sha(patch) == metadata["patch_sha256"], "Unreviewed patch"
    args.output.mkdir(parents=True, exist_ok=False)
    fixture = args.output / "fixture"
    destination = fixture / TARGET
    destination.parent.mkdir(parents=True)
    shutil.copyfile(original, destination)
    patch_command = [str(args.patch), "-p1", "--fuzz=0", "--batch", "--forward",
                     "-i", str(patch.resolve())]
    result = subprocess.run(patch_command, cwd=fixture, capture_output=True)
    (args.output / "apply.stdout.txt").write_bytes(result.stdout)
    (args.output / "apply.stderr.txt").write_bytes(result.stderr)
    assert result.returncode == 0, result.stderr.decode("utf-8", "replace")
    assert sha(destination) == metadata["patched_sha256"], "Unexpected patched bytes"
    source = destination.read_text(encoding="utf-8")
    # All reviewed full-page checks must propagate the checker result.
    checks = re.findall(r"(?m)^\s*(.*?)crystal_sddc_check_page_hash\(", source)
    assert checks == ["static int ", "ret = ", "ret = ", "ret = "], checks
    cases = re.findall(r"^\s*KUNIT_CASE\(", source, re.M)
    assert len(cases) == metadata["expected_state_cases"] == 26
    helpers = "\n".join(function(source, name) for name in (
        "crystal_sddc_delta_proof_valid", "crystal_sddc_check_page_hash"))
    harness = args.output / "extracted_helpers.c"
    harness.write_text(PREFIX + helpers + SUFFIX, encoding="utf-8")
    library = args.output / ("integrity.dll" if os.name == "nt" else "integrity.so")
    command = [str(args.compiler), "-shared", "-O2", "-std=gnu11", "-Wall",
               "-Wextra", "-Werror", "-Wno-unused-parameter", "-ffreestanding",
               "-fno-builtin", "-fno-stack-protector", str(harness)]
    if os.name == "nt":
        command += ["-fuse-ld=lld", "-nostdlib", "-Wl,/NOENTRY", "-Wl,/NODEFAULTLIB"]
    else:
        command += ["-fPIC", "-Wl,--no-undefined"]
    command += ["-o", str(library)]
    built = subprocess.run(command, capture_output=True)
    (args.output / "build.stdout.txt").write_bytes(built.stdout)
    (args.output / "build.stderr.txt").write_bytes(built.stderr)
    assert built.returncode == 0, built.stderr.decode("utf-8", "replace")
    dll = ctypes.CDLL(str(library.resolve()))
    proof = dll.test_proof
    proof.argtypes = [ctypes.c_int, ctypes.c_uint32, ctypes.c_void_p,
                      ctypes.c_void_p, ctypes.c_uint32]
    proof.restype = ctypes.c_int
    hash_check = dll.test_hash
    hash_check.argtypes = [ctypes.c_uint32, ctypes.c_uint32, ctypes.c_int,
                          ctypes.c_int, ctypes.POINTER(ctypes.c_uint64)]
    hash_check.restype = ctypes.c_int
    rng = random.Random(0x53444443)
    proof_calls = 0
    # Exact production predicate: identical data passes, every flipped byte fails.
    for length in (1, 8, 64, 1024, 4096):
        value = bytes(rng.getrandbits(8) for _ in range(length))
        original_buffer = ctypes.create_string_buffer(value, length)
        candidate = ctypes.create_string_buffer(value, length)
        assert proof(0, length, candidate, original_buffer, length) == 1
        proof_calls += 1
        for offset in range(length):
            candidate[offset] = bytes([value[offset] ^ 1])
            assert proof(0, length, candidate, original_buffer, length) == 0
            candidate[offset] = bytes([value[offset]])
            proof_calls += 1
        for ret, reported, left, right, requested in (
            (-5, length, candidate, original_buffer, length),
            (1, length, candidate, original_buffer, length),
            (0, length - 1, candidate, original_buffer, length),
            (0, length + 1, candidate, original_buffer, length),
            (0, length, None, original_buffer, length),
            (0, length, candidate, None, length),
            (0, 0, candidate, original_buffer, 0),
            (0, 4097, candidate, original_buffer, 4097),
        ):
            assert proof(ret, reported, left, right, requested) == 0
            proof_calls += 1
    counters = (ctypes.c_uint64 * 3)()
    hash_calls = 0
    for expected, actual, null_page, null_manager, ret, counts in (
        (1, 1, 0, 0, 0, (1, 0, 0)),
        (1, 2, 0, 0, -5, (1, 1, 0)),
        (0xFFFFFFFF, 0xFFFFFFFE, 0, 0, -5, (1, 1, 0)),
        (0, 7, 0, 0, 0, (0, 0, 1)),
        (1, 1, 1, 0, -5, (0, 0, 0)),
        (1, 1, 0, 1, -5, (0, 0, 0)),
    ):
        assert hash_check(expected, actual, null_page, null_manager, counters) == ret
        assert tuple(counters) == counts
        hash_calls += 1
    report = {
        "passed": True, "patch_applied_fuzz": 0,
        "source_sha256": sha(original), "patch_sha256": sha(patch),
        "patched_sha256": sha(destination), "helper_c_sha256": sha(harness),
        "library_sha256": sha(library), "compile_command": command,
        "proof_assertions": proof_calls, "hash_branch_assertions": hash_calls,
        "all_three_readers_return_checker_result": True,
        "state_kunit_cases_present": len(cases),
        "hash_function_mocked": True, "kernel_kunit_executed": False,
        "kernel_compilation_verified": False, "android_device_used": False,
        "confirmed_wrong_page_root_cause": False,
    }
    (args.output / "host-checks.json").write_text(
        json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--compiler", type=Path, default=Path(
        shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe"))
    parser.add_argument("--patch", type=Path, default=Path(
        shutil.which("patch") or "patch"))
    run(parser.parse_args())
