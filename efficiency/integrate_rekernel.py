"""Link the pinned official Re:Kernel sources into the kernel image."""
import argparse
import hashlib
import json
import pathlib
import shutil
import subprocess


def integrate(kernel, report):
    package = pathlib.Path(__file__).resolve().parent / "rekernel"
    lock = json.loads((package / "source-lock.json").read_text(encoding="utf-8"))
    upstream = package / "LKM-Source"
    target = kernel / "drivers/rekernel"
    if target.exists():
        raise RuntimeError("ReKernel source directory already exists; refuse mixed integration")
    for name, digest in lock["files"].items():
        if hashlib.sha256((upstream / name).read_bytes()).hexdigest() != digest:
            raise RuntimeError("Official ReKernel source checksum mismatch: " + name)
    shutil.copytree(upstream, target)
    adaptation = lock["builtin_adaptation"]
    patch = package / adaptation["file"]
    if hashlib.sha256(patch.read_bytes()).hexdigest() != adaptation["sha256"]:
        raise RuntimeError("ReKernel built-in adaptation checksum mismatch")
    command = ["patch", "--batch", "--forward", "--fuzz=0", "-p1", "-i", str(patch)]
    subprocess.run([*command, "--dry-run"], cwd=target, check=True)
    subprocess.run(command, cwd=target, check=True)
    edits = {
        "drivers/Kconfig": '\nsource "drivers/rekernel/Kconfig"\n',
        "drivers/Makefile": '\nobj-$(CONFIG_REKERNEL) += rekernel/\n',
    }
    for filename, addition in edits.items():
        path = kernel / filename
        original = path.read_text(encoding="utf-8")
        if "drivers/rekernel/Kconfig" in original or " += rekernel/" in original:
            raise RuntimeError("Duplicate ReKernel build registration: " + filename)
        path.write_text(original.rstrip() + "\n" + addition, encoding="utf-8")
    for name, digest in lock["files"].items():
        expected = adaptation["patched_files"].get(name, digest)
        if hashlib.sha256((target / name).read_bytes()).hexdigest() != expected:
            raise RuntimeError("Unexpected ReKernel source change during integration: " + name)
    result = {"source": lock, "linkage": "built-in when CONFIG_REKERNEL=y",
              "official_behavior_preserved": True,
              "adaptation": "Pinned codex/rekernel-builtin patch: Kbuild, Kconfig, binder include, late_initcall",
              "transport": "official Generic Netlink",
              "runtime_verified": False}
    report.parent.mkdir(parents=True, exist_ok=True)
    report.write_text(json.dumps(result, indent=2), encoding="utf-8")
    print(json.dumps({k: v for k, v in result.items() if k != "source"}, indent=2))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--kernel", type=pathlib.Path, required=True)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    args = parser.parse_args()
    integrate(args.kernel.resolve(), args.report.resolve())
