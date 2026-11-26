"""Package the config-only KernelSU front end with a verified ARM64 client."""
import argparse
import hashlib
import json
from pathlib import Path
import struct
import zipfile

HERE = Path(__file__).resolve().parent


def package(controller, output):
    binary = controller.read_bytes()
    if binary[:6] != b"\x7fELF\x02\x01" or struct.unpack_from("<H", binary, 18)[0] != 183:
        raise ValueError("Expected an ARM64 Android controller")
    output.parent.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(output, "w", compression=zipfile.ZIP_DEFLATED) as archive:
        archive.writestr("framectl", binary)
        for name in ("module.prop", "customize.sh", "service.sh", "action.sh", "config.sh", "uninstall.sh"):
            archive.writestr(name, (HERE / name).read_bytes().replace(b"\r\n", b"\n"))
        for directory in ("webroot", "defaults"):
            for path in sorted((HERE / directory).iterdir()):
                if path.is_file():
                    archive.writestr(directory + "/" + path.name, path.read_bytes())
        archive.writestr("build-provenance.json", json.dumps({
            "controller_sha256": hashlib.sha256(binary).hexdigest(), "frame_abi": 4,
            "game_rules_abi": 1, "default_enabled": False, "resident_process_added": False,
            "scene_table_abi": 1,
        }, indent=2) + "\n")
    print(output)


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--controller", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()
    package(args.controller, args.output)
