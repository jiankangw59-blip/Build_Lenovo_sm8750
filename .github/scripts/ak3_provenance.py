#!/usr/bin/env python3
"""Record compiler output and the unchanged upstream protected AK3 separately."""
import argparse
import hashlib
import json
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--ak3", type=Path, required=True)
    p.add_argument("--kernel", type=Path, required=True)
    p.add_argument("--report", type=Path, required=True)
    a = p.parse_args()
    if not a.kernel.is_file() or a.kernel.stat().st_size < 1048576:
        p.error("Missing compiled ARM64 Image")
    if a.kernel.read_bytes()[56:60] != b"ARM\x64":
        p.error("Compiled payload is not an uncompressed ARM64 Image")
    for rel in ("anykernel.sh", "tools/ak3-loader", "tools/ak3-core.sh",
                "META-INF/com/google/android/update-binary"):
        if not (a.ak3 / rel).is_file():
            p.error("Missing upstream installer component: " + rel)
    payloads = list(a.ak3.glob("*.7z"))
    if len(payloads) != 1 or payloads[0].stat().st_size < 1048576:
        p.error("Expected exactly one nonempty protected upstream payload")
    a.report.mkdir(parents=True, exist_ok=True)
    data = {
        "kernel_sha256": sha(a.kernel),
        "kernel_bytes": a.kernel.stat().st_size,
        "protected_archive_contents_verified": False,
        "flash_payload": "Image, independently exported from the compiler output",
        "ak3_files": {f.relative_to(a.ak3).as_posix(): sha(f)
                      for f in sorted(a.ak3.rglob("*")) if f.is_file()},
    }
    (a.report / "ak3-provenance.json").write_text(json.dumps(data, indent=2) + "\n")
    print(json.dumps(data, indent=2))


if __name__ == "__main__":
    main()
