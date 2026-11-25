"""Insert the thread affinity backend into the reviewed OPD2413 tree."""

import argparse
import shutil
from pathlib import Path


HERE = Path(__file__).resolve().parent
CORE_SEAM = "\nlong sched_setaffinity(pid_t pid, const struct cpumask *in_mask)\n"
CORE_INCLUDE = '#include "ace6_sched_affinity.inc"'
KBUILD = "obj-y += ace6_thread_affinity.o"
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
    "ace6_cpuset_affinity.h",
    "affinity_rules.h",
    "adaptive_policy.h",
    "ace6_sched_affinity.h",
    "ace6_sched_affinity.inc",
)


def install(kernel):
    top = (kernel / "Makefile").read_text(encoding="utf-8").splitlines()
    if not {"VERSION = 6", "PATCHLEVEL = 6", "SUBLEVEL = 89"}.issubset(top):
        raise SystemExit("Thread affinity backend requires the reviewed OPD2413 6.6.89 tree")
    sched = kernel / "kernel" / "sched"
    core = sched / "core.c"
    kbuild = sched / "Makefile"
    cpuset = kernel / "kernel" / "cgroup" / "cpuset.c"
    core_text = core.read_text(encoding="utf-8")
    kbuild_text = kbuild.read_text(encoding="utf-8")
    cpuset_text = cpuset.read_text(encoding="utf-8")
    if core_text.count(CORE_SEAM) != 1:
        raise SystemExit("Cannot locate the pinned scheduler affinity seam")
    if (CORE_INCLUDE in core_text) != (KBUILD in kbuild_text):
        raise SystemExit("Incomplete prior thread affinity integration")
    if (CPUSET_INCLUDE in cpuset_text) != (CPUSET_NOTIFY in cpuset_text):
        raise SystemExit("Incomplete prior cpuset notification integration")
    if CPUSET_INCLUDE not in cpuset_text:
        if cpuset_text.count(CPUSET_SEAM) != 1 or cpuset_text.count(CPUSET_ATTACH) != 1:
            raise SystemExit("Cannot locate the pinned cpuset migration seam")
        cpuset_text = cpuset_text.replace(CPUSET_SEAM, CPUSET_SEAM + "\n" + CPUSET_INCLUDE)
        cpuset_text = cpuset_text.replace(CPUSET_ATTACH, CPUSET_NOTIFY)

    for name in SOURCES:
        shutil.copyfile(HERE / name, sched / name)
    cpuset.write_text(cpuset_text, encoding="utf-8", newline="\n")
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
