#!/usr/bin/env python3
"""Verify the signed boot payload, trusted test key, metadata and partition size."""
import argparse
import hashlib
import json
import re
import subprocess
import sys
import tempfile
from pathlib import Path


def digest(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def run(*args, cwd=None):
    return subprocess.check_output([str(arg) for arg in args], cwd=cwd, text=True,
                                   stderr=subprocess.STDOUT)


def field(info, label):
    match = re.search(r"^\s*" + re.escape(label) + r":\s+(.+)$", info, re.M)
    if not match:
        raise ValueError(f"AVB metadata field is missing: {label}")
    return match.group(1).strip()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--boot", type=Path, required=True)
    parser.add_argument("--template", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--signer", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    args = parser.parse_args()
    args.report.mkdir(parents=True, exist_ok=True)
    avbtool = args.signer / "tools/avbtool.py"
    if not avbtool.is_file():
        avbtool = args.signer / "tools/avbtool"
    try:
        original = run(sys.executable, avbtool, "info_image", "--image", args.template)
        signed = run(sys.executable, avbtool, "info_image", "--image", args.boot)
        (args.report / "boot-template-avb.txt").write_text(original)
        (args.report / "boot-avb.txt").write_text(signed)
        algorithm = field(signed, "Algorithm")
        if algorithm not in ("SHA256_RSA4096", "SHA256_RSA2048", "SHA512_RSA4096", "SHA512_RSA2048"):
            raise ValueError(f"Unexpected boot signing algorithm: {algorithm}")
        for name in ("Algorithm", "Rollback Index", "Rollback Index Location"):
            if field(original, name) != field(signed, name):
                raise ValueError(f"Signed boot changed template {name}")
        if args.boot.stat().st_size != args.template.stat().st_size:
            raise ValueError("Signed boot size differs from the boot template partition size")
        if field(signed, "Partition Name") != "boot":
            raise ValueError("Missing boot hash descriptor")
        bits = algorithm.rsplit("RSA", 1)[1]
        key = args.signer / f"tools/pem/testkey_rsa{bits}.pem"
        verification = run(sys.executable, avbtool, "verify_image", "--image", args.boot, "--key", key)
        (args.report / "boot-signature-verification.txt").write_text(verification)
        with tempfile.TemporaryDirectory(prefix="verify-boot-") as directory:
            unpack = run((args.signer / "tools/magiskboot").resolve(), "unpack", args.boot.resolve(), cwd=directory)
            embedded = Path(directory) / "kernel"
            if not embedded.is_file() or digest(embedded) != digest(args.kernel):
                raise ValueError("Signed boot does not contain the final compiled kernel Image")
            (args.report / "boot-unpack.txt").write_text(unpack)
        manifest = {
            "boot_sha256": digest(args.boot),
            "kernel_sha256": digest(args.kernel),
            "template_sha256": digest(args.template),
            "partition_bytes": args.boot.stat().st_size,
            "algorithm": algorithm,
            "public_key_sha1": field(signed, "Public key (sha1)"),
            "rollback_index": field(signed, "Rollback Index"),
            "verified": True,
        }
        (args.report / "boot-verification.json").write_text(json.dumps(manifest, indent=2) + "\n")
        (args.report / "SHA256SUMS").write_text(f"{manifest['boot_sha256']}  boot.img\n")
        print(json.dumps(manifest, indent=2))
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Boot verification failed: {error}\n")


if __name__ == "__main__":
    main()
