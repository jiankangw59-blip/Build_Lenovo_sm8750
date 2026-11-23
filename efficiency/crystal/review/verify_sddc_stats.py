"""Verify the original -> integrity -> read-only stats source chain on the host."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
FIELDS = ("integrity_checks", "integrity_failures", "integrity_skipped")


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def verify(args):
    meta = json.loads((HERE / "sddc-integrity-stats.json").read_text(encoding="utf-8"))
    args.output.mkdir(parents=True, exist_ok=False)
    fixture = args.output / "fixture"
    for relative, expected in meta["files"].items():
        original = args.upstream / relative
        assert sha(original) == expected["original_sha256"], relative
        target = fixture / relative
        target.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(original, target)
    chain = []
    for name, sha_key, state_key in (
        ("sddc-integrity.patch", "integrity_patch_sha256", "after_integrity_sha256"),
        ("sddc-integrity-stats.patch", "stats_patch_sha256", "after_stats_sha256"),
    ):
        patch = HERE.parent / name
        assert sha(patch) == meta[sha_key], name
        command = [str(args.patch), "-p1", "--fuzz=0", "--batch", "--forward", "-i", str(patch)]
        result = subprocess.run(command, cwd=fixture, capture_output=True)
        (args.output / (name + ".stdout.txt")).write_bytes(result.stdout)
        (args.output / (name + ".stderr.txt")).write_bytes(result.stderr)
        assert result.returncode == 0, result.stderr.decode("utf-8", "replace")
        for relative, expected in meta["files"].items():
            assert sha(fixture / relative) == expected[state_key], (name, relative)
        chain.append({"patch": name, "sha256": sha(patch), "fuzz": 0})
    base = fixture / "drivers/crystalfrostwork/crystal_hybridswap"
    header = (base / "sddc/crystal_sddc.h").read_text(encoding="utf-8")
    snapshot = re.search(r"struct crystal_sddc_stats_snapshot \{(.*?)\n};", header, re.S).group(1)
    assert snapshot.endswith("\n" + "\n".join("\tu64 " + name + ";" for name in FIELDS))
    source = (base / "sddc/crystal_sddc.c").read_text(encoding="utf-8")
    body = source[source.index("void crystal_sddc_get_stats("):]
    body = body[:body.index("\n}") + 2]
    assert body.index("memset(stats, 0, sizeof(*stats))") < body.index("crystal_sddc_manager_get")
    for name in FIELDS:
        assert body.count("stats->" + name + " = atomic64_read(&sddc->stats." + name + ");") == 1
    # The CONFIG=n inline stub zeros sizeof(the expanded private snapshot).
    disabled = header[header.index("#else"):]
    assert "memset(stats, 0, sizeof(*stats));" in disabled
    zram = (base / "zram_drv.c").read_text(encoding="utf-8")
    show = zram[zram.index("static ssize_t sddc_stat_show("):]
    show = show[:show.index("\n}") + 2]
    names = re.findall(r'"([a-z_]+): %[a-z]+\\n"', show)
    arguments = re.findall(r"stats\.([a-z_]+)", show)
    assert len(names) == len(set(names))
    assert names == arguments, "sysfs format/argument ordering mismatch"
    assert names[-3:] == list(FIELDS)
    assert len("".join(name + ": " + str(2**64-1) + "\n" for name in names)) < 4096
    # Mode model checks source-declared names, not the kernel runtime.
    modes = []
    for enabled, checks, failures, skipped in ((0, 0, 0, 0), (1, 7, 2, 3)):
        values = {name: 0 for name in names}
        values.update(enabled=enabled, integrity_checks=checks,
                      integrity_failures=failures, integrity_skipped=skipped)
        text = "".join(f"{name}: {values[name]}\n" for name in names)
        parsed = dict(line.split(": ") for line in text.splitlines())
        assert tuple(int(parsed[name]) for name in FIELDS) == (checks, failures, skipped)
        modes.append({"enabled": enabled, "modeled_export": {name: parsed[name] for name in FIELDS}})
    report = {"passed": True, "source_chain": chain, "files": meta["files"],
              "sysfs_format_arguments_match": True, "sysfs_fields": len(names),
              "private_snapshot_fields_appended": list(FIELDS), "mode_models": modes,
              "kernel_compilation_verified": False, "kernel_runtime_verified": False,
              "kernel_kunit_executed": False, "device_used": False}
    (args.output / "stats-checks.json").write_text(json.dumps(report, indent=2)+"\n", encoding="utf-8")
    print(json.dumps({"passed": True, "source_chain": chain, "sysfs_fields": len(names)}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--upstream", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--patch", type=Path, default=Path(shutil.which("patch") or "patch"))
    verify(parser.parse_args())
