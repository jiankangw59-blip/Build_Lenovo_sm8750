#!/usr/bin/env python3
"""Apply and verify the BakaSU/SUSFS userspace generation compatibility fix."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def function_span(source, name):
    match = re.search(r"^[^\n;{}]*\b" + re.escape(name) + r"\([^;{}]*\)\s*\{", source, re.M)
    if not match:
        raise ValueError(f"Missing function definition: {name}")
    opening = source.index("{", match.start())
    depth = 1
    end = opening + 1
    while depth:
        depth += (source[end] == "{") - (source[end] == "}")
        end += 1
    return match.start(), end


def function(source, name):
    start, end = function_span(source, name)
    return source[start:end]


def replace_function(source, name, replacement):
    start, end = function_span(source, name)
    return source[:start] + replacement + source[end:]


def require(condition, message):
    if not condition:
        raise ValueError(message)


def audit_hot_path(kernel):
    source = (kernel / "security/selinux/ss/services.c").read_text()
    body = function(source, "avd_init")
    require("policy->latest_granting" in body, "avd_init lost the real policy generation")
    require(re.search(r"avd->seqno\s*=\s*0\s*;", body), "avd_init lost its early boot generation")
    require(not re.search(r"avd->seqno\s*=\s*1\s*;", body), "Global avd_init hardcodes generation 1")
    return hashlib.sha256(source.encode()).hexdigest()


def apply(kernel, bakasu, report):
    hot_path_sha = audit_hot_path(kernel)
    fs_path = kernel / "security/selinux/selinuxfs.c"
    fs = fs_path.read_text()
    require("susfs_access_seqno" not in fs, "Compatibility fix was already applied")
    wrapper = function(fs, "handle_sel_write_access")
    require("current_uid().val >= 10000" in wrapper and "ksu_selinux_hide_running" in wrapper,
            "SUSFS access dispatch predicate changed")
    body = function(fs, "susfs_sel_write_access")
    require(body.count("susfs_security_compute_av_user(ssid, tsid, tclass, &avd);") == 1,
            "Unexpected SUSFS compute_av signature")
    helper = """/* Match the fake status page only in the existing app hide path. */
static noinline u32 susfs_access_seqno(u32 seqno)
{
\tif (seqno > 1)
\t\treturn 1;
\treturn seqno;
}

"""
    body = body.replace("\tlength = scnprintf(buf, SIMPLE_TRANSACTION_LIMIT,",
                        "\tavd.seqno = susfs_access_seqno(avd.seqno);\n\n"
                        "\tlength = scnprintf(buf, SIMPLE_TRANSACTION_LIMIT,", 1)
    fs = replace_function(fs, "susfs_sel_write_access", helper + body)

    rules_path = bakasu / "kernel/selinux/rules.c"
    rules = rules_path.read_text()
    cache = function(rules, "reset_avc_cache")
    # This fork uses reset(0) and preserves latest_granting. Keep status at the
    # same generation under SUSFS, including later ksud sepolicy patches.
    require("avc_ss_reset(0);" in cache and "avc_ss_reset(avc, 0);" in cache,
            "BakaSU AVC generation implementation changed; inspect before applying")
    require(not re.search(r"\b\w+->latest_granting\s*(?:\+\+|=)", rules),
            "BakaSU now mutates policy generation; compatibility strategy needs review")
    calls = re.findall(r"^([ \t]*selinux_status_update_policyload\([^\n]+;)$", cache, re.M)
    require(len(calls) == 2, "Expected both version branches of the status update")
    for call in calls:
        cache = cache.replace(call, "#ifndef CONFIG_KSU_SUSFS\n" + call + "\n#endif", 1)
    rules = replace_function(rules, "reset_avc_cache", cache)

    hide_path = bakasu / "kernel/feature/selinux_hide.c"
    hide = hide_path.read_text()
    init = function(hide, "initialize_fake_status")
    require(init.count("fake_status = new_page;") == 1, "Fake status initialization changed")
    init = init.replace("    fake_status = new_page;", """#ifdef CONFIG_KSU_SUSFS
    /* The built-in SUSFS view represents the initial loaded policy. */
    new_status->policyload = 1;
