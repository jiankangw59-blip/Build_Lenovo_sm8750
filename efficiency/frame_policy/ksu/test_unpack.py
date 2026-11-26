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
        assert len((good / "aux.bin").read_bytes()) == 26584
        assert (good / "aux-required").read_bytes() == b"0\n"
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
        # A typed, metadata-only table needs no sysfs or policy device to decode.
        aux = bytearray(26584)
        struct.pack_into("<4I", aux, 0, 1, 0, 1, 1)
        struct.pack_into("<28i", aux, 56, *([-1] * 28))
        envelope3 = struct.pack("<10I", 0x46505333, len(settings), 592, 7556, 14384, 80, 26584, 1, 1, 1)
        valid3 = envelope3 + content + fas + aux
        target3 = parent / "v3"
        target3.mkdir()
        assert subprocess.run([str(controller), "unpack-settings", str(target3)], input=valid3, capture_output=True).returncode == 0
        assert (target3 / "aux.bin").read_bytes() == aux
        assert (target3 / "aux-required").read_bytes() == b"1\n"
        # Exercise the actual browser/CLI encoder -> actual production C decoder,
        # including complete feedback/options and the independent GPU override.
        for gpu_lock in (True, False):
            javascript = r'''
const fs=require('fs'),p=require('./webroot/protocol.js').frameSettingsProtocol;
const load=n=>JSON.parse(fs.readFileSync('./defaults/'+n+'.json','utf8'));
const settings={...load('settings'),sceneAux:true,sceneGpuLock:process.argv[1]==='true'};
process.stdout.write(Buffer.from(p(settings,load('seed'),load('scene-seed'),load('scene-aux-seed')).payload));
'''
            encoded = subprocess.run(["node", "-e", javascript, str(gpu_lock).lower()], cwd=HERE,
                                     capture_output=True, check=True, timeout=15).stdout
            destination=parent/("encoder-gpu-"+str(gpu_lock));destination.mkdir()
            decoded=subprocess.run([str(controller),"unpack-settings",str(destination)],input=encoded,capture_output=True)
            assert decoded.returncode==0,decoded.stderr
            for name,value in (("config.bin",config),("games.bin",rules),("scene.bin",scene),("fas.bin",fas)):
                assert (destination/name).read_bytes()==value,name
            policy=(destination/"aux.bin").read_bytes()
            assert len(policy)==26584 and struct.unpack_from("<I",policy,168)==(2047,)
            assert struct.unpack_from("<I",policy,212)==(int(gpu_lock),)
            assert struct.unpack_from("<Q",policy,48)==(0,)
            assert struct.unpack_from("<28i",policy,56)==tuple([-1]*28)
            imported=json.loads((destination/"settings.json").read_text())
            assert imported["sceneAux"] is True and imported["sceneGpuLock"] is gpu_lock
            assert (destination/"aux-required").read_bytes()==b"1\n"
        for offset, value in ((0, 2), (44, 1), (48, 1), (56, 0), (216, 32), (17624, 1)):
            bad = bytearray(aux)
            struct.pack_into("<I", bad, offset, value)
            cases.append(envelope3 + content + fas + bad)
        cases += [valid3[:-1], valid3 + b"x", valid3[:24] + struct.pack("<I", 26583) + valid3[28:],
                  valid3[:36] + struct.pack("<I", 0) + valid3[40:]]
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
    print("Settings decoder: FPS1/FPS2 compatibility, production FPS3 encoder/GPU override, exact files and invalid packets passed")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--controller", type=Path, required=True)
    main(parser.parse_args().controller.resolve())
