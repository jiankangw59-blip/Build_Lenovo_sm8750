"""Insert the optional thread affinity backend into a OnePlus 6.6.118 source tree."""

import argparse
import shutil
from pathlib import Path


HERE = Path(__file__).resolve().parent
CORE_SEAM = "\nlong sched_setaffinity(pid_t pid, const struct cpumask *in_mask)\n"
CORE_INCLUDE = '#include "ace6_sched_affinity.inc"'
KBUILD = "obj-y += ace6_thread_affinity.o"
SOURCES = (
    "ace6_thread_affinity.c",
    "affinity_rules.h",
    "adaptive_policy.h",
    "scene_context_wire.h",
    "ace6_sched_affinity.h",
    "ace6_sched_affinity.inc",
)


def install(kernel):
    top = (kernel / "Makefile").read_text(encoding="utf-8").splitlines()
    if not {"VERSION = 6", "PATCHLEVEL = 6", "SUBLEVEL = 118"}.issubset(top):
        raise SystemExit("Thread affinity backend requires the reviewed 6.6.118 tree")
    sched = kernel / "kernel" / "sched"
    core = sched / "core.c"
    kbuild = sched / "Makefile"
    core_text = core.read_text(encoding="utf-8")
    kbuild_text = kbuild.read_text(encoding="utf-8")
    if core_text.count(CORE_SEAM) != 1:
        raise SystemExit("Cannot locate the pinned scheduler affinity seam")
    if (CORE_INCLUDE in core_text) != (KBUILD in kbuild_text):
        raise SystemExit("Incomplete prior thread affinity integration")

    for name in SOURCES:
        shutil.copyfile(HERE / name, sched / name)
    shutil.copyfile(HERE.parent / "frame_policy" / "frame_policy.h", sched / "frame_policy.h")
    if CORE_INCLUDE not in core_text:
        core.write_text(
            core_text.replace(CORE_SEAM, f"\n{CORE_INCLUDE}\n" + CORE_SEAM),
            encoding="utf-8",
            newline="\n",
        )
        kbuild.write_text(kbuild_text + f"\n{KBUILD}\n", encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    install(parser.parse_args().kernel)
