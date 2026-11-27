"""Exercise real C codecs with known data, malformed streams and heap guards."""
import argparse
import base64
import ctypes as C
import hashlib
import json
import random
from pathlib import Path

PAGE = 4096
U = C.c_uint
P = C.c_void_p


class Buffer:
    """Canaries catch writes outside the actual output capacity."""
    def __init__(self, length, data=b""):
        self.length = length
        self.storage = C.create_string_buffer(b"\xa6" * (length + 128), length + 128)
        self.ptr = C.addressof(self.storage) + 64
        if data:
            if len(data) > length:
                raise ValueError("input exceeds allocation")
            C.memmove(self.ptr, data, len(data))

    def bytes(self, n=None):
        return C.string_at(self.ptr, self.length if n is None else n)

    def check(self):
        assert self.storage.raw[:64] == b"\xa6" * 64
        assert self.storage.raw[64 + self.length:] == b"\xa6" * 64


def configure(lib, prefix):
    names = {
        "create": ([], P), "destroy": ([P], None),
        "compress": ([P, P, U, P, C.POINTER(U)], C.c_int),
        "decompress": ([P, P, U, P, C.POINTER(U)], C.c_int),
        "compress_delta": ([P, P, U, P, U, P, C.POINTER(U), U], C.c_int),
        "decompress_delta": ([P, P, U, P, U, P, C.POINTER(U)], C.c_int),
        "decompress_delta_borrowed": ([P, P, U, P, U, C.POINTER(P), C.POINTER(U)], C.c_int),
    }
    result = {}
    for name, (arguments, returned) in names.items():
        fn = getattr(lib, prefix + name)
        fn.argtypes, fn.restype = arguments, returned
        result[name] = fn
    return result


