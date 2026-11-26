"""Verify an already-built candidate and export small, explicit acceptance evidence."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import shutil
import sys

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from prepare_boot import compiled_notes


def read(path):
    return json.loads(path.read_text(encoding="utf-8"))


def verify(candidate, baseline, kunit, output, source):
    if not re.fullmatch(r"[0-9a-f]{40}", source):
        raise ValueError("Expected the full source SHA")
    if (candidate / "workflow-commit.txt").read_text().strip() != source:
        raise ValueError("Candidate source differs from the requested build")
    kmi = read(candidate / "efficiency/kmi-verification.json")
    if not kmi["passed"] or kmi["failures"] or kmi["checked_imports"] != 32921:
        raise ValueError("Candidate did not pass all 32921 KMI imports")
    if not read(candidate / "efficiency/config-verification.json")["passed"]:
        raise ValueError("Candidate optimization configuration did not pass")
    profile = read(candidate / "efficiency/efficiency-patches.json")
    if profile["version"] != "6.6.118" or profile["profile"] != "crystal-lz4kds-le9uo-idle":
        raise ValueError("Candidate optimization profile differs from the baseline")
    old = set((baseline / "kernel.config").read_text().splitlines())
    new = set((candidate / "kernel.config").read_text().splitlines())
    added, removed = sorted(new - old), sorted(old - new)
    if added != ["CONFIG_FRAME_POLICY=y"] or removed:
        raise ValueError("Unexpected configuration changes: " + json.dumps({"added": added, "removed": removed}))
    proof = read(candidate / "ak3-provenance.json")
    image = candidate / "Image"
    with image.open("rb") as stream:
        image_hash = hashlib.file_digest(stream, "sha256").hexdigest()
    if image_hash != proof["kernel_sha256"] or image.stat().st_size != proof["kernel_bytes"]:
        raise ValueError("Exported Image differs from the packaged kernel provenance")
    results = read(kunit / "fixed.json")
    cases = [case for group in results["sub_groups"] for case in group["test_cases"]]
    if len(cases) != 14 or any(case["status"] != "PASS" for case in cases):
        raise ValueError("Expected exactly fourteen passing real kernel tests")
    comparison = read(kunit / "comparison.json")
    if not comparison["fixed_all_cases_passed"] or not comparison["legacy_detected"]:
        raise ValueError("The locking regression comparison did not pass")
    notes, notes_proof = compiled_notes(candidate)
    output.mkdir(parents=True, exist_ok=True)
    for name in ("kernel.config", "kernel.release", "workflow-commit.txt", "ak3-provenance.json",
                 "efficiency/kmi-verification.json", "efficiency/config-verification.json",
                 "efficiency/efficiency-patches.json"):
        destination = output / name
        destination.parent.mkdir(parents=True, exist_ok=True)
        shutil.copyfile(candidate / name, destination)
    shutil.copyfile(kunit / "fixed.json", output / "kunit-fixed.json")
    shutil.copyfile(kunit / "comparison.json", output / "kunit-comparison.json")
    (output / "compiled-kernel-notes.bin").write_bytes(notes)
    report = {"source": source, "passed": True, "kmi_imports": 32921, "kernel_cases": 14,
              "image_sha256": image_hash, "image_bytes": image.stat().st_size,
              "kernel_notes": notes_proof, "config_added": added, "config_removed": removed,
              "baseline_source": (baseline / "workflow-commit.txt").read_text().strip(),
              "candidate_flashed": False, "device_testing": False}
    (output / "candidate-verification.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for name in ("candidate", "baseline", "kunit", "output"):
        parser.add_argument("--" + name, required=True, type=Path)
    parser.add_argument("--source", required=True)
    args = parser.parse_args()
    verify(args.candidate, args.baseline, args.kunit, args.output, args.source)