#endif
    fake_status = new_page;""")
    hide = replace_function(hide, "initialize_fake_status", init)
    fs_path.write_text(fs)
    rules_path.write_text(rules)
    hide_path.write_text(hide)
    require(audit_hot_path(kernel) == hot_path_sha, "Kernel hot path changed")
    report.mkdir(parents=True, exist_ok=True)
    (report / "selinux-source.json").write_text(json.dumps({
        "strategy": "SUSFS generation preservation and synchronized app view",
        "services_sha256": hot_path_sha,
        "access_function": "susfs_sel_write_access",
        "files": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (fs_path, rules_path, hide_path)},
    }, indent=2) + "\n")
    print("SELinux source checks passed; avd_init preserves policy generation and early boot zero")


def verify(kernel, report, objdump):
    original = json.loads((report / "selinux-source.json").read_text())
    require(audit_hot_path(kernel) == original["services_sha256"], "A later patch changed services.c")
    config = (kernel / "out/.config").read_text()
    require("CONFIG_KSU_SUSFS=y\n" in config, "SUSFS did not resolve to built-in")
    fs = (kernel / "out/security/selinux/selinuxfs.i").read_text()
    require("susfs_access_seqno(avd.seqno)" in function(fs, "susfs_sel_write_access"),
            "Preprocessor dropped the access fix")
    rules = (kernel / "out/drivers/kernelsu/selinux/rules.i").read_text()
    require("selinux_status_update_policyload(" not in function(rules, "reset_avc_cache"),
            "SUSFS still resets status.policyload to zero")
    hide = (kernel / "out/drivers/kernelsu/feature/selinux_hide.i").read_text()
    require("new_status->policyload = 1;" in function(hide, "initialize_fake_status"),
            "Preprocessor dropped fake status generation normalization")
    obj = kernel / "out/security/selinux/selinuxfs.o"
    output = subprocess.check_output([objdump, "-dr", "--disassemble-symbols=susfs_access_seqno", str(obj)], text=True)
    require("<susfs_access_seqno>:" in output, "Compatibility function is absent from object code")
    # Clang can simplify min(unsigned_seqno, 1) to unsigned_seqno != 0.
    boolean_clamp = (re.search(r"\bcmp\s+w0,\s*#(?:0x)?0\b", output)
                     and re.search(r"\bcset\s+w0,\s*ne\b", output))
    threshold_clamp = (re.search(r"\b(cmp|subs)\b[^\n]*#(?:0x)?1\b", output)
                       and re.search(r"\b(csel|csinc|b\.[a-z]+)\b", output))
    require(boolean_clamp or threshold_clamp, "Object code lacks the generation clamp")
    (report / "susfs_access_seqno.disasm").write_text(output)
    for relative in ("security/selinux/selinuxfs.i", "drivers/kernelsu/selinux/rules.i",
                     "drivers/kernelsu/feature/selinux_hide.i"):
        (report / Path(relative).name).write_bytes((kernel / "out" / relative).read_bytes())
    print(output)
    print("SELinux preprocessor and object code gates passed")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("mode", choices=("apply", "verify"))
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--bakasu", type=Path)
    parser.add_argument("--report", type=Path, required=True)
    parser.add_argument("--objdump", default="llvm-objdump")
    args = parser.parse_args()
    try:
        if args.mode == "apply":
            require(args.bakasu is not None, "--bakasu is required for apply")
            apply(args.kernel, args.bakasu, args.report)
        else:
            verify(args.kernel, args.report, args.objdump)
    except (ValueError, OSError, subprocess.CalledProcessError) as error:
        parser.exit(1, f"SELinux gate failed: {error}\n")


if __name__ == "__main__":
    main()
