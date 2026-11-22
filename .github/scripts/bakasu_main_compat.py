#!/usr/bin/env python3
"""Retain the accepted SELinux integration while importing other official main updates."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess

MAIN = "2734e0bc155759955880f5fb99be640fc5b92156"
SELINUX_BASE = "7b65bde0710b5e7c6f755545cb15a4abd417f3a8"
RESTORE = (
    "kernel/selinux/rules.c", "kernel/selinux/sepolicy.c", "kernel/selinux/sepolicy.h",
    "kernel/selinux/inode.c", "kernel/selinux/selinux.h",
    "kernel/feature/selinux_hide.c", "kernel/feature/selinux_hide.h",
    "kernel/hook/lsm_hooks.c", "kernel/core/init.c", "kernel/runtime/boot_event.c",
    "kernel/tools/static_export_check.mk",
)
REMOVE = tuple("kernel/feature/selinux_hide/" + n for n in (
    "selinux_hide.c", "selinux_hide.h", "selinux_hide_auto.c", "selinux_hide_susfs.c"))


def git(source, *args):
    return subprocess.check_output(["git", "-C", str(source), *args])


def apply(source):
    source = source.resolve()
    if git(source, "rev-parse", "HEAD").decode().strip() != MAIN:
        raise ValueError("Unexpected official main source revision")
    for rel in RESTORE:
        target = source / rel
        target.parent.mkdir(parents=True, exist_ok=True)
        target.write_bytes(git(source, "show", SELINUX_BASE + ":" + rel))
    subprocess.run(["git", "-C", str(source), "add", "--intent-to-add", "--",
                    "kernel/feature/selinux_hide.c", "kernel/feature/selinux_hide.h"], check=True)
    for rel in REMOVE:
        target = source / rel
        if target.is_file():
            target.unlink()
    path = source / "kernel/Kbuild"
    text = path.read_text(encoding="utf-8")
    old = "feature/selinux_hide/selinux_hide.o"
    if text.count(old) != 1:
        raise ValueError("Official SELinux object selection changed")
    path.write_text(text.replace(old, "feature/selinux_hide.o"), encoding="utf-8", newline="\n")
    path = source / "kernel/runtime/ksud_integration.c"
    text = path.read_text(encoding="utf-8")
    old = '#include "feature/selinux_hide/selinux_hide.h"'
    if text.count(old) != 1:
        raise ValueError("Runtime SELinux include changed")
    path.write_text(text.replace(old, '#include "feature/selinux_hide.h"'), encoding="utf-8", newline="\n")
    overrides = set(RESTORE) | set(REMOVE) | {"kernel/Kbuild", "kernel/runtime/ksud_integration.c"}
    changed = set(git(source, "diff", "--name-only", "HEAD").decode().splitlines())
    if not changed <= overrides:
        raise ValueError("Unrelated official sources were modified: " + str(changed - overrides))
    retained = [
        "userspace/ksud/bin/aarch64/waitsys",
        "manager/app/src/main/java/org/bakasu/bakasu/ui/webui/file/FileInterface.kt",
        "manager/app/src/main/java/org/bakasu/bakasu/ui/webui/pm/KsuPm.kt",
        "kernel/manager/throne_tracker.c", "kernel/supercall/dispatch.c",
        "kernel/include/arch.h", "kernel/compat/kernel_compat.h",
        "kernel/hook/x86_64/syscall_hook.c",
    ]
    retained_hashes = {}
    for rel in retained:
        actual = (source / rel).read_bytes()
        expected = git(source, "show", "HEAD:" + rel)
        if rel.endswith((".c", ".h", ".kt")):
            actual = actual.replace(b"\r\n", b"\n")
            expected = expected.replace(b"\r\n", b"\n")
        if actual != expected:
            raise ValueError("Official update was lost: " + rel)
        retained_hashes[rel] = hashlib.sha256(expected).hexdigest()
    return {
        "official_main_commit": MAIN, "retained_selinux_base": SELINUX_BASE,
        "restored_selinux_files": list(RESTORE), "removed_auto_susfs_files": list(REMOVE),
        "retained_update_hashes": retained_hashes,
    }


if __name__ == "__main__":
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--source", type=Path, required=True)
    ap.add_argument("--report", type=Path, required=True)
    args = ap.parse_args()
    result = apply(args.source)
    args.report.mkdir(parents=True, exist_ok=True)
    (args.report / "upstream-main-overlay.json").write_text(json.dumps(result, indent=2) + "\n")
    print("Official non-SELinux updates retained:", MAIN)
