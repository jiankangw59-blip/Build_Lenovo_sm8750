"""Inspect packaged tuning/provenance using an explicitly synthetic ELF fixture."""
import json
from pathlib import Path
import struct
import tempfile
import unittest
import zipfile

from package import package


class Package(unittest.TestCase):
    def test_default_fas_is_present_once_and_has_matching_provenance(self):
        with tempfile.TemporaryDirectory(prefix="frame-module-package-") as temporary:
            root = Path(temporary)
            # This fixture only exercises the packager, never device execution.
            elf = bytearray(64)
            elf[:6] = b"\x7fELF\x02\x01"
            struct.pack_into("<H", elf, 18, 183)
            controller, output = root / "synthetic-controller", root / "module.zip"
            controller.write_bytes(elf)
            package(controller, output)
            with zipfile.ZipFile(output) as archive:
                self.assertEqual(archive.namelist().count("defaults/fas.bin"), 1)
                fas = archive.read("defaults/fas.bin")
                self.assertEqual(fas, struct.pack("<4I", 1, 1, 1000, 0) + bytes(64))
                self.assertEqual(json.loads(archive.read("defaults/settings.json"))["fasMode"], "balance")
                metadata = json.loads(archive.read("build-provenance.json"))
                self.assertEqual(metadata["fas_tuning_abi"], 1)
                self.assertEqual(metadata["settings_envelope"], 3)
                self.assertEqual(metadata["scene_aux_bytes"], 26584)
                self.assertEqual(len(archive.read("defaults/aux.bin")), 26584)
                self.assertEqual(archive.read("defaults/aux-required"), b"0\n")
                self.assertFalse(metadata["resident_process_added"])
                module = archive.read("module.prop").splitlines()
                self.assertIn(b"version=0.3.5-test", module)
                self.assertIn(b"versionCode=305", module)
                self.assertIn(b"load-fas", archive.read("config.sh"))


if __name__ == "__main__":
    unittest.main(verbosity=2)
