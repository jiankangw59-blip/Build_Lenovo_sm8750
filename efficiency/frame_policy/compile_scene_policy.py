"""Compile explicit Scene mode/category rules into an optional kernel table.

Only frequency bounds, limiter margins and the leased GPU governor percentage
are executable in this format. Other Scene actions are reported, never silently
presented as ported. The compiler does not run scripts or query the device.
"""
import argparse
import copy
import hashlib
import json
from pathlib import Path
import re
import struct
import xml.etree.ElementTree as ET

MODES = ("powersave", "balance", "performance", "fast")
LIMITS = (4, 8, 16, 64)
PROFILE_SIZE = 336
TABLE_SIZE = 14384
PACKAGE = re.compile(r"[A-Za-z0-9_]+(?:\.[A-Za-z0-9_]+)+\Z", re.ASCII)


def integer(value):
    if type(value) is int:
        return value
    if isinstance(value, str) and re.fullmatch(r"#?[0-9]+", value, re.ASCII):
        return int(value.removeprefix("#"))
    raise ValueError(f"Expected an unsigned literal frequency: {value!r}")


def expand(profile, commands, visiting=()):
    result = []
    for command in commands:
        if not isinstance(command, list) or not command or not isinstance(command[0], str):
            raise ValueError("Invalid Scene action")
        if command[0] == "@preset":
            if len(command) != 2 or command[1] in visiting:
                raise ValueError("Invalid/cyclic Scene preset")
            result.extend(expand(profile, profile["presets"][command[1]],
                                 visiting + (command[1],)))
        else:
            result.append(command)
    return result


def state_commands(sidecar, mode, state):
    if "modes" in sidecar:
        matches = [item for item in sidecar["modes"] if mode in item["mode"]]
        if not matches:
            matches = [item for item in sidecar["modes"] if "*" in item["mode"]]
        if len(matches) != 1:
            raise ValueError(f"No unambiguous Scene state for {mode}/{state}")
        states = matches[0]["state"]
    else:
        states = sidecar["state"]
    return sidecar.get("call", []) + states[state]


def margins(text):
    tokens = text.split()
    if not tokens:
        raise ValueError("Empty limiter margins")
    result = [(0, integer(tokens[0]))]
    for token in tokens[1:]:
        threshold, percent = token.split(":")
        result.append((integer(threshold), integer(percent)))
    if (not 1 <= len(result) <= 8 or any(not 1 <= p <= 1000 for _, p in result)
            or any(a[0] >= b[0] for a, b in zip(result, result[1:]))):
        raise ValueError("Invalid limiter margin steps")
    return result


def validate_limits(bounds):
    if len(bounds) != 2 or not 0 <= bounds[0] <= bounds[1] <= 10000000 or not bounds[1]:
        raise ValueError(f"Invalid frequency limits: {bounds}")


def encode_profile(item, cpu_count):
    if not item["enabled"]:
        return bytes(PROFILE_SIZE)
    data = struct.pack("<4I", 1, item["gpu_percent"], *item["gpu_khz"])
    for cpu in item["cpus"]:
        steps = cpu["margins"]
        data += struct.pack("<4I", *cpu["khz"], len(steps), 0)
        data += b"".join(struct.pack("<2I", *step) for step in steps)
        data += bytes((8 - len(steps)) * 8)
    data += bytes((4 - cpu_count) * 80)
    assert len(data) == PROFILE_SIZE
    return data


