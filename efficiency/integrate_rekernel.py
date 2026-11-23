"""Link the pinned official Re:Kernel sources into the kernel image."""
import argparse
import hashlib
import json
import pathlib
import shutil


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
    # Keep the original Makefile for provenance, and adapt only Kbuild linkage.
    (target / "Makefile").rename(target / "Makefile.upstream")
    (target / "Makefile").write_text(
        "include $(srctree)/drivers/rekernel/Makefile.upstream\n"
        "obj-m :=\n"
        "obj-$(CONFIG_REKERNEL) += rekernel.o\n"
        "ccflags-y += -I$(srctree)/drivers/rekernel\n", encoding="utf-8")
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
        actual = target / ("Makefile.upstream" if name == "Makefile" else name)
        if hashlib.sha256(actual.read_bytes()).hexdigest() != digest:
            raise RuntimeError("Official ReKernel file changed during integration: " + name)
    result = {"source": lock, "linkage": "built-in when CONFIG_REKERNEL=y",
              "official_c_and_header_bytes_preserved": True,
              "transport": "official Generic Netlink (legacy option disabled)",
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
