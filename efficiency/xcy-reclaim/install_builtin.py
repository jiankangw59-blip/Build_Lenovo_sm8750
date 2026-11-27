"""Attach the reviewed XiaoCaiYe sources to the selected OEM kernel."""
import argparse
import hashlib
import json
from pathlib import Path
import shutil
import subprocess


HERE = Path(__file__).resolve().parent


def insert_once(path, text):
    current = path.read_text(encoding="utf-8")
    if text not in current.splitlines():
        path.write_text(current.rstrip("\n") + "\n" + text + "\n", encoding="utf-8")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--report", type=Path)
    parser.add_argument("--device", choices=("ace6", "opd2413"), default="ace6")
    args = parser.parse_args()
    kernel = args.kernel.resolve()
    version = subprocess.check_output(["make", "-s", "-C", str(kernel), "kernelversion"], text=True).strip()
    expected_version = {"ace6": "6.6.118", "opd2413": "6.6.89"}[args.device]
    if version != expected_version:
        raise SystemExit(f"Expected the pinned {args.device} {expected_version} kernel, got {version}")
    mm = kernel / "mm"
    target = mm / "xcy-reclaim"
    target.mkdir(exist_ok=True)
    inputs = [HERE / "Kconfig", HERE / "Makefile", *sorted(HERE.glob("xcy_reclaim*.[ch]"))]
    for path in inputs:
        shutil.copyfile(path, target / path.name)
    insert_once(mm / "Kconfig", 'source "mm/xcy-reclaim/Kconfig"')
    insert_once(mm / "Makefile", "obj-$(CONFIG_XCY_RECLAIM) += xcy-reclaim/")
    subprocess.run([str(kernel / "scripts/config"), "--file",
                    "arch/arm64/configs/gki_defconfig", "--enable", "XCY_RECLAIM"],
                   cwd=kernel, check=True)
    if args.report:
        args.report.parent.mkdir(parents=True, exist_ok=True)
        args.report.write_text(json.dumps({
            "kernelversion": version,
            "sources": {path.name: hashlib.sha256(path.read_bytes()).hexdigest() for path in inputs},
        }, indent=2) + "\n", encoding="utf-8")


if __name__ == "__main__":
    main()
