"""Scene table arithmetic/layout and supported migration-boundary tests."""
import copy
from pathlib import Path
import struct
import tempfile
import unittest

from compile_scene_policy import compile_scene_table, import_powercfg


def fixture():
    profile = {"presets": {
        "common_app": [["$mod_percent", "105"]],
        "powersave_active": [["@limiter", "p1"]],
        "powersave_inactive": [["@limiter", "idle"]],
        "balance_active": [["@limiter", "p2"]],
        "balance_inactive": [["@limiter", "inactive"]],
        "performance_active": [["@limiter", "p3"]],
        "performance_inactive": [["@limiter", "inactive"]],
        "fast_active": [["@cpu_freqs_min", "960000", "1017600"]],
        "fast_inactive": [],
    }, "schemes": {}, "features": {"limiter": {"limiters": {}}}}
    for name, minimum, maximum, percent in (
            ("p1", 748800, 1996800, 150), ("p2", 960000, 2400000, 200),
            ("p3", 1152000, 2745600, 200), ("idle", 556800, 1555200, 100),
            ("inactive", 556800, 1785600, 120)):
        profile["features"]["limiter"]["limiters"][name] = {"cpus": [
            {"min": minimum, "max": maximum,
             "margins": f"{percent} 1152000:90", "excludes": [0]},
            {"min": 1017600, "max": 2841600, "margins": str(percent)},
        ]}
    apps = {"modes": []}
    for mode in ("powersave", "balance", "performance", "fast"):
        profile["schemes"][mode] = {"app": [["@preset", "common_app"],
            ["@cpu_freqs_max", "2745600", "#3283200"], ["@preset", mode + "_active"]]}
        apps["modes"].append({"mode": [mode], "state": {
            state: [["@preset", mode + "_" + state]] for state in ("active", "inactive")}})
    camera = {"call": [["@limiter", "NONE"]], "state": {
        "active": [["@cpu_freq", "policy0", "min", "2400000"],
                   ["@cpu_freq", "policy6", "min", "3283200"]],
        "inactive": [["@cpu_freq", "policy0", "min", "1996800"],
                     ["@cpu_freq", "policy6", "min", "2841600"]]}}
    controls = {"cpus": [{"cpu": 0, "game_khz": [384000, 3532800]},
                         {"cpu": 6, "game_khz": [1017600, 4320000]}],
                "gpu_scene_khz": [330000, 1083000], "gpu_idle_khz": [330000, 660000]}
    return profile, {"_apps.json": apps, "_elp.json": copy.deepcopy(apps),
                     "_camera.json": camera}, controls


class SceneTableTest(unittest.TestCase):
    def test_binary_layout_and_mode_difference(self):
        data, report = compile_scene_table(*fixture(), {"mode": "balance", "packages": {
            "com.example.reader": "powersave", "com.example.fast": "fast"}})
        self.assertEqual(len(data), 14384)
        self.assertEqual(struct.unpack_from("<6I", data), (1, 2, 2, 6, 0, 6))
        active = report["profiles"][report["defaults"][0]]
        self.assertEqual(active["cpus"][0]["khz"], [960000, 2400000])
        rules = {rule["package"]: rule for rule in report["rules"]}
        fast = report["profiles"][rules["com.example.fast"]["profiles"][0]]
        self.assertEqual(fast["cpus"][0]["margins"], [])
        self.assertEqual(fast["gpu_percent"], 105)
        off = report["profiles"][report["defaults"][2]]
        self.assertEqual(off["gpu_percent"], 0)
        self.assertEqual(off["gpu_khz"], (330000, 660000))
        self.assertIn("excludes", report["unported_limiter_fields"])

    def test_camera_does_not_inherit_app_dynamic_cap(self):
        _, report = compile_scene_table(*fixture(), {"packages": {
            "com.example.camera": {"mode": "powersave", "category": "Camera"}}})
        state = report["profiles"][report["rules"][0]["profiles"][0]]
        self.assertEqual(state["cpus"][0]["khz"], [2400000, 3532800])
        self.assertEqual(state["cpus"][1]["khz"], [3283200, 4320000])
        self.assertFalse(any(cpu["margins"] for cpu in state["cpus"]))

    def test_ignore_is_explicitly_disabled(self):
        _, report = compile_scene_table(*fixture(), {"packages": {"com.example.ignore": "igoned"}})
        self.assertEqual(len(set(report["rules"][0]["profiles"])), 1)
        self.assertFalse(report["profiles"][report["rules"][0]["profiles"][0]]["enabled"])

    def test_rejects_unrepresentable_or_ambiguous_inputs(self):
        for settings in ({"mode": "unknown"},
                         {"packages": {"*": "balance"}},
                         {"packages": {"com.test": {"category": "invented"}}},
                         {"packages": {f"com.test.p{i}": "balance" for i in range(65)}}):
            with self.subTest(settings=list(settings)), self.assertRaises(ValueError):
                compile_scene_table(*fixture(), settings)
        profile, sidecars, controls = fixture()
        profile["presets"]["balance_active"] = [["@preset", "balance_active"]]
        with self.assertRaises(ValueError):
            compile_scene_table(profile, sidecars, controls, {})

    def test_reports_other_actions_without_executing_them(self):
        profile, sidecars, controls = fixture()
        profile["presets"]["common_app"].append(["/do/not/write", "1"])
        _, report = compile_scene_table(profile, sidecars, controls, {})
        self.assertIn("/do/not/write", report["unported_action_names"])

    def test_filters_activity_preferences_using_package_inventory(self):
        text = '<map><string name="com.test.app">performance</string>' \
               '<string name="com.test.app.CameraActivity">fast</string>' \
               '<string name="*">powersave</string></map>'
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "powercfg.xml"
            path.write_text(text, encoding="utf-8")
            imported, skipped, mode = import_powercfg(path, {"com.test.app"})
        self.assertEqual(imported, {"com.test.app": {"mode": "performance"}})
        self.assertEqual(skipped, ["com.test.app.CameraActivity"])
        self.assertEqual(mode, "powersave")


if __name__ == "__main__":
    unittest.main()
