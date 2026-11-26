"""Exercise the production packet decoder without opening a policy device."""
import argparse
import json
from pathlib import Path
import struct
import subprocess
import tempfile

HERE = Path(__file__).resolve().parent


def main(controller):
    settings = (HERE / "defaults/settings.json").read_bytes()
    config = (HERE / "defaults/config.bin").read_bytes()
    rules = (HERE / "defaults/games.bin").read_bytes()
    scene = (HERE / "defaults/scene.bin").read_bytes()
    payload = struct.pack("<7I", 0x46505331, len(settings), len(config), len(rules), len(scene), 0, 0)
    payload += settings + config + rules + scene
    with tempfile.TemporaryDirectory(prefix="frame-settings-") as temporary:
        parent = Path(temporary)
        good = parent / "good"
        good.mkdir()
        result = subprocess.run([str(controller), "unpack-settings", str(good)], input=payload, capture_output=True)
        assert result.returncode == 0, result.stderr
        assert (good / "settings.json").read_bytes() == settings
        assert (good / "config.bin").read_bytes() == config
        assert (good / "games.bin").read_bytes() == rules
        assert (good / "scene.bin").read_bytes() == scene
        assert (good / "enabled").read_bytes() == b"0\n"
        assert (good / "scene-required").read_bytes() == b"0\n"
        json.loads((good / "settings.json").read_text(encoding="utf-8"))
        for file in good.iterdir():
            assert file.stat().st_mode & 0o777 == 0o600
        assert subprocess.run([str(controller), "unpack-settings", str(good)], input=payload,
                              capture_output=True).returncode != 0
        cases = [payload[:-1], payload + b"unexpected",
                 struct.pack("<7I", 0x46505331, 49153, 592, 7556, 14384, 0, 0),
                 payload[:20] + struct.pack("<I", 2) + payload[24:],
                 struct.pack("<I", 0) + payload[4:]]
        for index, invalid in enumerate(cases):
            target = parent / str(index)
            target.mkdir()
            assert subprocess.run([str(controller), "unpack-settings", str(target)], input=invalid,
                                  capture_output=True).returncode != 0
            assert not list(target.iterdir())
        link = parent / "link"
        link.symlink_to(good, target_is_directory=True)
        assert subprocess.run([str(controller), "unpack-settings", str(link)], input=payload,
                              capture_output=True).returncode != 0
    print("Settings decoder: exact bytes, private files, invalid packets and symlinks passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--controller", type=Path, required=True)
    main(parser.parse_args().controller.resolve())
