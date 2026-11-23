"""Compare successful, matching private-device runs, never infer NAND writes."""
import argparse
import json
from pathlib import Path

STORAGE_READ_PROTOCOL = "owned-file-dio-sync-evict-mincore-v1"


def compare(native, crystal):
    for data, backend in [(native, "native"), (crystal, "crystal")]:
        if not data.get("passed") or not data.get("runtime_test_performed") or data.get("backend") != backend:
            raise RuntimeError("Comparison requires successful actual runs of both named backends")
        if not data.get("main_zram_configuration_unchanged") or not data.get("same_boot_after_test"):
            raise RuntimeError("A run changed its protected baseline or crossed a reboot")
        if not data.get("rounds"):
            raise RuntimeError("Use new matching DIO/backing-sync/cache-observation controls; do not mix old buffered reports")
        for row in data.get("rounds", []):
            if not row.get("passed") or not all(row.get("checks", {}).get(key) is True for key in (
                    "owned_backing_file_synced_and_evicted", "direct_io_and_zero_file_cache_before_and_after_read")):
                raise RuntimeError("Every compared round must have the explicit owned-backing cold-read checks")
    if native["pattern_schema"] != crystal["pattern_schema"]:
        raise RuntimeError("Patterns differ")
    by_seed = [{r["seed"]: r for r in data["rounds"]} for data in (native, crystal)]
    if set(by_seed[0]) != set(by_seed[1]) or not by_seed[0]:
        raise RuntimeError("Round seeds differ")
    rows = []
    for seed, left in by_seed[0].items():
        right = by_seed[1][seed]
        if left["logical_input_bytes"] != right["logical_input_bytes"]:
            raise RuntimeError("Logical input lengths differ")
        if left["fill"]["fnv1a64"] != right["fill"]["fnv1a64"]:
            raise RuntimeError("Known input streams differ")
        nw = left["measurements"]["private_loop_write_bytes_including_internal_io"]
        cw = right["measurements"]["private_loop_write_bytes_including_internal_io"]
        if nw <= 0 or cw <= 0:
            raise RuntimeError("No real backend writes were recorded")
        rows.append({"seed": seed, "logical_input_bytes": left["logical_input_bytes"],
                     "native_loop_write_bytes": nw, "crystal_loop_write_bytes": cw,
                     "loop_write_bytes_reduction": nw - cw,
                     "loop_write_reduction_fraction": (nw - cw) / nw})
    return {"comparison_performed": True, "pattern_schema": native["pattern_schema"],
            "storage_read_protocol": STORAGE_READ_PROTOCOL,
            "native_codec": native["codec"], "crystal_codec": crystal["codec"], "rounds": rows,
            "scope": "Private loop logical I/O includes internal flush/GC, excludes file-allocation filesystem metadata; not UFS NAND writes",
            # Native Crypto API and the SDDC test kernel's private base encoder
            # may share a codec name without sharing an implementation.
            "encoder_implementation_equivalence_verified": False,
            "packing_isolated_from_codec_change": False,
            "attribution": "Combined backend result; codec names alone cannot isolate packing from encoder differences",
            "ufs_nand_writes_measured": False, "power_savings_verified": False}


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--native", type=Path, required=True)
    parser.add_argument("--crystal", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    report = compare(json.loads(args.native.read_text(encoding="utf-8")),
                     json.loads(args.crystal.read_text(encoding="utf-8")))
    args.output.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))
