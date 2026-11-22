#!/usr/bin/env python3
"""Record native credential paths and the exact setprocattr source delta."""
import argparse
import hashlib
import json
from pathlib import Path
import re

from selinux_seqno import function, require
from selinux_projection import unify_current_attr

NATIVE_FILES = {
    "kernel/cred.c": "native-cred.c",
    "security/security.c": "native-security.c",
    "security/selinux/hooks.c": "native-hooks.c",
}


def snapshot(kernel, report):
    baseline = report / "credential-baseline"
    baseline.mkdir(parents=True, exist_ok=True)
    hashes = {}
    for rel, name in NATIVE_FILES.items():
        data = (kernel / rel).read_bytes()
        (baseline / name).write_bytes(data)
        hashes[rel] = hashlib.sha256(data).hexdigest()
    (baseline / "manifest.json").write_text(json.dumps({
        "scope": "Captured immediately after unpacking the selected common source archive, before feature patches",
        "sha256": hashes,
    }, indent=2), encoding="utf-8")
    print("Unpacked native credential source baseline captured")


def audit(kernel, bakasu, report):
    baseline = report / "credential-baseline"
    captured = json.loads((baseline / "manifest.json").read_text(encoding="utf-8"))
    for rel, name in NATIVE_FILES.items():
        require(hashlib.sha256((baseline / name).read_bytes()).hexdigest() == captured["sha256"][rel],
                "Native credential baseline changed: " + rel)
    checks = {}
    for rel, names in {
        "kernel/cred.c": ("prepare_creds", "abort_creds", "__put_cred", "put_cred_rcu"),
        "security/security.c": ("security_prepare_creds", "security_cred_free"),
        "security/selinux/hooks.c": ("selinux_cred_prepare",),
    }.items():
        native = (baseline / NATIVE_FILES[rel]).read_text(encoding="utf-8")
        actual = (kernel / rel).read_text(encoding="utf-8")
        for name in names:
            before, after = function(native, name), function(actual, name)
            if before != after:
                raise ValueError(f"Credential lifecycle differs from unpacked common: {rel}:{name}")
            checks[name] = {"file": rel, "matches_unpacked_baseline": True,
                            "sha256": hashlib.sha256(after.encode()).hexdigest()}
    native = (baseline / NATIVE_FILES["security/selinux/hooks.c"]).read_text(encoding="utf-8")
    actual = (kernel / "security/selinux/hooks.c").read_text(encoding="utf-8")
    before, after = function(native, "selinux_setprocattr"), function(actual, "selinux_setprocattr")
    restored = after.replace("security_context_to_sid_user(", "security_context_to_sid(")
    if unify_current_attr(before, require) != restored:
        raise ValueError("setprocattr changed beyond the audited current rejection flow")
    registrations = []
    for path in (bakasu / "kernel").rglob("*.c"):
        text = path.read_text(encoding="utf-8")
        for match in re.finditer(r"LSM_HOOK_INIT\s*\(\s*(cred_prepare|cred_alloc_blank|cred_free)\s*,", text):
            registrations.append({"file": str(path.relative_to(bakasu)), "hook": match[1]})
    if registrations:
        raise ValueError("BakaSU registers a credential lifecycle hook: " + str(registrations))
    report.mkdir(parents=True, exist_ok=True)
    result = {"scope": "Source comparison with the selected common archive before feature patches; not a stock-device timing measurement",
              "baseline_sha256": captured["sha256"],
              "credential_functions": checks,
              "setprocattr_only_delta": "Explicit user conversion; parse current newline strings; reject empty current SID before credentials",
              "bakasu_credential_lifecycle_registrations": registrations}
    (report / "credential-path-audit.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Native credential lifecycle and setprocattr source boundaries verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", nargs="?", default="audit", choices=("snapshot", "audit"))
    for flag in ("kernel", "report"):
        parser.add_argument("--" + flag, type=Path, required=True)
    parser.add_argument("--bakasu", type=Path)
    args = parser.parse_args()
    if args.mode == "snapshot":
        snapshot(args.kernel.resolve(), args.report.resolve())
    else:
        require(args.bakasu is not None, "--bakasu required for audit")
        audit(args.kernel.resolve(), args.bakasu.resolve(), args.report.resolve())
