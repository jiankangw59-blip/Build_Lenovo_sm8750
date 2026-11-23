"""Create and read back a PLQ110 candidate boot file, without flashing it."""
import argparse
import gzip
import hashlib
import json
import pathlib
import shlex
import struct
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def compiled_notes(evidence):
    wanted = {"_text", "__start_notes", "__stop_notes"}
    symbols = {}
    with (evidence / "efficiency/System.map").open(encoding="utf-8") as stream:
        for line in stream:
            fields = line.split()
            if len(fields) == 3 and fields[2] in wanted:
                symbols[fields[2]] = int(fields[0], 16)
    if symbols.keys() != wanted:
        raise RuntimeError("Compiled kernel lacks bounded notes symbols")
    start, end = symbols["__start_notes"], symbols["__stop_notes"]
    offset = start - symbols["_text"]
    if not 0 <= offset or not 0 < end - start <= 65536:
        raise RuntimeError("Invalid compiled notes bounds")
    with (evidence / "Image").open("rb") as stream:
        stream.seek(offset)
        notes = stream.read(end - start)
    if len(notes) != end - start:
        raise RuntimeError("Compiled notes escape the exported Image")
    with gzip.open(evidence / "efficiency/vmlinux.gz", "rb") as elf:
        header = elf.read(64)
        if header[:6] != b"\x7fELF\x02\x01":
            raise RuntimeError("Expected a little-endian ELF64 vmlinux")
        phoff = struct.unpack_from("<Q", header, 32)[0]
        entsize, count = struct.unpack_from("<HH", header, 54)
        elf.seek(phoff)
        program_headers = elf.read(entsize * count)
        mapped = None
        for i in range(count):
            kind, flags, file_offset, address, physical, file_size, memory_size, align = struct.unpack_from(
                "<IIQQQQQQ", program_headers, i * entsize)
            if kind == 1 and address <= start and end <= address + file_size:
                if mapped is not None:
                    raise RuntimeError("Ambiguous notes mapping")
                mapped = file_offset + start - address
        if mapped is None:
            raise RuntimeError("Notes are outside the linked ELF load segments")
        elf.seek(mapped)
        if elf.read(end - start) != notes:
            raise RuntimeError("Exported Image notes differ from the linked vmlinux")
    return notes, {"sha256": hashlib.sha256(notes).hexdigest(), "size": len(notes),
                   "image_offset": offset, "vmlinux_file_offset": mapped,
                   "independent_elf_comparison": True}


