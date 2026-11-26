"""Compile the installed FAS extension's static per-game frequency relationships."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import struct
import tomllib

MAX_CPUS, MAX_RULES = 4, 32


def extension_relations(lua, cpus):
    start = re.search(r"^function set_default_extra_policy\([^\n]*\)\s*$", lua, re.M)
    if not start:
        raise ValueError("The extension has no supported static policy function")
    body = lua[start.end():]
    end = re.search(r"^function ", body, re.M)
    if end:
        body = body[:end.start()]
    fallback, rules, packages = {}, {}, None
    for raw in body.splitlines():
        line = raw.split("--", 1)[0].strip()
        if not line or line in ("end", "if extra_policy then"):
            continue
        if line == "else":
            packages = None
            continue
        if re.fullmatch(r"remove_extra_policy\(\d+\)", line):
            continue  # Disabling the kernel controller removes all its votes.
        condition = re.fullmatch(r"(?:if|elseif)\s+(.+)\s+then", line)
        if condition:
            packages = []
            for part in re.split(r"\s+or\s+", condition[1]):
                match = re.fullmatch(r'pkg\s*==\s*"([A-Za-z0-9_.]+)"', part)
                if not match:
                    raise ValueError("Unsupported dynamic package condition")
                packages.append(match[1])
            continue
        call = re.fullmatch(r"set_extra_policy_rel\(\s*(\d+),\s*(\d+),\s*(-?\d+),\s*(-?\d+)\s*\)", line)
        if not call:
            raise ValueError("Unsupported active Lua statement: " + line)
        cpu, reference, minimum, maximum = map(int, call.groups())
        if cpu not in cpus or reference not in cpus or not -10000000 <= minimum <= maximum <= 10000000:
            raise ValueError("Relation does not match the provided CPU policies")
        value = (2, cpus.index(reference), minimum, maximum)
        if packages is None:
            fallback[cpus.index(cpu)] = value
        else:
            for package in packages:
                rules.setdefault(package, {})[cpus.index(cpu)] = value
    if not rules and not fallback:
        raise ValueError("No static FAS frequency relationships were found")
    return fallback, rules


def validate_graph(relations, count):
    finished = set()
    while len(finished) < count:
        before = len(finished)
        for cpu in range(count):
            kind, reference, _, _ = relations.get(cpu, (0, 0, 0, 0))
            if kind != 2 or reference in finished:
                finished.add(cpu)
        if len(finished) == before:
            raise ValueError("Cyclic CPU frequency relationships")


def compile_rules(lua, cpus, target_fps=None):
    if not 1 <= len(cpus) <= MAX_CPUS or len(set(cpus)) != len(cpus) or any(
            type(cpu) is not int or not 0 <= cpu < 4096 for cpu in cpus):
        raise ValueError("Expected one to four distinct CPU policy IDs")
    fallback, rules = extension_relations(lua, cpus)
    target_fps = target_fps or {}
    rates = {}
    for package, values in target_fps.items():
        values = [values] if type(values) is int else values
        if (not re.fullmatch(r"[A-Za-z0-9_.]+", package) or type(values) is not list or
                not 1 <= len(values) <= 8 or values != sorted(set(values)) or
                any(type(value) is not int or not 15 <= value <= 240 for value in values)):
            raise ValueError("Invalid per-game target FPS: " + package)
        rates[package] = values
        if package not in rules:
            rules[package] = dict(fallback)
    common = next(iter(rates.values())) if rates and len({tuple(v) for v in rates.values()}) == 1 else []
    if len(rules) > MAX_RULES:
        raise ValueError("Too many per-game exceptions")

    def pack_rule(package, relations, fps):
        encoded = package.encode("ascii")
        if len(encoded) >= 128:
            raise ValueError("Package name is too long")
        validate_graph(relations, len(cpus))
        data = struct.pack("<128s", encoded)
        for cpu in range(MAX_CPUS):
            data += struct.pack("<IIii", *relations.get(cpu, (0, 0, 0, 0)))
        data += struct.pack("<9I", len(fps), *fps, *([0] * (8 - len(fps))))
        return data

    data = struct.pack("<8I", 1, len(rules), len(cpus), 0, *cpus,
                       *([0] * (MAX_CPUS - len(cpus))))
    data += pack_rule("", fallback, common)
    for package in sorted(rules):
        data += pack_rule(package, rules[package], rates.get(package, common))
    data += bytes((MAX_RULES - len(rules)) * 228)
    assert len(data) == 7556
    report = {"abi": 1, "cpu_ids": cpus, "fallback": fallback, "rules": rules,
              "source_sha256": hashlib.sha256(lua.encode()).hexdigest(),
              "fps_by_package": rates, "fallback_fps": common,
              "configuration_sha256": hashlib.sha256(data).hexdigest(),
              "cpu_hotplug_changed": False,
              "constraints_scope": "game frequency targets before hardware OPP rounding"}
    return data, report


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--lua", type=Path, required=True)
    parser.add_argument("--controls", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--standard-games", type=Path)
    parser.add_argument("--user-games", type=Path)
    args = parser.parse_args()
    controls = json.loads(args.controls.read_text(encoding="utf-8"))
    standard = tomllib.loads(args.standard_games.read_text(encoding="utf-8")) if args.standard_games else {}
    user = tomllib.loads(args.user_games.read_text(encoding="utf-8")) if args.user_games else {}
    games = dict(standard.get("game_list", {})) if user.get("config", {}).get("keep_std", True) else {}
    games.update(user.get("game_list", {}))
    data, report = compile_rules(args.lua.read_text(encoding="utf-8"), [p["cpu"] for p in controls["cpus"]], games)
    args.output.write_bytes(data)
    args.output.with_suffix(".json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))
