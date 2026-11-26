"""Export public numeric aux policy, without personal application inventories.

Direct actions use the reviewed expanded catalog; raw feedback is resolved from
each original selector before deduplication. Legacy percent margins are unused.
"""
import argparse
import hashlib
import json
from decimal import Decimal
from pathlib import Path
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from compile_scene_policy import MODES, expand, state_commands

TABLE_SIZE, CPU_SIZE, PROFILE_SIZE = 26584, 136, 544


def fixed(value, scale=1):
    if isinstance(value, bool):
        raise ValueError("Boolean is not a limiter value")
    result = Decimal(str(value)) * scale
    if not result.is_finite() or result != result.to_integral_value():
        raise ValueError("Original value cannot be represented exactly")
    return int(result)


def additive(raw):
    tokens = raw.split()
    if not tokens:
        raise ValueError("Empty original additive margin")
    result = [[0, max(fixed(tokens[0]), 0) * 1000]]
    for token in tokens[1:]:
        threshold, offset = token.split(":")
        result.append([fixed(threshold), max(fixed(offset), 0) * 1000])
    if len(result) > 8 or any(not 0 <= t <= 10000000 or not -10000000 <= v <= 10000000 for t, v in result) \
            or any(a[0] >= b[0] for a, b in zip(result, result[1:])):
        raise ValueError("Invalid original additive steps")
    return result


def empty_cpu():
    return dict(validMask=0, upUs=0, downUs=0, hispeedKHz=0, waltBoost=0,
                coreEnable=0, minCpus=0, maxCpus=0, excludesMask="0x0", prefer=0,
                limiterClusterCount=0, limiterClustersPacked=0, feedbackMask=0,
                perfectKHz=0, smoothness=0, mtMilliPercent=0, additive=[])


def generate(catalog, ids, profile, sidecars, options):
    if not 1 <= len(ids) <= 4 or len(set(ids)) != len(ids):
        raise ValueError("Invalid CPU policy IDs")
    option_names = {"source", "statSource", "gestureBoost", "gestureDurationMs", "launcherBoost", "perfNotify",
                    "perfNotifyInApps", "perfUsePresets", "kswapdBoost", "panelPeakHz", "gpuLock"}
    if set(options["values"]) != option_names | {"validMask"} or set(options["origins"]) != option_names:
        raise ValueError("Only the known numeric options and provenance may enter the public seed")
    allowed_origins = {"saved-original-source", "native-default-key-missing", "original-profile",
                       "original-profile-omitted-native-min500", "explicit-native-pref", "physical-panel-capability"}
    if any(value not in allowed_origins for value in options["origins"].values()):
        raise ValueError("Unrecognized option provenance")
    profiles, selectors = [], {}

    def intern(mode, category, state):
        original = catalog["profiles"][catalog["selectors"][f"{mode}:{category}:{state}"]]
        direct = original["direct"]
        limiter_name = None
        if state != "off":
            name = {"normal": "_apps.json", "ELP": "_elp.json", "Camera": "_camera.json"}[category]
            commands = profile["schemes"][mode]["app"] + state_commands(sidecars[name], mode, state)
            for command in expand(profile, commands):
                if command[0] == "@limiter":
                    if len(command) != 2:
                        raise ValueError("Invalid limiter selection")
                    limiter_name = None if command[1] == "NONE" else command[1]
        limiter = profile["features"]["limiter"]["limiters"][limiter_name] if limiter_name else None
        if limiter is not None and len(limiter["cpus"]) != len(ids):
            raise ValueError("Original limiter topology mismatch")
        clusters = limiter.get("core_ctl", []) if limiter else []
        if len(clusters) > len(ids) or len(set(clusters)) != len(clusters) or any(type(v) is not int or not 0 <= v < len(ids) for v in clusters):
            raise ValueError("Invalid ordered cluster vector")
        cpus = []
        for index, cpu in enumerate(ids):
            row = empty_cpu()
            for name, attribute, bit in (("up_rate_limit_us", "upUs", 1), ("down_rate_limit_us", "downUs", 2)):
                path = f"/sys/devices/system/cpu/cpufreq/policy{cpu}/walt/{name}"
                if path in direct:
                    row[attribute] = direct[path]; row["validMask"] |= bit
            for name, attribute, bit in (("hispeed", "hispeedKHz", 4), ("walt_boost", "waltBoost", 8)):
                if name in direct:
                    row[attribute] = direct[name][index]; row["validMask"] |= bit
            core = ["core_ctl_" + str(cpu), "min_cpus_cpu" + str(cpu), "max_cpus_cpu" + str(cpu)]
            if any(key in direct for key in core):
                if not all(key in direct for key in core):
                    raise ValueError("Original direct core tuple incomplete")
                row.update(coreEnable=direct[core[0]], minCpus=direct[core[1]], maxCpus=direct[core[2]])
                row["validMask"] |= 16
            if limiter is not None:
                raw = limiter["cpus"][index]
                excluded = raw.get("excludes", [])
                if any(type(v) is not int or not 0 <= v < 64 for v in excluded):
                    raise ValueError("Invalid excluded CPU")
                row.update(excludesMask=hex(sum(1 << v for v in set(excluded))), prefer=raw.get("prefer", 0),
                           limiterClusterCount=len(clusters),
                           limiterClustersPacked=sum(v << (i * 8) for i, v in enumerate(clusters)),
                           perfectKHz=fixed(raw.get("perfect", 0)), smoothness=fixed(raw.get("smoothness", 0)),
                           mtMilliPercent=fixed(raw.get("mt", 0), 1000), additive=additive(raw["margins"]),
                           feedbackMask=127)
                if raw.get("fas", False):
                    raise ValueError("Go-FAS is separate from non-game aux")
            cpus.append(row)
        result = {"cpus": cpus}
        if result not in profiles:
            profiles.append(result)
        return profiles.index(result)

    for mode in MODES:
        selectors[mode] = {category: [intern(mode, category, state) for state in ("active", "inactive", "off")]
                           for category in ("normal", "ELP", "Camera")}
    empty = {"cpus": [empty_cpu() for _ in ids]}
    if empty not in profiles:
        profiles.append(empty)
    ignored = profiles.index(empty)
    return dict(abi=1, tableSize=TABLE_SIZE, cpuSize=CPU_SIZE, profileSize=PROFILE_SIZE,
                cpuIds=ids, profiles=profiles, selectors=selectors, ignored=[ignored] * 3,
                options=options["values"], optionOrigins=options["origins"],
                categoryAliases={name: "ELP" for name in ("IM", "Music", "Reader", "ShortVideo", "Nav", "ScrollOpt")},
                sourceSemantics="36 original selectors; deduplicate complete typed records after raw selection",
                marginUnits="threshold_kHz; max(rawOffsetMHz,0)*1000 additive offset_kHz",
                feedbackCoreMetadataOnly=False)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--catalog", type=Path, required=True)
    parser.add_argument("--profile", type=Path, required=True)
    parser.add_argument("--sidecars", type=Path, required=True)
    parser.add_argument("--options", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--cpu-ids", default="0,6")
    args = parser.parse_args()
    load = lambda path: json.loads(path.read_text(encoding="utf-8-sig"))
    result = generate(load(args.catalog), [int(v) for v in args.cpu_ids.split(",")], load(args.profile),
                      {name: load(args.sidecars / name) for name in ("_apps.json", "_elp.json", "_camera.json")}, load(args.options))
    result["rawProfileSha256"] = hashlib.sha256(args.profile.read_bytes()).hexdigest()
    args.output.write_text(json.dumps(result, indent=2, ensure_ascii=True) + "\n", encoding="utf-8")