def prepare(args):
    evidence = args.evidence.resolve()
    image = evidence / "Image"
    provenance = json.loads((evidence / "ak3-provenance.json").read_text(encoding="utf-8"))
    for name in ["config-verification.json", "kmi-verification.json"]:
        if json.loads((evidence / "efficiency" / name).read_text(encoding="utf-8"))["passed"] is not True:
            raise RuntimeError("Candidate failed " + name)
    patches = json.loads((evidence / "efficiency/efficiency-patches.json").read_text(encoding="utf-8"))
    if patches["version"] != "6.6.89" or patches["source"] != "c1802dd6f3343e71c166a3c67c50554c81880b6e":
        raise RuntimeError("Only the compatible Ace 6 6.6.89 baseline is accepted")
    profile = patches.get("profile", "baseline")
    if profile != args.expected_profile:
        raise RuntimeError("Requested profile differs from the compiled artifact")
    files = [entry["file"] for entry in patches["applied"]]
    stage2 = {name for name in files if name in (
        "patches/kcompressd-6.6.89.patch", "patches/le9uo-6.6.89.patch")}
    stage2_profile = "kcompressd" if profile == "crystal" else profile
    expected_stage2 = {f"patches/{stage2_profile}-6.6.89.patch"} if profile != "baseline" else set()
    if stage2 != expected_stage2 or len(files) != 19 + bool(expected_stage2):
        raise RuntimeError("Candidate does not contain exactly the requested isolated patch series")
    if profile == "crystal":
        crystal = json.loads((evidence / "efficiency/crystal-integration.json").read_text(encoding="utf-8"))
        if crystal.get("passed") is not True or crystal.get("source_commit") != "13d291aef3d1d9607ad3fa5ff30dc23b2888a3da":
            raise RuntimeError("Candidate lacks the pinned Crystal integration report")
        config = (evidence / "kernel.config").read_text(encoding="utf-8")
        for line in ["CONFIG_CRYSTAL_HYBRIDSWAP=y", "# CONFIG_ZRAM is not set",
                     'CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_DEF_COMP="lz4kd"',
                     "CONFIG_CRYSTAL_HYBRIDSWAP_ZRAM_LOCK_DEF_COMP=y"]:
            if line not in config.splitlines():
                raise RuntimeError("Candidate Crystal configuration differs: " + line)
    release = (evidence / "kernel.release").read_text(encoding="utf-8").strip()
    if not release.startswith("6.6.89-"):
        raise RuntimeError("Unexpected actual kernel release: " + release)
    kernel_sha = digest(image)
    if kernel_sha != provenance["kernel_sha256"]:
        raise RuntimeError("Compiled Image checksum mismatch")
    if image.read_bytes()[56:60] != b"ARMd":
        raise RuntimeError("Expected an uncompressed ARM64 Image")
    notes, notes_metadata = compiled_notes(evidence)
    tool = args.ak3.resolve() / "tools/magiskboot"
    if digest(tool) != provenance["ak3_files"]["tools/magiskboot"]:
        raise RuntimeError("AK3 magiskboot provenance mismatch")

    original = args.original.resolve()
    with original.open("rb") as stream:
        header = stream.read(4096)
    if header[:8] != b"ANDROID!" or struct.unpack_from("<I", header, 40)[0] != 4:
        raise RuntimeError("Expected the original v4 boot format")
    if struct.unpack_from("<I", header, 12)[0] != 0:
        raise RuntimeError("This device baseline expects a boot image with no ramdisk")

    def adb(*argv, timeout=120):
        p = subprocess.run(["adb", "-s", args.serial, *argv], capture_output=True, timeout=timeout)
        if p.returncode:
            raise RuntimeError((p.stdout + p.stderr).decode("utf-8", "replace")[-5000:])
        return (p.stdout + p.stderr).decode("utf-8", "replace").replace("\r", "").strip()

    def root(code):
        return adb("shell", "su -c " + shlex.quote(code))

    if root("getprop ro.product.model") != "PLQ110" or root("getprop ro.boot.slot_suffix") != "_b":
        raise RuntimeError("Device or active slot differs from the recorded baseline")
    original_sha = digest(original)
    if root("sha256sum /dev/block/by-name/boot_b").split()[0] != original_sha:
        raise RuntimeError("Current boot differs from the rollback image")
    if int(root("blockdev --getsize64 /dev/block/by-name/boot_b")) != original.stat().st_size:
        raise RuntimeError("Original boot size differs from the actual partition")

    args.output.mkdir(parents=True, exist_ok=False)
    stage = f"/data/local/tmp/ace6-candidate-{kernel_sha[:12]}-{time.time_ns()}"
    adb("shell", "mkdir -m 700 " + stage)
    for source, name in [(original, "original.img"), (image, "Image"), (tool, "magiskboot")]:
        adb("push", str(source), stage + "/" + name)
        if root("sha256sum " + stage + "/" + name).split()[0] != digest(source):
            raise RuntimeError("Staged file checksum mismatch: " + name)
    script = f"""set -e
cd {stage}
chmod 700 magiskboot
mkdir pack verify
cd pack
../magiskboot unpack -h ../original.img
cp ../Image kernel
../magiskboot repack ../original.img ../candidate.img
cd ../verify
../magiskboot unpack -h ../candidate.img
"""
    log = root(script)
    (args.output / "packing.log").write_text(log + "\n", encoding="utf-8")
    if root(f"sha256sum {stage}/verify/kernel").split()[0] != kernel_sha:
        raise RuntimeError("Candidate kernel readback differs from the compiled Image")
    if root(f"cat {stage}/pack/header") != root(f"cat {stage}/verify/header"):
        raise RuntimeError("Boot header settings changed during packing")
    actual_size = int(root(f"wc -c < {stage}/candidate.img"))
    if actual_size != original.stat().st_size:
        raise RuntimeError("Repacked image does not retain the original partition size")
    boot = args.output / "boot-6.6.89-efficiency.img"
    adb("pull", stage + "/candidate.img", str(boot))
    boot_sha = digest(boot)
    if root(f"sha256sum {stage}/candidate.img").split()[0] != boot_sha:
        raise RuntimeError("Downloaded candidate checksum mismatch")
    if root("sha256sum /dev/block/by-name/boot_b").split()[0] != original_sha:
        raise RuntimeError("Boot partition changed externally during preparation")
    result = {"serial": args.serial, "model": "PLQ110", "active_slot": "_b",
              "release": release, "source": patches["source"], "profile": profile,
              "workflow_commit": (evidence / "workflow-commit.txt").read_text(encoding="utf-8").strip(),
              "kernel_sha256": kernel_sha, "candidate_boot_sha256": boot_sha,
              "candidate_size": actual_size, "rollback_boot_sha256": original_sha,
              "staging_directory": stage, "flash_performed": False,
              "kernel_readback_verified": True, "current_partition_unchanged": True}
    result["kernel_notes"] = notes_metadata
    (args.output / "compiled-kernel-notes.bin").write_bytes(notes)
    (args.output / "candidate-boot.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--evidence", type=pathlib.Path, required=True)
    parser.add_argument("--ak3", type=pathlib.Path, required=True)
    parser.add_argument("--original", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    parser.add_argument("--expected-profile", choices=["baseline", "kcompressd", "le9uo", "crystal"], default="baseline")
    prepare(parser.parse_args())
