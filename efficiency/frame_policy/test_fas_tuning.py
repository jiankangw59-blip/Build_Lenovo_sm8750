"""Verify imported mode precision, overrides and refusal of unsupported policy."""
import struct
import unittest

from compile_fas_tuning import compile_tuning


class FasTuning(unittest.TestCase):
    def test_actual_four_mode_values_and_user_precedence(self):
        for mode, value in {"powersave": 3.0, "balance": 1.0, "performance": 0.3, "fast": 0}.items():
            blob, report = compile_tuning(mode, {mode: {"margin_fps": 2}},
                                          {mode: {"margin_fps": value, "core_temp_thresh": "disabled"}})
            self.assertEqual(len(blob), 80)
            self.assertEqual(report["base_margin_mfps"], int(value * 1000))
            self.assertEqual(struct.unpack_from("<I", blob, 8)[0], int(value * 1000))

    def test_advanced_overrides_use_explicit_rate_margin(self):
        blob, report = compile_tuning("balance", {}, {"balance": {"margin_fps": {"base": 1, "165": 0.3, "120": 1.5}}})
        self.assertEqual(report["overrides"], [(120, 1500), (165, 300)])
        self.assertEqual(struct.unpack_from("<4I", blob, 16), (120, 1500, 165, 300))
        self.assertEqual(blob[32:], bytes(48))

    def test_invalid_margin_inputs_are_rejected(self):
        for value in (True, "1", -1, float("nan"), float("inf"), 0.0001, 31,
                      {"base": 1, "060": 1}, {"base": 1, "60": 60},
                      {"base": 1, **{str(i): 1 for i in range(15, 24)}}):
            with self.subTest(value=value), self.assertRaises(ValueError):
                compile_tuning("balance", {}, {"balance": {"margin_fps": value}})

    def test_unsupported_temperature_offset_is_never_silently_ignored(self):
        with self.assertRaises(ValueError):
            compile_tuning("fast", {}, {"fast": {"core_temp_thresh": 75}})


if __name__ == "__main__":
    unittest.main()
