"""Check source installation for the Ace 6 and OPD2413 backends."""

import argparse
from pathlib import Path
import tempfile

import install_builtin as affinity


def fixture(root, sublevel):
    sched = root / "kernel/sched"
    cgroup = root / "kernel/cgroup"
    sched.mkdir(parents=True)
    cgroup.mkdir(parents=True)
    (root / "Makefile").write_text(f"VERSION = 6\nPATCHLEVEL = 6\nSUBLEVEL = {sublevel}\n")
    (sched / "core.c").write_text(affinity.CORE_SEAM + "{}\n")
    (sched / "Makefile").write_text("obj-y += core.o\n")
    cpuset = cgroup / "cpuset.c"
    cpuset.write_text(affinity.CPUSET_SEAM + affinity.CPUSET_ATTACH)
    return sched, cpuset


def snapshot(kernel):
    return {path.relative_to(kernel): path.read_bytes() for path in kernel.rglob("*") if path.is_file()}


def test_install(root, device, sublevel):
    sched, cpuset = fixture(root, sublevel)
    original_cpuset = cpuset.read_bytes()
    affinity.install(root, device)
    first = snapshot(root)
    assert (sched / "core.c").read_text().count(affinity.CORE_INCLUDE) == 1
    assert (sched / "Makefile").read_text().splitlines().count(affinity.KBUILD) == 1
    for name in affinity.SOURCES:
        assert (sched / name).read_bytes() == (affinity.HERE / name).read_bytes(), name
    assert (sched / "frame_policy.h").read_bytes() == (affinity.HERE.parent / "frame_policy/frame_policy.h").read_bytes()
    has_flag = affinity.OPD_CPUSET_CFLAGS in (sched / "Makefile").read_text()
    assert has_flag == (device == "opd2413")
    if device == "opd2413":
        assert (sched / "ace6_cpuset_affinity.h").read_bytes() == (affinity.HERE / "ace6_cpuset_affinity.h").read_bytes()
        assert cpuset.read_text().count(affinity.CPUSET_NOTIFY) == 1
        assert cpuset.read_text().count(affinity.CPUSET_INCLUDE) == 1
    else:
        assert cpuset.read_bytes() == original_cpuset
        assert not (sched / "ace6_cpuset_affinity.h").exists()
    affinity.install(root, device)
    assert snapshot(root) == first


def test_unreviewed_version(root):
    fixture(root, 89)
    before = snapshot(root)
    try:
        affinity.install(root)
    except SystemExit as error:
        assert "6.6.118" in str(error)
    else:
        raise AssertionError("Ace 6 overlay accepted the OPD2413 kernel")
    assert snapshot(root) == before


def test_missing_cpuset_seam(root):
    _, cpuset = fixture(root, 89)
    cpuset.write_text(affinity.CPUSET_SEAM)
    before = snapshot(root)
    try:
        affinity.install(root, "opd2413")
    except SystemExit as error:
        assert "cpuset migration seam" in str(error)
    else:
        raise AssertionError("OPD2413 overlay accepted a missing cpuset seam")
    assert snapshot(root) == before


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--scratch", type=Path)
    args = parser.parse_args()
    with tempfile.TemporaryDirectory(dir=args.scratch) as directory:
        root = Path(directory)
        test_install(root / "ace6", "ace6", 118)
        test_install(root / "opd2413", "opd2413", 89)
        test_unreviewed_version(root / "wrong-device")
        test_missing_cpuset_seam(root / "missing-seam")
    print("Ace 6 and OPD2413 source installation passed")
