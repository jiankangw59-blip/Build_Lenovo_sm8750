import copy
import gzip
import json
from pathlib import Path
import unittest

from memory_layout import compare_memory_layouts


class MemoryLayoutTests(unittest.TestCase):
    def setUp(self):
        self.expected = json.loads(Path(__file__).with_name("kmi").joinpath("plq110-memory-layouts.json").read_text(encoding="utf-8"))
        self.actual = copy.deepcopy(self.expected)

    def test_unchanged_baseline(self):
        self.assertEqual(compare_memory_layouts(self.expected, self.actual), [])

    def add_le9uo(self):
        for name, bit in [("anon_below_min", 360), ("clean_below_low", 361), ("clean_below_min", 362)]:
            self.actual["scan_control"]["members"].append({
                "name": name, "offset_bits": bit, "bitfield_bits": 1, "type": "int:unsigned int:4:32"})

    def test_only_three_reviewed_padding_bits_allowed(self):
        self.add_le9uo()
        self.assertEqual(compare_memory_layouts(self.expected, self.actual), [])

    def test_118_le9uo_preserves_native_fields(self):
        snapshot = Path(__file__).with_name("kmi") / "plq110-6.6.118-layouts.json.gz"
        self.expected["scan_control"] = json.loads(gzip.decompress(snapshot.read_bytes()))["structs"]["scan_control"]
        self.actual = copy.deepcopy(self.expected)
        for name, bit in [("anon_below_min", 424), ("clean_below_low", 425), ("clean_below_min", 426)]:
            self.actual["scan_control"]["members"].append({
                "name": name, "offset_bits": bit, "bitfield_bits": 1, "type": "int:unsigned int:4:32"})
        self.assertEqual(compare_memory_layouts(self.expected, self.actual, 424), [])
        self.assertTrue(compare_memory_layouts(self.expected, self.actual))
        field = next(m for m in self.actual["scan_control"]["members"] if m["name"] == "proactive_swappiness")
        field["offset_bits"] += 8
        self.assertTrue(compare_memory_layouts(self.expected, self.actual, 424))

    def test_reject_upstream_bitfield_shift(self):
        field = next(m for m in self.actual["scan_control"]["members"] if m["name"] == "no_demotion")
        field["offset_bits"] += 3
        self.assertTrue(compare_memory_layouts(self.expected, self.actual))

    def test_reject_flag_overlapping_existing_field(self):
        self.add_le9uo()
        self.actual["scan_control"]["members"][-1]["offset_bits"] = 384
        self.assertTrue(compare_memory_layouts(self.expected, self.actual))

    def test_reject_resizing(self):
        self.actual["pglist_data"]["size"] += 8
        self.assertTrue(compare_memory_layouts(self.expected, self.actual))

    def test_kcompressd_reserve_union(self):
        field = next(m for m in self.actual["pglist_data"]["members"] if m["name"] == "android_kabi_reserved1")
        field.update(name="", type="union::8")
        self.assertEqual(compare_memory_layouts(self.expected, self.actual), [])

    def test_reject_reserve_overflow(self):
        field = next(m for m in self.actual["pglist_data"]["members"] if m["name"] == "android_kabi_reserved1")
        field.update(name="", type="union::16")
        self.assertTrue(compare_memory_layouts(self.expected, self.actual))


if __name__ == "__main__":
    unittest.main()
