"""Record the supplied KO's DWARF/source boundaries without executing it."""
import argparse
import hashlib
import json
from pathlib import Path


def check(args):
    path = args.analysis / "analysis/oplus_bsp_hybridswap_zram.json"
    source = args.analysis / "recovered/oplus_bsp_hybridswap_zram.c"
    evidence = json.loads(path.read_text(encoding="utf-8"))
    expected = {
        "crystal_sddc_codec": (16, {"encode_state": 0, "delta_window": 8}),
        "crystal_sddc_delta_header": (24, {"magic": 0, "version": 4, "header_size": 6,
                                                  "ref_id": 8, "ref_generation": 12,
                                                  "ref_size": 16, "target_size": 20}),
    }
    checks = {}
    for name, (size, members) in expected.items():
        actual = evidence["structures"][name]
        checks[name] = actual["size"] == size and {
            member["name"]: member["offset"] for member in actual["members"]} == members
    functions = [f for f in evidence["functions"] if f["name"].startswith("crystal_sddc_codec_")
                 or f["name"] in ["crystal_lz4kd_encode", "crystal_lz4kd_decode",
                                   "crystal_lz4kd_encode_delta", "crystal_lz4kd_decode_delta"]]
    names = {f["name"] for f in functions}
    required = {"crystal_sddc_codec_compress", "crystal_sddc_codec_decompress",
                "crystal_sddc_codec_compress_delta", "crystal_sddc_codec_decompress_delta",
                "crystal_sddc_codec_decompress_delta_borrowed", "crystal_lz4kd_decode_delta"}
    checks["codec_entry_points_with_dwarf"] = required <= names
    report = {
        "passed": all(checks.values()), "checks": checks,
        "ko_sha256_from_original_analysis": evidence["sha256"],
        "analysis_sha256": hashlib.sha256(path.read_bytes()).hexdigest(),
        "recovered_c_sha256": hashlib.sha256(source.read_bytes()).hexdigest(),
        "dwarf_layouts": {name: evidence["structures"][name] for name in expected},
        "functions": [{key: f.get(key) for key in ["name", "source", "line", "prototype", "calls"]}
                      for f in functions],
        "confirmed_contract_difference": {
            "ko_delta_decode_output_ceiling": "1..4096 bytes, from unsigned decompiled condition",
            "retained_source_delta_decode_output_ceiling": "exactly 4096 bytes",
            "restored_codec": "uses the supplied KO's 1..4096-byte ceiling",
        },
        "version_boundary": "Retained pin enables optional native SDDC-to-ZMS; supplied KO skips SDDC writeback.",
        "original_ko_executed": False,
        "binary_equivalence_proven": False,
    }
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": report["passed"], "checks": checks,
                      "recovered_functions": len(functions)}, indent=2))
    if not report["passed"]: raise SystemExit(1)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--analysis", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    check(parser.parse_args())