def compile_scene_table(profile, sidecars, controls, settings):
    cpus = controls["cpus"]
    ids = [cpu["cpu"] for cpu in cpus]
    if not 1 <= len(ids) <= 4 or len(set(ids)) != len(ids):
        raise ValueError("Expected one to four distinct CPU policy IDs")
    hardware = [tuple(cpu["hardware_khz"] if "hardware_khz" in cpu else cpu["game_khz"])
                for cpu in cpus]
    for bounds in hardware:
        validate_limits(bounds)
    default_mode = settings.get("mode", "balance")
    if default_mode not in MODES:
        raise ValueError("Unknown default Scene mode")
    gpu = tuple(controls["gpu_scene_khz"])
    gpu_idle = tuple(controls.get("gpu_idle_khz", gpu))
    validate_limits(gpu)
    validate_limits(gpu_idle)
    unported = set()
    unsupported_limiter_fields = set()
    profiles = []
    labels = []

    def compile_actions(commands, name, idle=False):
        item = {"enabled": True, "gpu_percent": 0,
                "gpu_khz": gpu_idle if idle else gpu,
                "cpus": [{"khz": list(bounds), "margins": []} for bounds in hardware]}
        for command in expand(profile, commands):
            op, *args = command
            if op in ("@cpu_freqs_min", "@cpu_freqs_max"):
                if len(args) != len(ids):
                    raise ValueError("Scene CPU action differs from CPU topology")
                slot = 0 if op.endswith("min") else 1
                for cpu, value in zip(item["cpus"], args):
                    cpu["khz"][slot] = integer(value)
            elif op == "@cpu_freq":
                if len(args) != 3 or not re.fullmatch(r"policy[0-9]+", args[0]):
                    raise ValueError("Unsupported per-policy CPU action")
                index = ids.index(int(args[0][6:]))
                if args[1] not in ("min", "max"):
                    raise ValueError("Unknown CPU frequency field")
                item["cpus"][index]["khz"][0 if args[1] == "min" else 1] = integer(args[2])
            elif op == "@limiter":
                if len(args) != 1:
                    raise ValueError("Invalid limiter action")
                if args[0] == "NONE":
                    # A camera's NONE must not inherit the previous app's low
                    # dynamic ceiling. Thermal/vendor QoS constraints still win.
                    for cpu, bounds in zip(item["cpus"], hardware):
                        cpu["margins"] = []
                        cpu["khz"][1] = bounds[1]
                else:
                    limiter = profile["features"]["limiter"]["limiters"][args[0]]
                    if len(limiter["cpus"]) != len(ids):
                        raise ValueError("Limiter topology mismatch")
                    for cpu, value in zip(item["cpus"], limiter["cpus"]):
                        cpu["khz"] = [integer(value["min"]), integer(value["max"])]
                        cpu["margins"] = margins(value["margins"])
                        unsupported_limiter_fields.update(set(value) - {"min", "max", "margins"})
                    unsupported_limiter_fields.update(set(limiter) - {"cpus"})
            elif op == "$mod_percent":
                if len(args) != 1 or not 1 <= integer(args[0]) <= 200:
                    raise ValueError("Invalid GPU governor percentage")
                item["gpu_percent"] = integer(args[0])
            else:
                # Keep unknown writes out of the kernel, and expose the exact
                # action names in the report instead of declaring full parity.
                if op.startswith("@cpu_freq"):
                    raise ValueError(f"Unsupported frequency action: {op}")
                unported.add(op)
        if idle:
            item["gpu_percent"] = 0  # Screen off releases our governor-file lease.
        for cpu, bounds in zip(item["cpus"], hardware):
            validate_limits(cpu["khz"])
            if cpu["khz"][1] > bounds[1]:
                raise ValueError("Scene CPU maximum exceeds hardware bounds")
            cpu["khz"][0] = max(cpu["khz"][0], bounds[0])
            validate_limits(cpu["khz"])
        blob = encode_profile(item, len(ids))
        for index, existing in enumerate(profiles):
            if blob == encode_profile(existing, len(ids)):
                return index
        if len(profiles) == 16:
            raise ValueError("Scene profile table exceeds 16 unique states")
        labels.append(name)
        profiles.append(item)
        return len(profiles) - 1

    off_limiter = settings.get("off_limiter", "idle")
    off = compile_actions([["@limiter", off_limiter]], "screen-off:" + off_limiter, idle=True)
    cache = {}

    def indices(mode, category):
        key = mode, category
        if key in cache:
            return cache[key]
        if mode in ("ignore", "igoned"):
            disabled = {"enabled": False}
            if disabled not in profiles:
                if len(profiles) == 16:
                    raise ValueError("Scene profile table full")
                profiles.append(disabled)
                labels.append("ignored")
            result = [profiles.index(disabled)] * 3
        else:
            if mode not in MODES:
                raise ValueError(f"Unknown Scene mode: {mode}")
            sidecar_name = "_camera.json" if category == "Camera" else \
                "_elp.json" if category in ("IM", "Music", "Reader", "ShortVideo", "Nav", "ScrollOpt") \
                else "_apps.json" if category == "normal" else None
            if sidecar_name is None:
                raise ValueError(f"Unknown explicit Scene category: {category}")
            result = []
            for state in ("active", "inactive"):
                commands = profile["schemes"][mode]["app"] + \
                    state_commands(sidecars[sidecar_name], mode, state)
                result.append(compile_actions(commands, f"{mode}:{category}:{state}"))
            result.append(off)
        cache[key] = result
        return result

    defaults = indices(default_mode, "normal")
    rules = []
    for package, value in sorted(settings.get("packages", {}).items()):
        if not isinstance(package, str) or not PACKAGE.fullmatch(package) or len(package) > 127:
            raise ValueError("Expected an exact ASCII package name, not a wildcard/activity selector")
        value = {"mode": value} if isinstance(value, str) else value
        if set(value) - {"mode", "category"}:
            raise ValueError("Unknown per-package Scene setting")
        rules.append({"package": package,
                      "profiles": indices(value.get("mode", default_mode), value.get("category", "normal"))})
    if len(rules) > 64:
        raise ValueError("Scene package table exceeds 64 entries")
    data = struct.pack("<12I", 1, len(rules), len(ids), len(profiles),
                       *ids, *([0] * (4 - len(ids))), *defaults, 0)
    data += b"".join(encode_profile(item, len(ids)) for item in profiles)
    data += bytes((16 - len(profiles)) * PROFILE_SIZE)
    for rule in rules:
        data += struct.pack("<128s3I", rule["package"].encode("ascii"), *rule["profiles"])
    data += bytes((64 - len(rules)) * 140)
    assert len(data) == TABLE_SIZE
    report = {
        "abi": 1, "size": len(data), "cpu_ids": ids, "defaults": defaults,
        "profiles": [{"label": label, **item} for label, item in zip(labels, profiles)],
        "rules": rules, "unported_action_names": sorted(unported),
        "unported_limiter_fields": sorted(unsupported_limiter_fields),
        "inactive_requires_existing_event_provider": True,
        "activity_overrides_require_existing_event_provider": True,
        "off_policy": "explicit limiter " + off_limiter,
        "game_branch_has_no_scene_profile": True,
        "table_sha256": hashlib.sha256(data).hexdigest(),
    }
    return data, report