def page(rng, mode):
    if mode == 0: return b"\0" * PAGE
    if mode == 1: return bytes([rng.randrange(256)]) * PAGE
    if mode == 2: return rng.randbytes(256) * 16
    if mode == 3: return rng.randbytes(2048) * 2
    if mode == 4: return rng.randbytes(128) + b"\0" * (PAGE - 128)
    if mode == 5: return bytes(range(256)) * 16
    if mode == 6: return rng.randbytes(PAGE)
    return b"".join((i ^ rng.randrange(8)).to_bytes(4, "little") for i in range(PAGE // 4))


def ordinary(api, codec, data):
    src, dst = Buffer(PAGE, data), Buffer(2 * PAGE)
    n = U(dst.length)
    assert api["compress"](codec, src.ptr, PAGE, dst.ptr, C.byref(n)) == 0
    src.check(); dst.check(); assert src.bytes() == data
    if n.value == 2 * PAGE:
        return None  # An explicit raw fallback, never a compressed zero page.
    assert 0 < n.value < PAGE
    encoded = dst.bytes(n.value)
    restored = Buffer(PAGE)
    size = U(PAGE)
    assert api["decompress"](codec, dst.ptr, n.value, restored.ptr, C.byref(size)) == 0
    assert size.value == PAGE and restored.bytes() == data
    dst.check(); restored.check()
    return encoded


def delta(api, codec, reference, current, ceiling=PAGE):
    ref, src, dst = Buffer(len(reference), reference), Buffer(len(current), current), Buffer(2 * PAGE)
    n = U(dst.length)
    assert api["compress_delta"](codec, ref.ptr, len(reference), src.ptr, len(current),
                                  dst.ptr, C.byref(n), 0) == 0
    ref.check(); src.check(); dst.check()
    assert ref.bytes() == reference and src.bytes() == current
    if n.value == 2 * PAGE:
        return None
    assert 0 < n.value <= PAGE
    size = U(ceiling)
    restored = Buffer(ceiling)
    ret = api["decompress_delta"](codec, dst.ptr, n.value, ref.ptr, len(reference),
                                    restored.ptr, C.byref(size))
    assert ret == 0, (ret, len(reference), len(current), n.value, ceiling)
    assert size.value == len(current) and restored.bytes(size.value) == current
    restored.check()
    borrowed, size = P(), U(ceiling)
    assert api["decompress_delta_borrowed"](codec, dst.ptr, n.value, ref.ptr, len(reference),
                                           C.byref(borrowed), C.byref(size)) == 0
    assert size.value == len(current) and C.string_at(borrowed, size.value) == current
    return dst.bytes(n.value)


def run(args):
    rng = random.Random(0x4C5A344B4453)
    lib = C.CDLL(str(args.library.resolve()))
    lib.lab_check_allocation_guards.restype = C.c_ulong
    lib.lab_live_allocations.restype = U
    lib.lab_set_fail_after.argtypes = [C.c_int]
    api = configure(lib, "restored_lz4kds_")
    upstream = configure(lib, "crystal_sddc_codec_")
    counts = {"ordinary_roundtrips": 0, "ordinary_raw_fallbacks": 0,
              "delta_roundtrips": 0, "delta_raw_fallbacks": 0,
              "compressed_stream_delta_chains": 0, "malformed_decode_calls": 0,
              "malformed_rejected": 0, "malformed_accepted": 0,
              "allocation_failure_cases": 0, "invalid_argument_cases": 0,
              "truncated_stream_rejections": 0, "ko_contract_differences": 0,
              "identical_and_zero_delta_cases": 0}
    assert lib.lab_live_allocations() == 0
    for interface in [api, upstream]:
        for fail_at in range(3):
            lib.lab_set_fail_after(fail_at)
            error = interface["create"]()
            assert error >= 2**64 - 4095
            assert lib.lab_live_allocations() == 0
            counts["allocation_failure_cases"] += 1
    lib.lab_set_fail_after(-1)
    codec, original = api["create"](), upstream["create"]()
    assert codec and original and lib.lab_live_allocations() == 6
    streams = []
    accepted_samples = []
    try:
        for reference in [b"\0" * PAGE, bytes([0x5a]) * PAGE, rng.randbytes(PAGE)]:
            assert delta(api, codec, reference, reference) is not None
            assert delta(upstream, original, reference, reference) is not None
            counts["identical_and_zero_delta_cases"] += 1
        for i in range(args.rounds):
            data = page(rng, i % 8)
            encoded = ordinary(api, codec, data)
            comparison = ordinary(upstream, original, data)
            assert encoded == comparison
            counts["ordinary_raw_fallbacks" if encoded is None else "ordinary_roundtrips"] += 1
            if encoded is not None and len(streams) < 24: streams.append(encoded)
            reference = rng.randbytes(PAGE)
            current = bytearray(reference)
            for unused in range(1 + i % 16): current[rng.randrange(PAGE)] ^= rng.randrange(1, 256)
            encoded_delta = delta(api, codec, reference, bytes(current))
            assert encoded_delta == delta(upstream, original, reference, bytes(current))
            counts["delta_raw_fallbacks" if encoded_delta is None else "delta_roundtrips"] += 1
            # LZ4KDS deltas ordinarily operate on compressed streams, not raw pages.
            ref_page = rng.randbytes(PAGE // 2) * 2
            cur_page = bytearray(ref_page)
            cur_page[127] ^= 0x55; cur_page[PAGE // 2 + 127] ^= 0x55
            ref_stream = ordinary(api, codec, ref_page)
            cur_stream = ordinary(api, codec, bytes(cur_page))
            assert ref_stream and cur_stream
            stream_delta = delta(api, codec, ref_stream, cur_stream)
            if stream_delta is not None:
                counts["compressed_stream_delta_chains"] += 1
                # Decode the actual current encoded stream back into a known page.
                src, dest, n = Buffer(len(cur_stream), cur_stream), Buffer(PAGE), U(PAGE)
                assert api["decompress"](codec, src.ptr, len(cur_stream), dest.ptr, C.byref(n)) == 0
                assert dest.bytes() == bytes(cur_page); dest.check()
            if i % 50 == 0: assert lib.lab_check_allocation_guards() == 0

        # KO accepts smaller output ceilings for the intermediate compressed stream.
        reference = rng.randbytes(2048)
        current = reference[:1000] + bytes([reference[1000] ^ 1]) + reference[1001:]
        value = delta(api, codec, reference, current, ceiling=len(current))
        assert value
        ref, src, dst, n = Buffer(len(reference), reference), Buffer(len(value), value), Buffer(PAGE), U(len(current))
        assert upstream["decompress_delta"](original, src.ptr, len(value), ref.ptr, len(reference),
                                            dst.ptr, C.byref(n)) == -22
        counts["ko_contract_differences"] += 1

        # Truncating a valid compressed page must not silently produce a full page.
        for stream in streams:
            src = Buffer(len(stream), stream)
            for cut in sorted(set([0, 1, 2, len(stream) // 2, len(stream) - 1])):
                dst, n = Buffer(PAGE), U(PAGE)
                assert api["decompress"](codec, src.ptr, cut, dst.ptr, C.byref(n)) == -22
                dst.check(); counts["truncated_stream_rejections"] += 1

        for i in range(args.fuzz_cases):
            size = rng.randrange(PAGE + 1)
            src = Buffer(size, rng.randbytes(size))
            dst, n = Buffer(PAGE), U(PAGE)
            ret = api["decompress"](codec, src.ptr, size, dst.ptr, C.byref(n))
            assert ret in (0, -22)
            if not ret: assert n.value == PAGE
            if not ret and len(accepted_samples) < 16:
                accepted_samples.append({"case": i, "mode": "ordinary", "input_b64": base64.b64encode(src.bytes()).decode(),
                                         "output_sha256": hashlib.sha256(dst.bytes()).hexdigest()})
            counts["malformed_decode_calls"] += 1
            counts["malformed_rejected" if ret else "malformed_accepted"] += 1
            src.check(); dst.check()
            ref = Buffer(PAGE, rng.randbytes(PAGE))
            capacity = rng.randrange(1, PAGE + 1)
            dst, n = Buffer(capacity), U(capacity)
            ret = api["decompress_delta"](codec, src.ptr, size, ref.ptr, PAGE,
                                            dst.ptr, C.byref(n))
            assert ret in (0, -22)
            if not ret: assert 0 < n.value <= capacity
            if not ret and len(accepted_samples) < 16:
                accepted_samples.append({"case": i, "mode": "delta", "input_b64": base64.b64encode(src.bytes()).decode(),
                                         "reference_b64": base64.b64encode(ref.bytes()).decode(), "capacity": capacity,
                                         "decoded_length": n.value,
                                         "output_sha256": hashlib.sha256(dst.bytes(n.value)).hexdigest()})
            counts["malformed_decode_calls"] += 1
            counts["malformed_rejected" if ret else "malformed_accepted"] += 1
            src.check(); ref.check(); dst.check()
            if i % 50 == 0: assert lib.lab_check_allocation_guards() == 0

        for bad_size in [0, 1, 15, 4095, 4097, 8192]:
            src, dst, n = Buffer(PAGE, b"\0" * PAGE), Buffer(PAGE), U(PAGE)
            assert api["compress"](codec, src.ptr, bad_size, dst.ptr, C.byref(n)) == -22
            counts["invalid_argument_cases"] += 1
            dst.check()
        for bad_size in [0, 4097, 8192]:
            src, dst, n = Buffer(PAGE), Buffer(PAGE), U(PAGE)
            assert api["decompress"](codec, src.ptr, bad_size, dst.ptr, C.byref(n)) == -22
            counts["invalid_argument_cases"] += 1
            dst.check()
        assert lib.lab_check_allocation_guards() == 0
    finally:
        api["destroy"](codec); upstream["destroy"](original)
    assert lib.lab_live_allocations() == 0 and lib.lab_check_allocation_guards() == 0
    result = {"passed": True, "seed": "0x4C5A344B4453", "counts": counts,
              "library_sha256": hashlib.sha256(args.library.read_bytes()).hexdigest(),
              "test_script_sha256": hashlib.sha256(Path(__file__).read_bytes()).hexdigest(),
              "live_allocations_after_cleanup": lib.lab_live_allocations(),
              "heap_and_buffer_guard_failures": 0,
              "android_device_used": False, "kernel_sddc_reference_concurrency_tested": False,
              "original_ko_binary_executed": False, "power_or_storage_benefit_measured": False,
              "warning": "Codec syntax is not a checksum; accepted altered streams need higher-layer integrity checks."}
    samples_path = args.output.with_name(args.output.stem + ".accepted-inputs.json")
    samples_path.write_text(json.dumps(accepted_samples, indent=2) + "\n", encoding="utf-8")
    result["accepted_input_samples"] = str(samples_path)
    args.output.write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--library", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--rounds", type=int, default=2000)
    parser.add_argument("--fuzz-cases", type=int, default=10000)
    run(parser.parse_args())
