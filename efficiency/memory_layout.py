"""Additional memory-patch ABI guards: only explicit padding/reserve reuse."""
import json
from pathlib import Path


def compare_memory_layouts(expected, actual, scan_padding_bit=360):
    failures = []
    for name, old in expected.items():
        new = actual[name]
        reserved = [m for m in old["members"] if m["name"] == "android_kabi_reserved1"] if name == "pglist_data" else []
        protected = [m for m in old["members"] if m not in reserved]
        if new["size"] != old["size"] or not all(m in new["members"] for m in protected):
            failures.append({"structure": name, "reason": "Existing memory ABI field or size changed"})
            continue
        for field in new["members"]:
            if field in protected or field in reserved:
                continue
            if name == "scan_control":
                allowed = {"anon_below_min": scan_padding_bit, "clean_below_low": scan_padding_bit + 1,
                           "clean_below_min": scan_padding_bit + 2}
                valid = (allowed.get(field["name"]) == field["offset_bits"] and
                         field["bitfield_bits"] == 1 and field["type"] == "int:unsigned int:4:32")
            else:
                valid = (len(reserved) == 1 and field["name"] == "" and field["type"] == "union::8" and
                         field["offset_bits"] == reserved[0]["offset_bits"] and not field["bitfield_bits"])
            if not valid:
                failures.append({"structure": name, "reason": "Unreviewed memory padding/reserve reuse", "field": field})
    return failures


def verify_memory_layouts(btf, scan_control=None):
    expected = json.loads(Path(__file__).with_name("kmi").joinpath("plq110-memory-layouts.json").read_text(encoding="utf-8"))
    scan_padding_bit = 360
    if scan_control is not None:
        expected["scan_control"] = scan_control
        scan_padding_bit = 424
    actual = btf.layouts(expected)
    return compare_memory_layouts(expected, actual, scan_padding_bit), actual
