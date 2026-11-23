"""Materialize pre-link LLVM bitcode for existing machine-code audits.

The original kernel build objects remain unchanged. This audits the same
translation-unit scope as the non-LTO object gate; final-link ABI and device
tests remain separate gates.
"""
import argparse
import hashlib
import json
import pathlib
import subprocess


def materialize(source, clang, report):
    source, clang, report = map(pathlib.Path, (source, clang, report))
    data = source.read_bytes()
    if data[:4] not in (b"BC\xc0\xde", b"\xde\xc0\x17\x0b"):
        raise ValueError("Input is not LLVM bitcode: " + str(source))
    report.mkdir(parents=True, exist_ok=True)
    unique = hashlib.sha256(str(source).encode()).hexdigest()[:12]
    output = report / (source.stem + "-" + unique + ".audit-elf.o")
    provenance = output.with_suffix(".json")
    digest = hashlib.sha256(data).hexdigest()
    if output.exists() and provenance.exists():
        old = json.loads(provenance.read_text(encoding="utf-8"))
        if old.get("bitcode_sha256") == digest and old.get("clang") == str(clang):
            if old.get("audit_elf_sha256") == hashlib.sha256(output.read_bytes()).hexdigest():
                return output
    command = [str(clang), "--target=aarch64-linux-gnu", "-O2", "-fno-lto",
               "-x", "ir", "-c", str(source), "-o", str(output)]
    subprocess.run(command, check=True)
    if output.read_bytes()[:4] != b"\x7fELF":
        raise RuntimeError("Bitcode materialization did not produce ELF")
    provenance.write_text(json.dumps({
        "bitcode_source": str(source), "bitcode_sha256": digest,
        "audit_elf_sha256": hashlib.sha256(output.read_bytes()).hexdigest(),
        "clang": str(clang), "command": command,
        "scope": "Code generated from the pre-link IR; original build objects are unchanged. This is not a substitute for final-link ABI or runtime verification."
    }, indent=2), encoding="utf-8")
    return output


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--source", type=pathlib.Path, required=True)
    parser.add_argument("--clang", type=pathlib.Path, required=True)
    parser.add_argument("--report", type=pathlib.Path, required=True)
    args = parser.parse_args()
    print(materialize(args.source, args.clang, args.report))
