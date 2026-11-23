"""Build the ARM64 block helper and a real host pattern DLL; never use ADB."""
import argparse
import ctypes
import hashlib
import json
import os
from pathlib import Path
import subprocess

HERE = Path(__file__).resolve().parent


def build(args):
    args.output.mkdir(parents=True, exist_ok=False)
    host = "windows-x86_64" if os.name == "nt" else "linux-x86_64"
    compiler = args.ndk / "toolchains/llvm/prebuilt" / host / "bin" / (
        "aarch64-linux-android26-clang.cmd" if os.name == "nt" else "aarch64-linux-android26-clang")
    binary = args.output / "crystal-block-probe"
    flags = ["-O2", "-std=c11", "-Wall", "-Wextra", "-Werror"]
    command = [str(compiler), *flags, str(HERE / "block_probe.c"), "-o", str(binary)]
    result = subprocess.run(command, capture_output=True)
    (args.output / "android-build.stdout.txt").write_bytes(result.stdout)
    (args.output / "android-build.stderr.txt").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    blob = binary.read_bytes()
    if blob[:6] != b"\x7fELF\x02\x01" or int.from_bytes(blob[18:20], "little") != 183:
        raise RuntimeError("Compiler did not produce an AArch64 ELF64 helper")
    owned_binary = args.output / "crystal-owned-pages-probe"
    owned_command = [str(compiler), *flags, str(HERE / "owned_pages_probe.c"), "-o", str(owned_binary)]
    result = subprocess.run(owned_command, capture_output=True)
    (args.output / "owned-build.stdout.txt").write_bytes(result.stdout)
    (args.output / "owned-build.stderr.txt").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    owned_blob = owned_binary.read_bytes()
    if owned_blob[:6] != b"\x7fELF\x02\x01" or int.from_bytes(owned_blob[18:20], "little") != 183:
        raise RuntimeError("Owned-pages compiler output is not AArch64 ELF64")
    lifecycle_binary = args.output / "crystal-lifecycle-probe"
    lifecycle_command = [str(compiler), *flags, "-pthread", str(HERE / "lifecycle_probe.c"), "-o", str(lifecycle_binary)]
    result = subprocess.run(lifecycle_command, capture_output=True)
    (args.output / "lifecycle-build.stdout.txt").write_bytes(result.stdout)
    (args.output / "lifecycle-build.stderr.txt").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    lifecycle_blob = lifecycle_binary.read_bytes()
    if lifecycle_blob[:6] != b"\x7fELF\x02\x01" or int.from_bytes(lifecycle_blob[18:20], "little") != 183:
        raise RuntimeError("Lifecycle compiler output is not AArch64 ELF64")
    backing_binary = args.output / "crystal-backing-probe"
    backing_command = [str(compiler), *flags, str(HERE / "backing_probe.c"), "-o", str(backing_binary)]
    result = subprocess.run(backing_command, capture_output=True)
    (args.output / "backing-build.stdout.txt").write_bytes(result.stdout)
    (args.output / "backing-build.stderr.txt").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    backing_blob = backing_binary.read_bytes()
    if backing_blob[:6] != b"\x7fELF\x02\x01" or int.from_bytes(backing_blob[18:20], "little") != 183:
        raise RuntimeError("Owned-backing compiler output is not AArch64 ELF64")
    report = {"android_compiled": True, "android_device_used": False, "runtime_test_performed": False,
              "android_command": command, "helper": str(binary),
              "helper_sha256": hashlib.sha256(blob).hexdigest(),
              "owned_helper": str(owned_binary), "owned_command": owned_command,
              "owned_helper_sha256": hashlib.sha256(owned_blob).hexdigest(),
              "lifecycle_helper": str(lifecycle_binary), "lifecycle_command": lifecycle_command,
              "lifecycle_helper_sha256": hashlib.sha256(lifecycle_blob).hexdigest(),
              "backing_helper": str(backing_binary), "backing_command": backing_command,
              "backing_helper_sha256": hashlib.sha256(backing_blob).hexdigest(),
              "source_sha256": {p.name: hashlib.sha256(p.read_bytes()).hexdigest()
                                 for p in HERE.iterdir() if p.is_file()}}
    if os.name == "nt":
        host_compiler = args.host_compiler or Path(r"C:\Program Files\LLVM\bin\clang.exe")
        library = args.output / "pattern.dll"
        host_command = [str(host_compiler), "-shared", "-O2", "-std=c11", "-Wall", "-Wextra", "-Werror",
                        "-ffreestanding", "-fno-builtin", "-fno-stack-protector", "-fuse-ld=lld",
                        "-nostdlib", str(HERE / "pattern_host.c"), "-Wl,/NOENTRY", "-Wl,/NODEFAULTLIB",
                        "-o", str(library)]
    else:
        host_compiler = args.host_compiler or Path("clang")
        library = args.output / "pattern.so"
        host_command = [str(host_compiler), "-shared", "-fPIC", "-O2", "-std=c11", "-Wall", "-Wextra",
                        "-Werror", str(HERE / "pattern_host.c"), "-o", str(library)]
    result = subprocess.run(host_command, capture_output=True)
    (args.output / "host-build.stdout.txt").write_bytes(result.stdout)
    (args.output / "host-build.stderr.txt").write_bytes(result.stderr)
    if result.returncode:
        raise RuntimeError(result.stderr.decode("utf-8", "replace"))
    report.update(host_command=host_command, host_pattern_library=str(library),
                  host_library_sha256=hashlib.sha256(library.read_bytes()).hexdigest())
    (args.output / "build.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--host-compiler", type=Path)
    parser.add_argument("--output", type=Path, required=True)
    build(parser.parse_args())
