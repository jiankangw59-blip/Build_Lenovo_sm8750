"""Package the KernelSU affinity editor with an ARM64 controller and rule set."""

import argparse
from pathlib import Path
import zipfile


HERE = Path(__file__).resolve().parent
RULES = (
    "template_rules.prop",
    "app_profile_rules.prop",
    "app_profile_rules.profiles",
    "auto_rules.prop",
    "measured_game_rules.prop",
    "applist.prop",
)


def package(controller, rules, output):
    binary = controller.read_bytes()
    if binary[:6] != b"\x7fELF\x02\x01" or binary[18:20] != b"\xb7\x00":
        raise ValueError("affinityctl must be a little-endian ARM64 ELF binary")
    if not (rules / "applist.prop").is_file():
        raise FileNotFoundError(rules / "applist.prop")

    text_files = {name: HERE / name for name in ("module.prop", "action.sh", "config.sh", "customize.sh")}
    text_files["service.sh"] = HERE.parent / "boot-service.sh"
    text_files.update({path.relative_to(HERE).as_posix(): path for path in (HERE / "webroot").iterdir() if path.is_file()})
    for name in RULES:
        if (rules / name).is_file():
            text_files[f"rules/{name}"] = rules / name

    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as module:
        module.writestr("affinityctl", binary)
        for name, path in sorted(text_files.items()):
            module.writestr(name, path.read_bytes().replace(b"\r\n", b"\n"))


if __name__ == "__main__":
    cli = argparse.ArgumentParser(description=__doc__)
    cli.add_argument("--controller", type=Path, required=True)
    cli.add_argument("--rules", type=Path, required=True)
    cli.add_argument("--output", type=Path, required=True)
    args = cli.parse_args()
    package(args.controller, args.rules, args.output)
