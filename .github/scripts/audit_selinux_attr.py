#!/usr/bin/env python3
"""Record native credential paths and the exact setprocattr source delta."""
import argparse
import hashlib
import json
from pathlib import Path
import re
import subprocess

from selinux_seqno import function, require
from selinux_projection import unify_current_attr


def audit(kernel, bakasu, report):
    checks = {}
    for rel, names in {
        "kernel/cred.c": ("prepare_creds", "abort_creds", "__put_cred", "put_cred_rcu"),
        "security/security.c": ("security_prepare_creds", "security_cred_free"),
        "security/selinux/hooks.c": ("selinux_cred_prepare",),
    }.items():
        native = subprocess.check_output(["git", "-C", str(kernel), "show", "HEAD:" + rel]).decode()
        actual = (kernel / rel).read_text(encoding="utf-8")
        for name in names:
            before, after = function(native, name), function(actual, name)
            if before != after:
                raise ValueError(f"Credential lifecycle differs from common HEAD: {rel}:{name}")
            checks[name] = {"file": rel, "matches_common_head": True,
                            "sha256": hashlib.sha256(after.encode()).hexdigest()}
    native = subprocess.check_output(["git", "-C", str(kernel), "show", "HEAD:security/selinux/hooks.c"]).decode()
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
    result = {"scope": "Source comparison with the selected common HEAD; not a stock-device timing measurement",
              "common_head": subprocess.check_output(["git", "-C", str(kernel), "rev-parse", "HEAD"], text=True).strip(),
              "credential_functions": checks,
              "setprocattr_only_delta": "Explicit user conversion; parse current newline strings; reject empty current SID before credentials",
              "bakasu_credential_lifecycle_registrations": registrations}
    (report / "credential-path-audit.json").write_text(json.dumps(result, indent=2) + "\n", encoding="utf-8")
    print("Native credential lifecycle and setprocattr source boundaries verified")


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    for flag in ("kernel", "bakasu", "report"):
        parser.add_argument("--" + flag, type=Path, required=True)
    args = parser.parse_args()
    audit(args.kernel.resolve(), args.bakasu.resolve(), args.report.resolve())
