"""Check the build against the tablet's loaded vendor modules and BTF."""
import argparse
import gzip
import json
from pathlib import Path
import re

from kmi_baseline import Btf


NETWORK_TYPES = {"tcp_sock", "inet_connection_sock"}
NETWORK_RESERVE = re.compile(r"unused\d*|android_kabi_reserved\d+")
SCAN_FLAGS = {"anon_below_min", "clean_below_low", "clean_below_min"}


def member_width(member):
    if member["bitfield_bits"]:
        return member["bitfield_bits"]
    if member["type"].startswith("pointer:"):
        return 64
    match = re.fullmatch(r"(?:int|struct|union):[^:]*:(\d+)(?::.*)?", member["type"])
    return int(match[1]) * 8 if match else 0


def layout_matches(name, expected, actual):
    if actual["size"] != expected["size"]:
        return False
    if name in NETWORK_TYPES:
        original = expected["members"]
        protected = [entry for entry in original if not NETWORK_RESERVE.fullmatch(entry["name"])]
        reserves = [(m["offset_bits"], m["offset_bits"] + member_width(m))
                    for m in original if m not in protected]
        current = actual["members"]
        return all(m in current for m in protected) and all(
            m in protected or (member_width(m) and any(
                first <= m["offset_bits"] and m["offset_bits"] + member_width(m) <= end
                for first, end in reserves))
            for m in current
        )
    if name == "scan_control":
        flags = [entry for entry in actual["members"] if entry["name"] in SCAN_FLAGS]
        return (
            {entry["name"] for entry in flags} == SCAN_FLAGS
            and sorted((m["offset_bits"], m["bitfield_bits"]) for m in flags)
                == [(360, 1), (361, 1), (362, 1)]
            and [m for m in actual["members"] if m["name"] not in SCAN_FLAGS] == expected["members"]
        )
    return actual["members"] == expected["members"]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--symvers", required=True, type=Path)
    parser.add_argument("--btf", required=True, type=Path)
    parser.add_argument("--report", required=True, type=Path)
    args = parser.parse_args()
    baseline_path = Path(__file__).with_name("kmi") / "opd2413-6.6.89.json.gz"
    baseline = json.loads(gzip.decompress(baseline_path.read_bytes()))
    exports = {}
    for line in args.symvers.read_text(encoding="utf-8").splitlines():
        fields = line.split()
        if len(fields) >= 2:
            exports[fields[1]] = int(fields[0], 16)
    failures = []
    for symbol, (crc, count) in baseline["gki_crcs"].items():
        actual_crc = exports.get(symbol)
        if actual_crc != int(crc, 16):
            failures.append({"symbol": symbol, "references": count, "expected": crc,
                             "actual": None if actual_crc is None else f"0x{actual_crc:08x}"})
    btf = Btf(args.btf.read_bytes())
    layouts = btf.layouts(baseline["structures"])
    for name, expected in baseline["structures"].items():
        if not layout_matches(name, expected, layouts[name]):
            failures.append({"structure": name, "expected_size": expected["size"],
                             "actual_size": layouts[name]["size"]})
    report = {
        "model": baseline["model"],
        "baseline": baseline["kernel_release"],
        "baseline_loaded_modules": len(baseline["modules"]),
        "checked_imports": sum(count for _, count in baseline["gki_crcs"].values()),
        "checked_exports": len(baseline["gki_crcs"]),
        "layouts": layouts,
        "failures": failures,
        "passed": not failures,
    }
    args.report.parent.mkdir(parents=True, exist_ok=True)
    args.report.write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(f"Checked {report['checked_imports']} imports and {len(layouts)} BTF layouts, "
          f"{len(failures)} failures")
    if failures:
        print(json.dumps(failures[:20], indent=2))
        raise SystemExit(1)


if __name__ == "__main__":
    main()