def import_powercfg(path, known_packages):
    """Filter by an actual package inventory; never infer package vs activity."""
    root = ET.parse(path).getroot()
    result, skipped, default_mode = {}, [], None
    for entry in root:
        name, mode = entry.attrib.get("name", ""), (entry.text or "").strip()
        if entry.tag != "string":
            raise ValueError("Unsupported Scene preference entry")
        if name == "*":
            if mode not in MODES:
                raise ValueError("Unknown default Scene preference")
            if default_mode is not None:
                raise ValueError("Duplicate default Scene preference")
            default_mode = mode
            continue
        if name not in known_packages:
            skipped.append(name)
            continue
        if mode not in (*MODES, "ignore", "igoned"):
            raise ValueError("Unknown per-package Scene preference")
        if name in result:
            raise ValueError("Duplicate Scene package preference")
        result[name] = {"mode": mode}
    return result, skipped, default_mode


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--profile", required=True, type=Path)
    parser.add_argument("--sidecars", required=True, type=Path)
    parser.add_argument("--controls", required=True, type=Path)
    parser.add_argument("--settings", required=True, type=Path)
    parser.add_argument("--powercfg", type=Path)
    parser.add_argument("--known-packages", type=Path,
                        help="JSON list of actual installed package IDs, required with --powercfg")
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    load = lambda path: json.loads(path.read_text(encoding="utf-8"))
    settings = copy.deepcopy(load(args.settings))
    skipped_count = 0
    if args.powercfg:
        if not args.known_packages:
            parser.error("--powercfg requires --known-packages to exclude activity selectors")
        imported, skipped, mode = import_powercfg(args.powercfg, set(load(args.known_packages)))
        if mode is not None:
            settings.setdefault("mode", mode)
        skipped_count = len(skipped)
        imported.update(settings.get("packages", {}))
        settings["packages"] = imported
    sidecars = {name: load(args.sidecars / name) for name in ("_apps.json", "_elp.json", "_camera.json")}
    data, report = compile_scene_table(load(args.profile), sidecars, load(args.controls), settings)
    report["nonpackage_preferences_skipped"] = skipped_count
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_bytes(data)
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({key: report[key] for key in ("abi", "size", "cpu_ids", "table_sha256")}))


if __name__ == "__main__":
    main()
