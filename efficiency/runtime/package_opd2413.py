"""Build the OPD2413 boot policy module for one verified kernel."""
import argparse
import hashlib
from pathlib import Path
from zipfile import ZIP_DEFLATED, ZipFile


HERE = Path(__file__).resolve().parent
CRYSTAL = HERE.parent / "scene_crystal"
MODULE_PROP = """id=opd2413_kernel_efficiency
name=OPD2413 Kernel Efficiency
version=1.1
versionCode=2
author=Codex
description=ZRAM, Crystal kernel writeback, workqueue and TCP settings for the enrolled OPD2413 kernel.
"""


def main():
    args = argparse.ArgumentParser(description=__doc__)
    args.add_argument("--notes", type=Path, required=True)
    args.add_argument("--traffic-binder", type=Path, required=True)
    args.add_argument("--output", type=Path, required=True)
    options = args.parse_args()
    notes = options.notes.read_bytes()
    if not notes:
        raise ValueError("The kernel notes section is empty")
    binder = options.traffic_binder.read_bytes()
    if binder[:6] != b"\x7fELF\x02\x01" or binder[18:20] != b"\xb7\x00":
        raise ValueError("Expected an ARM64 traffic binder")
    sources = {
        "enrolled.sh": HERE / "enrolled.sh",
        "post-fs-data.sh": HERE / "post-fs-data.sh",
        "service.sh": HERE / "opd2413-service.sh",
        "action.sh": HERE / "opd2413-action.sh",
        "apply_network_and_workqueue.sh": HERE / "service.sh",
        "customize.sh": HERE / "customize.sh",
        "provision_crystal_backing.sh": HERE / "provision_crystal_backing.sh",
        "activate_xcy.sh": HERE / "activate_xcy.sh",
        "prepare_xcy_devices.sh": HERE / "prepare_xcy_devices.sh",
        "xcy_sepolicy.rule": HERE / "xcy_sepolicy.rule",
        "defaults/swap_config.conf": HERE / "swap_config.conf",
        "early_crystal_writeback.sh": CRYSTAL / "early_crystal_writeback.sh",
        "start_crystal_kernel_writeback.sh": CRYSTAL / "start_crystal_kernel_writeback.sh",
        "crystal_writeback.sh": CRYSTAL / "crystal_writeback.sh",
    }
    options.output.parent.mkdir(parents=True, exist_ok=True)
    with ZipFile(options.output, "w", ZIP_DEFLATED) as module:
        module.writestr("module.prop", MODULE_PROP)
        module.writestr("kernel.sha256", hashlib.sha256(notes).hexdigest() + "\n")
        module.writestr("bind_traffic", binder)
        for name, path in sources.items():
            module.writestr(name, path.read_bytes().replace(b"\r\n", b"\n"))


if __name__ == "__main__":
    main()
