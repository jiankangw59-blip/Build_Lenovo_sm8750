"""Share identical generated AppOpt profiles while preserving ordered rule expansion."""

import argparse
from collections import Counter
from itertools import groupby
import json
from pathlib import Path
import shutil


OTHER_SOURCES = (
    "template_rules.prop", "auto_rules.prop", "measured_game_rules.prop", "applist.prop"
)


def active_rows(path):
    return [
        row for line in path.read_text(encoding="utf-8").splitlines()
        if (row := line.strip()) and not row.startswith("#")
    ]


def selector(row):
    return row.split("=", 1)[0].split("{", 1)[0].rstrip()


def compact(rows):
    profiles = {}
    uses = []
    for name, group in groupby(rows, key=selector):
        if not name or any(char.isspace() for char in name):
            raise ValueError(f"Invalid process selector {name!r}")
        relative = tuple(row[len(name):] for row in group)
        profile = profiles.setdefault(relative, len(profiles) + 1)
        uses.append((profile, name))
    definitions = {profile: relative for relative, profile in profiles.items()}
    expanded = [name + rule for profile, name in uses for rule in definitions[profile]]
    if expanded != rows:
        raise AssertionError("Compacted profile changes rule content or order")
    header = [
        "# Shared generated app profiles. Edit applist.prop for per-app overrides.",
        "# @profile defines thread rules relative to a process. @use selects a process.",
    ]
    compacted = header.copy()
    for relative, profile in profiles.items():
        compacted.extend((f"@profile {profile}", *relative))
    compacted.extend(f"@use {profile} {name}" for profile, name in uses)
    return compacted, len(profiles), len(uses), sum(map(len, profiles))


def generate(source, output):
    targets = (*OTHER_SOURCES, "app_profile_rules.profiles", "compaction.json")
    if output.exists() and any((output / name).exists() for name in targets):
        raise FileExistsError(f"Output already contains rules: {output}")
    generated = active_rows(source / "app_profile_rules.prop")
    packed, profiles, selectors, rule_lines = compact(generated)
    preserved = {name: active_rows(source / name) for name in OTHER_SOURCES}
    rows = generated + [row for group in preserved.values() for row in group]
    normalized = [
        (key.strip(), mask.split("#", 1)[0].strip())
        for row in rows for key, mask in [row.split("=", 1)]
    ]
    exact_repeat = len(normalized) - len(Counter(normalized))
    distinct_selectors = len({key for key, _ in normalized})
    data = {
        "expanded_total_rules": len(rows),
        "generated_rules": len(generated),
        "shared_profiles": profiles,
        "profile_rule_definitions": rule_lines,
        "process_references": selectors,
        "compact_data_lines": len(rows) - len(generated) + rule_lines + selectors + profiles,
        "identical_repeated_rows": exact_repeat,
        "conflicting_repeated_rows": len(rows) - exact_repeat - distinct_selectors,
        "expansion_matches_in_order": True,
    }
    output.mkdir(parents=True, exist_ok=True)
    for name in OTHER_SOURCES:
        shutil.copyfile(source / name, output / name)
    (output / "app_profile_rules.profiles").write_text(
        "\n".join(packed) + "\n", encoding="utf-8", newline="\n"
    )
    summary = json.dumps(data, indent=2) + "\n"
    (output / "compaction.json").write_text(summary, encoding="utf-8", newline="\n")
    print(summary, end="")


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("source", type=Path)
    cli.add_argument("output", type=Path)
    args = cli.parse_args()
    generate(args.source, args.output)
