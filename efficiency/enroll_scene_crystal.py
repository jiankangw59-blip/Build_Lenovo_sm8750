"""Enroll a verified Crystal Image in Scene's existing early-boot allowlists."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from prepare_boot import compiled_notes

STATE = "/data/adb/ace6-kernel-efficiency"


def ci_notes(evidence, image_sha):
    """Consume our Actions linked-ELF proof and recheck the downloaded Image."""
    proof = json.loads((evidence / "candidate-verification.json").read_text(encoding="utf-8"))
    source = (evidence / "workflow-commit.txt").read_text().strip()
    if (not re.fullmatch("[0-9a-f]{40}", source) or proof["source"] != source or
            not proof["passed"] or proof["kmi_imports"] != 32921 or proof["kernel_cases"] < 14 or
            proof["image_sha256"] != image_sha or proof["config_added"] != ["CONFIG_FRAME_POLICY=y"] or
            proof["config_removed"]):
        raise ValueError("CI verification is not the expected additive frame-policy build")
    metadata = proof["kernel_notes"]
    offset, size = metadata["image_offset"], metadata["size"]
    image_size = (evidence / "Image").stat().st_size
    if (not metadata["independent_elf_comparison"] or not 0 < size <= 65536 or
            not 0 <= offset <= image_size - size or image_size != proof["image_bytes"]):
        raise ValueError("Invalid CI notes bounds or independent ELF proof")
    with (evidence / "Image").open("rb") as stream:
        stream.seek(offset)
        notes = stream.read(size)
    if (notes != (evidence / "compiled-kernel-notes.bin").read_bytes() or
            hashlib.sha256(notes).hexdigest() != metadata["sha256"]):
        raise ValueError("Downloaded Image identity differs from its CI linked-ELF proof")
    return notes, {**metadata, "elf_comparison_location": "GitHub Actions", "source": source}


def enroll(evidence, serial, backup, inherit=None, use_ci_notes=False):
    def root(script):
        process = subprocess.run(
            ["adb", "-s", serial, "shell", "su -c sh"], input=script.encode(),
            capture_output=True, timeout=30)
        if process.returncode:
            raise RuntimeError(process.stderr.decode("utf-8", "replace"))
        return process.stdout.decode("utf-8").replace("\r", "").strip()

    def load(name):
        return json.loads((evidence / name).read_text(encoding="utf-8"))

    kmi = load("efficiency/kmi-verification.json")
    profile = load("efficiency/efficiency-patches.json")
    assert kmi["passed"] and not kmi["failures"] and kmi["checked_imports"] == 32921
    assert load("efficiency/config-verification.json")["passed"]
    assert profile["version"] == "6.6.118" and profile["profile"] == "crystal-lz4kds-le9uo-idle"
    config = (evidence / "kernel.config").read_text(encoding="utf-8").splitlines()
    assert 'CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP="lz4kds"' in config
    with (evidence / "Image").open("rb") as stream:
        image_sha = hashlib.file_digest(stream, "sha256").hexdigest()
    assert image_sha == load("ak3-provenance.json")["kernel_sha256"]
    notes, metadata = ci_notes(evidence, image_sha) if use_ci_notes else compiled_notes(evidence)
    digest = metadata["sha256"]
    assert root("getprop ro.product.model\n") == "PLQ110"
    assert root("test -f /data/adb/modules/scene_swap_controller/scripts/early_crystal_writeback.sh\n") == ""
    current = root("sha256sum /sys/kernel/notes\n").split()[0]
    source = inherit or current
    assert re.fullmatch("[0-9a-f]{64}", source)
    known = root(f"cat {STATE}/known-crystal-kernels\n")
    selected = root(f"cat {STATE}/crystal-kernel-writeback.enabled\n")
    mappings = [line.split() for line in known.splitlines()]
    assert [value for sha, value in mappings if sha == source] == ["lz4kds"]
    existing = [value for sha, value in mappings if sha == digest]
    assert existing in ([], ["lz4kds"])
    policy_enabled = source in selected.splitlines()
    backup.mkdir(parents=True, exist_ok=False)
    (backup / "known-crystal-kernels.before").write_text(known + "\n", encoding="utf-8")
    (backup / "crystal-kernel-writeback.enabled.before").write_text(selected + "\n", encoding="utf-8")
    (backup / "compiled-notes.bin").write_bytes(notes)
    # Append exact verified identities; keep unrelated registrations and settings.
    script = f'''set -eu
state={STATE}
test -d "$state" && test ! -L "$state"
for name in known-crystal-kernels crystal-kernel-writeback.enabled; do
    test -f "$state/$name" && test ! -L "$state/$name"
done
exec 7>"$state/enrollment.lock"
flock -x 7 7>&7
known="$state/known-crystal-kernels"
selected="$state/crystal-kernel-writeback.enabled"
grep -Fqx '{source} lz4kds' "$known"
if ! grep -Fqx '{digest} lz4kds' "$known"; then
    cp -p "$known" "$known.before-{digest[:12]}"
    cat "$known" > "$known.tmp.$$"
    printf '%s lz4kds\\n' '{digest}' >> "$known.tmp.$$"
    chmod 600 "$known.tmp.$$"
    mv "$known.tmp.$$" "$known"
fi
'''
    if policy_enabled:
        script += f'''grep -Fqx '{source}' "$selected"
if ! grep -Fqx '{digest}' "$selected"; then
    cp -p "$selected" "$selected.before-{digest[:12]}"
    cat "$selected" > "$selected.tmp.$$"
    printf '%s\\n' '{digest}' >> "$selected.tmp.$$"
    chmod 600 "$selected.tmp.$$"
    mv "$selected.tmp.$$" "$selected"
fi
grep -Fqx '{digest}' "$selected"
'''
    script += f"grep -Fqx '{digest} lz4kds' \"$known\"\n"
    root(script)
    report = {"notes_sha256": digest, "policy_inherited_from": source,
              "kernel_writeback_enabled": policy_enabled, "codec": "lz4kds",
              "image_sha256": image_sha, "kmi_checks": 32921,
              "notes_proof": metadata,
              "running_candidate": current == digest,
              "zram_configuration_unchanged": True, "swap_reset_executed": False}
    (backup / "enrollment.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--evidence", type=Path, required=True)
    parser.add_argument("--serial", required=True)
    parser.add_argument("--backup-dir", type=Path, required=True)
    parser.add_argument("--inherit-policy-from", help="Previously verified kernel notes SHA256")
    parser.add_argument("--ci-notes-proof", action="store_true",
                        help="Use our Actions linked-ELF proof and recheck local Image notes")
    args = parser.parse_args()
    enroll(args.evidence, args.serial, args.backup_dir, args.inherit_policy_from, args.ci_notes_proof)
