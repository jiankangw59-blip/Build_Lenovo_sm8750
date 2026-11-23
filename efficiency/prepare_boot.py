"""Create and read back a PLQ110 candidate boot file, without flashing it."""
import argparse
import hashlib
import json
import pathlib
import shlex
import struct
import subprocess
import time


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


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
    release = (evidence / "kernel.release").read_text(encoding="utf-8").strip()
    if not release.startswith("6.6.89-"):
        raise RuntimeError("Unexpected actual kernel release: " + release)
    kernel_sha = digest(image)
    if kernel_sha != provenance["kernel_sha256"]:
        raise RuntimeError("Compiled Image checksum mismatch")
    if image.read_bytes()[56:60] != b"ARMd":
        raise RuntimeError("Expected an uncompressed ARM64 Image")
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
              "release": release, "source": patches["source"],
              "workflow_commit": (evidence / "workflow-commit.txt").read_text(encoding="utf-8").strip(),
              "kernel_sha256": kernel_sha, "candidate_boot_sha256": boot_sha,
              "candidate_size": actual_size, "rollback_boot_sha256": original_sha,
              "staging_directory": stage, "flash_performed": False,
              "kernel_readback_verified": True, "current_partition_unchanged": True}
    (args.output / "candidate-boot.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(result, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--serial", required=True)
    parser.add_argument("--evidence", type=pathlib.Path, required=True)
    parser.add_argument("--ak3", type=pathlib.Path, required=True)
    parser.add_argument("--original", type=pathlib.Path, required=True)
    parser.add_argument("--output", type=pathlib.Path, required=True)
    prepare(parser.parse_args())
