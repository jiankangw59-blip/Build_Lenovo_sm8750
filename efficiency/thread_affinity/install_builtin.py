"""Insert the built-in affinity backend for the selected OEM kernel."""

import argparse
import shutil
from pathlib import Path


HERE = Path(__file__).resolve().parent
CORE_SEAM = "\nlong sched_setaffinity(pid_t pid, const struct cpumask *in_mask)\n"
CORE_INCLUDE = '#include "ace6_sched_affinity.inc"'
KBUILD = "obj-y += ace6_thread_affinity.o"
OPD_CPUSET_CFLAGS = "CFLAGS_ace6_thread_affinity.o += -DACE6_OPD2413_CPUSET_TRACE"
CPUSET_SEAM = "#include <trace/hooks/sched.h>\n"
CPUSET_INCLUDE = '#include "../sched/ace6_cpuset_affinity.h"\n'
CPUSET_ATTACH = (
    "\tcgroup_taskset_for_each(task, css, tset)\n"
    "\t\tcpuset_attach_task(cs, task);\n"
)
CPUSET_NOTIFY = (
    "\tcgroup_taskset_for_each(task, css, tset) {\n"
    "\t\tcpuset_attach_task(cs, task);\n"
    "\t\ttrace_ace6_cpuset_attach_task(task);\n"
    "\t}\n"
)
SOURCES = (
    "ace6_thread_affinity.c",
    "affinity_rules.h",
    "adaptive_policy.h",
    "scene_context_wire.h",
    "ace6_sched_affinity.h",
    "ace6_sched_affinity.inc",
)


def opd_cpuset_source(path):
    source = path.read_text(encoding="utf-8")
    if (CPUSET_INCLUDE in source) != (CPUSET_NOTIFY in source):
        raise SystemExit("Incomplete prior cpuset notification integration")
    if CPUSET_INCLUDE in source:
        return source
    if source.count(CPUSET_SEAM) != 1 or source.count(CPUSET_ATTACH) != 1:
        raise SystemExit("Cannot locate the pinned cpuset migration seam")
    return source.replace(CPUSET_SEAM, CPUSET_SEAM + "\n" + CPUSET_INCLUDE).replace(CPUSET_ATTACH, CPUSET_NOTIFY)


def install(kernel, device="ace6"):
    top = (kernel / "Makefile").read_text(encoding="utf-8").splitlines()
    sublevel = {"ace6": 118, "opd2413": 89}[device]
    if not {"VERSION = 6", "PATCHLEVEL = 6", f"SUBLEVEL = {sublevel}"}.issubset(top):
        raise SystemExit(f"Thread affinity backend requires the reviewed {device} 6.6.{sublevel} tree")
    sched = kernel / "kernel" / "sched"
    core = sched / "core.c"
    kbuild = sched / "Makefile"
    core_text = core.read_text(encoding="utf-8")
    kbuild_text = kbuild.read_text(encoding="utf-8")
    if core_text.count(CORE_SEAM) != 1:
        raise SystemExit("Cannot locate the pinned scheduler affinity seam")
    if (CORE_INCLUDE in core_text) != (KBUILD in kbuild_text):
        raise SystemExit("Incomplete prior thread affinity integration")
    if device == "opd2413":
        cpuset = kernel / "kernel/cgroup/cpuset.c"
        cpuset_text = opd_cpuset_source(cpuset)

    for name in SOURCES:
        shutil.copyfile(HERE / name, sched / name)
    shutil.copyfile(HERE.parent / "frame_policy" / "frame_policy.h", sched / "frame_policy.h")
    if device == "opd2413":
        shutil.copyfile(HERE / "ace6_cpuset_affinity.h", sched / "ace6_cpuset_affinity.h")
        cpuset.write_text(cpuset_text, encoding="utf-8", newline="\n")
        if OPD_CPUSET_CFLAGS not in kbuild_text:
            kbuild_text += f"\n{OPD_CPUSET_CFLAGS}\n"
    if CORE_INCLUDE not in core_text:
        core.write_text(
            core_text.replace(CORE_SEAM, f"\n{CORE_INCLUDE}\n" + CORE_SEAM),
            encoding="utf-8",
            newline="\n",
        )
        kbuild_text += f"\n{KBUILD}\n"
    kbuild.write_text(kbuild_text, encoding="utf-8", newline="\n")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--kernel", required=True, type=Path)
    parser.add_argument("--device", choices=("ace6", "opd2413"), default="ace6")
    args = parser.parse_args()
    install(args.kernel, args.device)
