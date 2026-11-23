"""Windows guard-page checks for codec reads/writes at exact buffer ends."""
import argparse
import base64
import ctypes as C
import hashlib
import json
import os
from pathlib import Path
import random

from test_codec import configure, P, U, PAGE


class Protected:
    def __init__(self, data=None, capacity=PAGE):
        if not 0 <= capacity <= PAGE:
            raise ValueError("one accessible page only")
        self.capacity = capacity
        self.k = C.WinDLL("kernel32", use_last_error=True)
        self.k.VirtualAlloc.argtypes, self.k.VirtualAlloc.restype = [P, C.c_size_t, U, U], P
        self.k.VirtualProtect.argtypes, self.k.VirtualProtect.restype = [P, C.c_size_t, U, C.POINTER(U)], C.c_int
        self.k.VirtualFree.argtypes, self.k.VirtualFree.restype = [P, C.c_size_t, U], C.c_int
        self.base = self.k.VirtualAlloc(None, PAGE * 3, 0x3000, 4)
        if not self.base:
            raise C.WinError(C.get_last_error())
        old = U()
        try:
            for address in [self.base, self.base + 2 * PAGE]:
                if not self.k.VirtualProtect(address, PAGE, 1, C.byref(old)):
                    raise C.WinError(C.get_last_error())
            C.memset(self.base + PAGE, 0xa6, PAGE)
            self.ptr = self.base + 2 * PAGE - capacity
            if data is not None:
                if len(data) != capacity:
                    raise ValueError("data size")
                if data: C.memmove(self.ptr, data, len(data))
        except BaseException:
            self.close()
            raise

    def check(self):
        prefix_size = PAGE - self.capacity
        assert C.string_at(self.base + PAGE, prefix_size) == b"\xa6" * prefix_size

    def close(self):
        if self.base:
            self.k.VirtualFree(self.base, 0, 0x8000)
            self.base = None

    def __enter__(self): return self
    def __exit__(self, *unused): self.close()


def run(args):
    if os.name != "nt": raise RuntimeError("This test uses Windows VirtualProtect")
    lib = C.CDLL(str(args.library.resolve()))
    lib.lab_check_allocation_guards.restype = C.c_ulong
    lib.lab_live_allocations.restype = U
    api = configure(lib, "restored_lz4kds_")
    codec = api["create"]()
    rng = random.Random(0x4755415244504147)
    count = 0
    current = {}
    result = {"passed": False}
    try:
        for i in range(args.cases):
            size = rng.randrange(PAGE + 1)
            raw = rng.randbytes(size)
            refdata = rng.randbytes(PAGE)
            capacity = rng.randrange(1, PAGE + 1)
            current = {"case": i, "mode": "ordinary", "input_b64": base64.b64encode(raw).decode()}
            with Protected(raw, size) as src, Protected(capacity=PAGE) as dst:
                n = U(PAGE)
                assert api["decompress"](codec, src.ptr, size, dst.ptr, C.byref(n)) in (0, -22)
                src.check(); dst.check(); count += 1
            current.update(mode="delta", capacity=capacity, reference_b64=base64.b64encode(refdata).decode())
            with Protected(raw, size) as src, Protected(refdata, PAGE) as ref, Protected(capacity=capacity) as dst:
                n = U(capacity)
                assert api["decompress_delta"](codec, src.ptr, size, ref.ptr, PAGE, dst.ptr, C.byref(n)) in (0, -22)
                src.check(); ref.check(); dst.check(); count += 1
            assert lib.lab_check_allocation_guards() == 0
        # Encoder error paths must also stay inside the actual small destination.
        for capacity in [1, 2, 3, 4, 8, 16, 31, 32, 64, 256, PAGE]:
            current = {"mode": "encoder_small_destination", "capacity": capacity}
            with Protected(b"\x5a" * PAGE, PAGE) as src, Protected(capacity=capacity) as dst:
                n = U(capacity)
                assert api["compress"](codec, src.ptr, PAGE, dst.ptr, C.byref(n)) in (0, -22)
                src.check(); dst.check(); count += 1
        result = {"passed": True, "calls": count, "seed": "0x4755415244504147",
                  "protected_buffers": "inaccessible pages before and after each accessible page; exact input/output end at final guard",
                  "kernel_concurrency_tested": False, "android_device_used": False}
    except BaseException as error:
        result.update(calls=count, reason=repr(error), first_failure=current)
    finally:
        api["destroy"](codec)
        result["live_allocations_after_cleanup"] = lib.lab_live_allocations()
        result["allocation_guard_failures"] = lib.lab_check_allocation_guards()
    result["library_sha256"] = hashlib.sha256(args.library.read_bytes()).hexdigest()
    result["test_script_sha256"] = hashlib.sha256(Path(__file__).read_bytes()).hexdigest()
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "first_failure"}, indent=2))
    if not result["passed"]: raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cases", type=int, default=10000)
    run(parser.parse_args())
