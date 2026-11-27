"""Export only public numeric Scene frequency profiles for the config WebUI.

This generator accepts no personal powercfg or package inventory. It reuses the
reviewed Python Scene importer, then deduplicates profile bytes across all modes
and categories. The WebUI later repacks only selected profiles into the bounded
kernel table; a large library never silently truncates package behavior.
"""
import argparse
import hashlib
import json
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compile_scene_policy import MODES, compile_scene_table, encode_profile


def generate(profile, sidecars, controls):
    profiles, encoded, selectors = [], [], {}
    unported_actions, unported_fields = set(), set()

    def intern(item):
        blob = encode_profile(item, len(controls["cpus"]))
        if blob in encoded:
            return encoded.index(blob)
        encoded.append(blob)
        if not item["enabled"]:
            profiles.append({"enabled": False})
        else:
            profiles.append({
                "enabled": True, "gpuPercent": item["gpu_percent"],
                "gpuKHz": list(item["gpu_khz"]),
                "cpus": [{"kHz": cpu["khz"], "margins": cpu["margins"]}
                         for cpu in item["cpus"]],
            })
        return len(profiles) - 1

    for mode in MODES:
        selectors[mode] = {}
        for category, source_category in (("normal", "normal"), ("Camera", "Camera"), ("ELP", "IM")):
            _, report = compile_scene_table(profile, sidecars, controls, {
                "mode": mode, "packages": {
                    "org.framepolicy.seed": {"mode": mode, "category": source_category}}})
            indices = report["rules"][0]["profiles"]
            selectors[mode][category] = [intern(report["profiles"][index]) for index in indices]
            unported_actions.update(report["unported_action_names"])
            unported_fields.update(report["unported_limiter_fields"])
    ignored = intern({"enabled": False})
    ids = [cpu["cpu"] for cpu in controls["cpus"]]
    return {
        "abi": 1, "tableSize": 14384, "profileSize": 336,
        "cpuIds": ids,
        "cpuBounds": [cpu["hardware_khz"] if "hardware_khz" in cpu else cpu["game_khz"]
                      for cpu in controls["cpus"]],
        "profiles": profiles, "selectors": selectors,
        "ignored": [ignored] * 3,
        "categoryAliases": {value: "ELP" for value in
                            ("IM", "Music", "Reader", "ShortVideo", "Nav", "ScrollOpt")},
        "libraryProfileCount": len(profiles),
        "selectionPacking": "deduplicate only profiles selected by default/package rules",
        "inactiveEventAvailable": False,
        "unportedActionNames": sorted(unported_actions),
        "unportedLimiterFields": sorted(unported_fields),
        "numericProfilesSha256": hashlib.sha256(b"".join(encoded)).hexdigest(),
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--sidecars", required=True, type=Path)
    parser.add_argument("--controls", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    load = lambda path: json.loads(path.read_text(encoding="utf-8"))
    sidecars = {name: load(args.sidecars / name)
                for name in ("_apps.json", "_elp.json", "_camera.json")}
    result = generate(load(args.profile), sidecars, load(args.controls))
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(result, ensure_ascii=True, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"libraryProfiles": result["libraryProfileCount"],
                      "numericProfilesSha256": result["numericProfilesSha256"]}))


if __name__ == "__main__":
    main()
