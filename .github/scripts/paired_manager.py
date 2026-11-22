#!/usr/bin/env python3
"""Reproduce a verified boot's BakaSU changes before building its manager."""
import argparse
import hashlib
import importlib.util
import json
import os
import re
import subprocess
import sys
import tempfile
import zipfile
from pathlib import Path


def sha(path):
    return hashlib.sha256(path.read_bytes()).hexdigest()


def load_module(path, name):
    spec = importlib.util.spec_from_file_location(name, path)
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def prepare(args):
    source, evidence = args.source.resolve(), args.evidence.resolve()
    boot = json.loads((evidence / "boot-verification.json").read_text())
    if boot.get("verified") is not True:
        raise ValueError("The source boot did not pass its pre-flash gate")
    commit = (evidence / "bakasu-commit.txt").read_text().strip()
    actual = subprocess.check_output(["git", "-C", str(source), "rev-parse", "HEAD"], text=True).strip()
    if not re.fullmatch(r"[0-9a-f]{40}", commit) or actual != commit:
        raise ValueError("Manager checkout differs from the verified boot's BakaSU commit")
    package = args.package
    if not re.fullmatch(r"[A-Za-z][A-Za-z0-9_]*(?:\.[A-Za-z][A-Za-z0-9_]*)+", package):
        raise ValueError("Invalid manager package name")
    recorded = json.loads((evidence / "selinux-source.json").read_text())
    adapter = load_module(Path(__file__).with_name("selinux_seqno.py"), "seqno_adapter")
    with tempfile.TemporaryDirectory(prefix="paired-source-") as directory:
        fixture = Path(directory)
        inputs = {
            "selinuxfs-input.c": "security/selinux/selinuxfs.c",
            "services-input.c": "security/selinux/ss/services.c",
            "status-input.c": "security/selinux/status.c",
            "susfs-input.c": "fs/susfs.c",
            "hooks-input.c": "security/selinux/hooks.c",
            "security-input.h": "security/selinux/include/security.h",
        }
        for filename, relative in inputs.items():
            target = fixture / relative
            target.parent.mkdir(parents=True, exist_ok=True)
            target.write_bytes((evidence / filename).read_bytes())
        for filename, relative in (("rules-input.c", "kernel/selinux/rules.c"),
                                   ("selinux_hide-input.c", "kernel/feature/selinux_hide.c")):
            if ((source / relative).read_text(encoding="utf-8")
                    != (evidence / filename).read_text(encoding="utf-8")):
                raise ValueError(f"BakaSU input source drifted: {relative}")
        adapter.apply(fixture, source, fixture / "report")
        reproduced = {}
        for absolute, expected in recorded["files"].items():
            if "/KernelSU/" in absolute:
                relative = absolute.split("/KernelSU/", 1)[1]
                path = source / relative
            else:
                relative = absolute.split("/common/", 1)[1]
                path = fixture / relative
            if sha(path) != expected:
                raise ValueError(f"Reproduced patch differs from compiled boot: {relative}")
            reproduced[relative] = expected
    pair = (evidence / "workflow-commit.txt").read_text().strip()
    if not re.fullmatch(r"[0-9a-f]{40}", pair):
        raise ValueError("Missing source workflow commit")
    build_tag = "selinux-" + pair[:7]
    gradle = source / "manager/build.gradle.kts"
    text = gradle.read_text(encoding="utf-8")
    old = 'extra["managerVersionName"] = getGitDescribe()'
    if text.count(old) != 1:
        raise ValueError("Manager version declaration changed")
    gradle.write_text(text.replace(old, old + f' + "-{build_tag}"'), encoding="utf-8", newline="\n")
    rust = source / "userspace/ksud/build.rs"
    text = rust.read_text(encoding="utf-8")
    old = 'println!("cargo:rustc-env=VERSION_NAME={name}");'
    if text.count(old) != 1:
        raise ValueError("ksud version declaration changed")
    rust.write_text(text.replace(old, f'println!("cargo:rustc-env=VERSION_NAME={{}}-{build_tag}", name.trim());'),
                    encoding="utf-8", newline="\n")
    manifest = {
        "kernel_run": args.kernel_run,
        "workflow_commit": pair,
        "bakasu_commit": commit,
        "boot_sha256": boot["boot_sha256"],
        "kernel_sha256": boot["kernel_sha256"],
        "source_hashes": reproduced,
        "package": package,
        "build_tag": build_tag,
        "uapi_header_sha256": sha(source / "uapi/ksu.h"),
        "signing": "Applied after packaging; see manager-verification.json",
    }
    assets = source / "manager/app/src/main/assets"
    assets.mkdir(parents=True, exist_ok=True)
    (assets / "paired-build.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    args.report.mkdir(parents=True, exist_ok=True)
    (args.report / "paired-build.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    diff = subprocess.check_output(["git", "-C", str(source), "diff", "--binary"])
    (args.report / "bakasu-paired.patch").write_bytes(diff)
    print(json.dumps(manifest, indent=2))


def package(args):
    source = args.source.resolve()
    apks = list((source / "manager/app/build/outputs/apk/release").glob("*-arm64-v8a-*.apk"))
    if len(apks) != 1:
        raise ValueError(f"Expected one ARM64 manager APK, got {apks}")
    daemon = source / "userspace/ksud/target/aarch64-linux-android/release/ksud"
    module = load_module(source / "repack_apk.py", "upstream_repack")
    manifest = json.loads((args.report / "paired-build.json").read_text())
    args.report.mkdir(parents=True, exist_ok=True)
    raw = args.report / "manager-repack-unsigned.apk"
    final = args.report / f"BakaSU-{manifest['build_tag']}-arm64-unsigned.apk"
    module.repack_apk(apks[0], raw, ["arm64-v8a"], {"arm64-v8a": daemon})
    subprocess.run([str(args.zipalign), "-f", "-P", "16", "4", str(raw), str(final)], check=True)
    with zipfile.ZipFile(final) as archive:
        embedded = archive.read("lib/arm64-v8a/libksud.so")
        if hashlib.sha256(embedded).hexdigest() != sha(daemon):
            raise ValueError("Packaged ksud differs from the locally compiled binary")
        if json.loads(archive.read("assets/paired-build.json")) != manifest:
            raise ValueError("APK paired-build metadata differs from the compiled source")
    raw.unlink()
    manifest.update(apk_sha256=sha(final), ksud_sha256=sha(daemon))
    (args.report / "manager-verification.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (args.report / "SHA256SUMS").write_text(f"{sha(final)}  {final.name}\n", encoding="utf-8")
    (args.report / "ksud").write_bytes(daemon.read_bytes())
    print(json.dumps(manifest, indent=2))


def sign(args):
    report = args.report.resolve()
    manifest = json.loads((report / "manager-verification.json").read_text())
    apks = list(report.glob("*-unsigned.apk"))
    if len(apks) != 1:
        raise ValueError("Expected exactly one unsigned manager APK")
    unsigned = apks[0]
    if sha(unsigned) != manifest["apk_sha256"]:
        raise ValueError("Unsigned APK changed after the payload gate")
    expected = args.expected_cert_sha256.lower()
    if not re.fullmatch(r"[0-9a-f]{64}", expected):
        raise ValueError("A pinned owner certificate SHA256 is required")
    if not os.environ.get(args.password_env):
        raise ValueError("The signing password environment variable is empty")
    signed = report / unsigned.name.replace("-unsigned.apk", ".apk")
    subprocess.run([str(args.apksigner), "sign", "--ks", str(args.keystore),
                    "--ks-key-alias", args.alias,
                    "--ks-pass", "env:" + args.password_env,
                    "--key-pass", "env:" + args.password_env,
                    # BakaSU's kernel and ksud require a v2-only APK signing block.
                    "--v1-signing-enabled", "false", "--v2-signing-enabled", "true",
                    "--v3-signing-enabled", "false",
                    "--v4-signing-enabled", "false", "--out", str(signed), str(unsigned)],
                   check=True, capture_output=True)
    result = subprocess.run([str(args.apksigner), "verify", "--verbose", "--print-certs", str(signed)],
                            check=True, capture_output=True, text=True)
    verification = result.stdout + result.stderr
    (report / "apk-signature-verification.txt").write_text(verification, encoding="utf-8")
    certificate = re.search(r"Signer #1 certificate SHA-256 digest: ([0-9a-f]+)", verification)
    if not certificate or certificate[1] != expected:
        raise ValueError("Signed APK does not use the pinned owner certificate")
    schemes = dict(re.findall(r"Verified using (v[\d.]+) scheme[^\n]*: (true|false)", verification))
    if schemes.get("v2") != "true" or any(
            schemes.get(version) != "false" for version in ("v1", "v3", "v3.1", "v4")):
        raise ValueError("BakaSU requires v2-only signing without v1/v3/v4")
    with zipfile.ZipFile(signed) as archive:
        if hashlib.sha256(archive.read("lib/arm64-v8a/libksud.so")).hexdigest() != manifest["ksud_sha256"]:
            raise ValueError("Signing changed the paired ksud payload")
        provenance = json.loads(archive.read("assets/paired-build.json"))
        if (provenance["boot_sha256"] != manifest["boot_sha256"]
                or provenance["source_hashes"] != manifest["source_hashes"]):
            raise ValueError("Signing changed the source/boot pairing metadata")
    manifest.update(signing="owner-signed", certificate_sha256=expected,
                    unsigned_apk_sha256=manifest["apk_sha256"], apk_sha256=sha(signed),
                    apk_filename=signed.name)
    (report / "manager-verification.json").write_text(json.dumps(manifest, indent=2) + "\n", encoding="utf-8")
    (report / "SHA256SUMS").write_text(f"{sha(signed)}  {signed.name}\n", encoding="utf-8")
    unsigned.unlink()
    print(f"Owner-signed APK verified: {signed.name}; certificate SHA256={expected}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("prepare", "package", "sign"))
    parser.add_argument("--source", type=Path, required=True)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--evidence", type=Path)
    parser.add_argument("--kernel-run", type=int)
    parser.add_argument("--package", default="com.resukisu.resukisu")
    parser.add_argument("--zipalign", type=Path)
    parser.add_argument("--apksigner", type=Path)
    parser.add_argument("--keystore", type=Path)
    parser.add_argument("--alias")
    parser.add_argument("--password-env", default="OWNER_STORE_PASSWORD")
    parser.add_argument("--expected-cert-sha256", default="")
    args = parser.parse_args()
    try:
        if args.mode == "prepare":
            if args.evidence is None or args.kernel_run is None:
                raise ValueError("prepare requires --evidence and --kernel-run")
            prepare(args)
        elif args.mode == "package":
            if args.zipalign is None:
                raise ValueError("package requires --zipalign")
            package(args)
        else:
            if args.apksigner is None or args.keystore is None or not args.alias:
                raise ValueError("sign requires --apksigner, --keystore and --alias")
            sign(args)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"Paired manager gate failed: {error}\n")


if __name__ == "__main__":
    main()
