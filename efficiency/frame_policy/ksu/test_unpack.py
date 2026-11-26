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
    fas = struct.pack("<4I", 1, 1, 1000, 0) + bytes(64)
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
        assert (good / "fas.bin").read_bytes() == fas
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
        envelope2 = struct.pack("<8I", 0x46505332, len(settings), len(config), len(rules), len(scene), 80, 1, 0)
        content = settings + config + rules + scene
        for mode in range(4):
            selected = struct.pack("<4I", 1, mode, [3000, 1000, 300, 0][mode], 1)
            selected += struct.pack("<2I", 165, 300) + bytes(56)
            destination = parent / ("v2-mode-" + str(mode))
            destination.mkdir()
            assert subprocess.run([str(controller), "unpack-settings", str(destination)],
                                  input=envelope2 + content + selected, capture_output=True).returncode == 0
            assert (destination / "fas.bin").read_bytes() == selected
            assert (destination / "settings.json").read_bytes() == settings
            assert (destination / "enabled").read_bytes() == b"1\n"
        valid2 = envelope2 + content + fas
        invalid_fas = []
        for offset, value in ((0, 2), (4, 4), (8, 30001), (12, 9), (76, 1)):
            changed = bytearray(fas)
            struct.pack_into("<I", changed, offset, value)
            invalid_fas.append(bytes(changed))
        invalid_fas.append(struct.pack("<4I2I", 1, 1, 1000, 1, 15, 11001) + bytes(56))
        cases += [valid2[:-1], valid2 + b"unexpected", valid2[:20] + struct.pack("<I", 79) + valid2[24:],
                  valid2[:24] + struct.pack("<I", 2) + valid2[28:],
                  valid2[:28] + struct.pack("<I", 2) + valid2[32:]]
        cases += [envelope2 + content + value for value in invalid_fas]
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
    print("Settings decoder: FPS1 legacy defaults, FPS2 four FAS modes, exact/private files and invalid packets passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--controller", type=Path, required=True)
    main(parser.parse_args().controller.resolve())
