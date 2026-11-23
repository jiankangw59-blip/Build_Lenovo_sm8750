"""Build a reviewable Scene native-writeback payload from hash-locked originals."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import sys

from prepare_scene import prepare, EXPECTED


def build(args):
    if args.output.exists():
        raise RuntimeError("Choose a fresh output directory; existing payloads are preserved")
    host = "windows-x86_64" if sys.platform == "win32" else "darwin-x86_64" if sys.platform == "darwin" else "linux-x86_64"
    compiler = args.ndk / "toolchains/llvm/prebuilt" / host / "bin" / (
        "aarch64-linux-android26-clang.cmd" if sys.platform == "win32" else "aarch64-linux-android26-clang")
    if not compiler.is_file():
        raise RuntimeError("Missing the requested Android NDK compiler")
    scripts = args.output / "scripts"
    prepare(args.source, scripts)
    here = Path(__file__).resolve().parent
    subprocess.run([str(compiler), "-O2", "-Wall", "-Wextra", "-Werror",
                    str(here / "native_writeback_event.c"), "-o", str(scripts / "native_writeback_event")], check=True)
    # Windows' system32/bash.exe is a WSL launcher, not Git Bash.
    bash = str(args.bash) if args.bash else str(Path(os.environ.get("ProgramFiles", r"C:\Program Files")) / "Git/bin/bash.exe") if sys.platform == "win32" else shutil.which("bash")
    if not bash:
        raise RuntimeError("Bash is required to validate the generated shell scripts")
    for script in args.output.rglob("*.sh"):
        subprocess.run([bash, "-n", str(script)], check=True)
    (args.output / "native-writeback.conf").write_text(
        (here / "native-writeback.conf").read_text(encoding="utf-8"), encoding="utf-8", newline="\n")
    version = subprocess.run([str(compiler), "--version"], capture_output=True, text=True, check=True).stdout.splitlines()[0]
    files = {str(p.relative_to(args.output)).replace("\\", "/"): hashlib.sha256(p.read_bytes()).hexdigest()
             for p in args.output.rglob("*") if p.is_file()}
    report = {"compiler": version, "flags": ["-O2", "-Wall", "-Wextra", "-Werror"],
              "source_inputs": EXPECTED, "payload_sha256": files,
              "controller_source_sha256": hashlib.sha256((here / "native_writeback_event.c").read_bytes()).hexdigest(),
              "runtime_verification_performed_by_this_build": False}
    (args.output / "manifest.json").write_text(json.dumps(report, indent=2) + "\n", encoding="utf-8")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--ndk", type=Path, required=True)
    parser.add_argument("--source", type=Path, required=True, help="Audited Scene scripts directory, with ../service.sh")
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--bash", type=Path, help="Optional explicit Bash executable")
    build(parser.parse_args())
