"""Build the isolated codec lab; never touch the Android kernel or device."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess

HERE = Path(__file__).resolve().parent
EXPORTS = [
    "crystal_lz4kd_encode_state_bytes_min", "crystal_lz4kd_encode", "crystal_lz4kd_decode",
    "crystal_lz4kd_encode_delta", "crystal_lz4kd_decode_delta",
    "crystal_sddc_codec_create", "crystal_sddc_codec_destroy", "crystal_sddc_codec_compress",
    "crystal_sddc_codec_decompress", "crystal_sddc_codec_compress_delta",
    "crystal_sddc_codec_decompress_delta", "crystal_sddc_codec_decompress_delta_borrowed",
    "restored_lz4kds_create", "restored_lz4kds_destroy", "restored_lz4kds_compress",
    "restored_lz4kds_decompress", "restored_lz4kds_compress_delta",
    "restored_lz4kds_decompress_delta", "restored_lz4kds_decompress_delta_borrowed",
    "lab_set_fail_after", "lab_check_allocation_guards", "lab_live_allocations",
]


def build(args):
    manifest = json.loads((HERE / "source-manifest.json").read_text(encoding="utf-8"))
    for name, expected in manifest["files"].items():
        if hashlib.sha256((HERE / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError("Unreviewed vendor source: " + name)
    args.output.mkdir(parents=True, exist_ok=False)
    if args.target == "android-arm64":
        if not args.ndk:
            raise RuntimeError("Android cross compilation requires --ndk")
        host = "windows-x86_64" if os.name == "nt" else "linux-x86_64"
        compiler = args.compiler or args.ndk / "toolchains/llvm/prebuilt" / host / "bin" / (
            "aarch64-linux-android26-clang.cmd" if os.name == "nt" else "aarch64-linux-android26-clang")
    else:
        compiler = args.compiler or Path(shutil.which("clang") or r"C:\Program Files\LLVM\bin\clang.exe")
    flags = ["-shared", "-O2", "-g", "-std=gnu11", "-Wall", "-Wextra", "-Werror",
             "-Wno-unused-parameter", "-Wno-sign-compare", "-ffreestanding", "-fno-builtin",
             "-fno-stack-protector", "-I", str(HERE / "shim")]
    sources = ["shim/lab_runtime.c", "restored_codec.c", "vendor/crystal_sddc_codec.c",
               "vendor/lz4kd/lz4kd_encode.c", "vendor/lz4kd/lz4kd_encode_delta.c",
               "vendor/lz4kd/lz4kd_decode.c"]
    windows_host = os.name == "nt" and args.target == "host"
    output = args.output / ("codec.dll" if windows_host else "libcodec.so")
    command = [str(compiler), *flags, *[str(HERE / name) for name in sources]]
    if windows_host:
        sdk = args.windows_sdk or Path(r"C:\Program Files (x86)\Windows Kits\10\Lib")
        libraries = sorted(sdk.glob("*/um/x64/kernel32.lib"))
        if not libraries:
            raise RuntimeError("Windows SDK kernel32.lib is required")
        definition = args.output / "codec.def"
        definition.write_text("EXPORTS\n" + "\n".join(EXPORTS) + "\n", encoding="ascii")
        command += ["-fuse-ld=lld", "-nostdlib", str(libraries[-1]), "-Wl,/NOENTRY",
                    "-Wl,/NODEFAULTLIB", "-Wl,/DEF:" + str(definition)]
    else:
        command += ["-fPIC", "-Wl,--no-undefined"]
    command += ["-o", str(output)]
    process = subprocess.run(command, capture_output=True)
    (args.output / "build.stdout.txt").write_bytes(process.stdout)
    (args.output / "build.stderr.txt").write_bytes(process.stderr)
    if process.returncode:
        raise RuntimeError(process.stderr.decode("utf-8", "replace"))
    version = subprocess.run([str(compiler), "--version"], capture_output=True, check=True)
    report = {"passed": True, "target": args.target,
              "compiler": version.stdout.decode("utf-8", "replace").splitlines()[0],
              "command": command, "library": str(output),
              "library_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
              "vendor_source": manifest, "android_device_used": False,
              "compiled_sources_sha256": {name: hashlib.sha256((HERE / name).read_bytes()).hexdigest()
                                          for name in sources}}
    (args.output / "build.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps({"passed": True, "library": str(output), "compiler": report["compiler"]}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--output", type=Path, required=True, help="Fresh directory outside source")
    parser.add_argument("--compiler", type=Path)
    parser.add_argument("--target", choices=["host", "android-arm64"], default="host")
    parser.add_argument("--ndk", type=Path)
    parser.add_argument("--windows-sdk", type=Path)
    build(parser.parse_args())
