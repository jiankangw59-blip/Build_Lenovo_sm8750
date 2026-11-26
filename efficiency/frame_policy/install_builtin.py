"""Install the frame policy without changing exported kernel structure layouts."""
import argparse
import shutil
from pathlib import Path

HERE = Path(__file__).resolve().parent


def install(kernel, for_kunit=False):
    version = (kernel / "Makefile").read_text(encoding="utf-8")
    if "VERSION = 6\n" not in version or "PATCHLEVEL = 6\n" not in version:
        raise SystemExit("This port requires a 6.6 ARM64 kernel")
    root = kernel / "kernel/sched"
    if not for_kunit and not (root / "ace6_thread_affinity.c").is_file():
        raise SystemExit("Install the reviewed thread affinity and Scene event backend first")
    destination = root / "frame_policy"
    destination.mkdir(exist_ok=True)
    for name in ("Kconfig", "Makefile", "frame_policy.c", "frame_control.h", "frame_policy_uapi.h",
                 "frame_policy_kunit.h", "frame_rate.h"):
        shutil.copyfile(HERE / name, destination / name)
    shutil.copyfile(HERE / "frame_policy.h", root / "frame_policy.h")
    additions = {
        kernel / "init/Kconfig": 'source "kernel/sched/frame_policy/Kconfig"',
        root / "Makefile": "obj-$(CONFIG_FRAME_POLICY) += frame_policy/",
        kernel / "arch/arm64/configs/gki_defconfig": "CONFIG_FRAME_POLICY=y",
    }
    for path, line in additions.items():
        text = path.read_text(encoding="utf-8")
        if line not in text.splitlines():
            path.write_text(text.rstrip() + "\n" + line + "\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--for-kunit", action="store_true",
                        help="Install without the Android event provider for isolated kernel tests")
    args = parser.parse_args()
    install(args.kernel, args.for_kunit)
