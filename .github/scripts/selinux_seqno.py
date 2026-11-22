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


def object_code(objdump, path):
    table = subprocess.check_output([objdump, "-t", str(path)], text=True)
    symbols = {}
    for line in table.splitlines():
        match = re.match(r"^([0-9a-f]+)\s+.*?\bF\s+(\S+)\s+([0-9a-f]+)\s+(.+)$", line)
        if match:
            name = match[4].split()[-1]
            symbols[name] = (match[2], int(match[1], 16), int(match[3], 16))
    return symbols, subprocess.check_output([objdump, "-dr", str(path)], text=True)


def function_code(obj, name, optional=False):
    symbols, listing = obj
    if optional and name not in symbols:
        return ""
    require(name in symbols, f"Object code has no function {name}")
    section, start, size = symbols[name]
    require(size > 0, f"Object function {name} has no extent")
    lines = []
    current_section = None
    for line in listing.splitlines():
        heading = re.match(r"Disassembly of section (.+):", line)
        if heading:
            current_section = heading[1]
        address = re.match(r"\s*([0-9a-f]+)(?::| <)", line)
        if current_section == section and address and start <= int(address[1], 16) < start + size:
            lines.append(line)
    # Use the ELF function extent: --disassemble-symbols can stop at an internal
    # ARM64 $d/$x mapping symbol and omit the remaining instructions.
    return "\n".join(lines) + "\n"


def audit_hot_path(kernel):
    source = (kernel / "security/selinux/ss/services.c").read_text(encoding="utf-8")
    body = function(source, "avd_init")
    require("policy->latest_granting" in body, "avd_init lost the real policy generation")
    require(re.search(r"avd->seqno\s*=\s*0\s*;", body), "avd_init lost its early boot generation")
    require(not re.search(r"avd->seqno\s*=\s*1\s*;", body), "Global avd_init hardcodes generation 1")
    hot_path = body + function(source, "security_compute_av")
    return hashlib.sha256(hot_path.encode()).hexdigest()


def apply(kernel, bakasu, report):
    kernel, bakasu = kernel.resolve(), bakasu.resolve()
    hot_path_sha = audit_hot_path(kernel)
    fs_path = kernel / "security/selinux/selinuxfs.c"
    fs = fs_path.read_text(encoding="utf-8")
    require("susfs_access_seqno" not in fs, "Compatibility fix was already applied")
    wrapper = function(fs, "handle_sel_write_access")
    require("current_uid().val >= 10000" in wrapper and "ksu_selinux_hide_running" in wrapper,
            "SUSFS access dispatch predicate changed")
    predicate = """
/* Keep status and access on the same view, including secondary users. */
static noinline bool susfs_hide_user_queries(void)
{
\tuid_t appid = current_uid().val % 100000;

\tif (appid < 10000 || !READ_ONCE(ksu_selinux_hide_running))
\t\treturn false;
\tif (!smp_load_acquire(&fake_status)) {
\t\tif (!selinux_kernel_status_page())
\t\t\treturn false;
\t\tinitialize_fake_status();
\t}
\treturn smp_load_acquire(&fake_status) != NULL;
}
"""
    declaration = "extern void initialize_fake_status(void);"
    require(fs.count(declaration) == 1, "SUSFS fake status declaration changed")
    fs = fs.replace(declaration, declaration + "\n" + predicate, 1)
    fs = replace_function(fs, "handle_sel_write_access", """static ssize_t handle_sel_write_access(struct file *file, char *buf, size_t size)
{
\tif (susfs_hide_user_queries())
\t\treturn susfs_sel_write_access(file, buf, size);
\treturn sel_write_access(file, buf, size);
}""")
    fs = replace_function(fs, "sel_open_handle_status", """static int sel_open_handle_status(struct inode *inode, struct file *filp)
{
\tstruct page *status = selinux_kernel_status_page();

#ifdef CONFIG_KSU_SUSFS
\tif (susfs_hide_user_queries()) {
\t\tfilp->private_data = smp_load_acquire(&fake_status);
\t\treturn 0;
\t}
#endif
\tif (!status)
\t\treturn -ENOMEM;
\tfilp->private_data = status;
\treturn 0;
}""")
    mmap = function(fs, "sel_mmap_handle_status")
    require(all(term in mmap for term in ("vma->vm_pgoff", "PAGE_SIZE", "VM_WRITE", "VM_MAYWRITE")),
            "Status mmap lost upstream ABI guards")
    require("vm_flags_clear(vma, VM_MAYWRITE)" in mmap, "Status mmap no longer clears VM_MAYWRITE")
    require(re.search(r"if\s*\(vma->vm_pgoff\s*>\s*0\s*\|\|\s*size\s*!=\s*(?:__)?PAGE_SIZE\)\s*return -EIO;", mmap),
            "Status mmap offset/size rejection changed")
    require(re.search(r"if\s*\(vma->vm_flags & VM_WRITE\)\s*return -EPERM;", mmap),
            "Status mmap writable rejection changed")
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
    rules = rules_path.read_text(encoding="utf-8")
    cache = function(rules, "reset_avc_cache")
    # Port generation tracking as one unit: publication, cache and notification.
    require("avc_ss_reset(0);" in cache and "avc_ss_reset(avc, 0);" in cache,
            "BakaSU AVC generation implementation changed; inspect before applying")
    require(not re.search(r"\b\w+->latest_granting\s*(?:\+\+|=)", rules),
            "BakaSU now mutates policy generation; compatibility strategy needs review")
    cache = cache.replace("reset_avc_cache()", "reset_avc_cache(u32 seqno)")
    cache = cache.replace("{", "{\n    /* Allocate before notification so late readers observe this generation. */\n"
                          "    selinux_kernel_status_page();", 1)
    cache = cache.replace("avc_ss_reset(0)", "avc_ss_reset(seqno)")
    cache = cache.replace("avc_ss_reset(avc, 0)", "avc_ss_reset(avc, seqno)")
    cache = cache.replace("selnl_notify_policyload(0)", "selnl_notify_policyload(seqno)")
    cache = cache.replace("selinux_status_update_policyload(0)", "selinux_status_update_policyload(seqno)")
    cache = cache.replace("selinux_status_update_policyload(&selinux_state, 0)",
                          "selinux_status_update_policyload(&selinux_state, seqno)")
    rules = replace_function(rules, "reset_avc_cache", cache)
    for name in ("apply_kernelsu_rules", "handle_sepolicy"):
        body = function(rules, name)
        require("mutex_lock(&selinux_state.policy_mutex)" in body,
                f"{name} no longer serializes policy publication")
        publish = "    rcu_assign_pointer(selinux_state.policy, pol);"
        require(body.count(publish) == 1 and body.count("reset_avc_cache();") == 2,
                f"{name} policy publication layout changed")
        body = body.replace(publish, """    u32 seqno = old_pol->latest_granting + 1;
    pol->latest_granting = seqno;
""" + publish, 1)
        body = body.replace("reset_avc_cache();", "reset_avc_cache(seqno);", 1)
        body = body.replace("reset_avc_cache();", "reset_avc_cache(0);", 1)
        rules = replace_function(rules, name, body)

    services_path = kernel / "security/selinux/ss/services.c"
    services = services_path.read_text(encoding="utf-8")
    for name in ("selinux_policy_commit", "security_set_bools"):
        body = function(services, name)
        guards = re.findall(r"^#ifdef CONFIG_KSU_SUSFS\n.*?^#endif[^\n]*\n", body, re.M | re.S)
        require(len(guards) == 2 and "susfs_is_current_ksu_domain" in guards[0]
                and "bypass_orig:" in guards[1], f"{name} SUSFS generation bypass changed")
        for guard in guards:
            body = body.replace(guard, "", 1)
        services = replace_function(services, name, body)
    status_path = kernel / "security/selinux/status.c"
    status = status_path.read_text(encoding="utf-8")
    for name in ("selinux_status_update_policyload", "selinux_status_update_setenforce"):
        body = function(status, name)
        guards = re.findall(r"^#ifdef CONFIG_KSU_SUSFS\n.*?^#endif[^\n]*\n", body, re.M | re.S)
        require(len(guards) == 1 and "susfs_is_current_ksu_domain" in guards[0],
                f"{name} notification bypass changed")
        status = replace_function(status, name, body.replace(guards[0], "", 1))

    hide_path = bakasu / "kernel/feature/selinux_hide.c"
    hide = hide_path.read_text(encoding="utf-8")
    init = function(hide, "initialize_fake_status")
    require(init.count("fake_status = new_page;") == 1, "Fake status initialization changed")
    init = init.replace("    fake_status = new_page;", """#ifdef CONFIG_KSU_SUSFS
    /* Pre-6.10 status uses zero as the initial policyload sentinel. */
#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 10, 0)
    new_status->version = SELINUX_KERNEL_STATUS_VERSION;
    new_status->sequence = 0;
    new_status->policyload = 0;
#endif
#endif
    smp_store_release(&fake_status, new_page);""")
    hide = replace_function(hide, "initialize_fake_status", init)
    fs_path.write_text(fs, encoding="utf-8", newline="\n")
    rules_path.write_text(rules, encoding="utf-8", newline="\n")
    hide_path.write_text(hide, encoding="utf-8", newline="\n")
    services_path.write_text(services, encoding="utf-8", newline="\n")
    status_path.write_text(status, encoding="utf-8", newline="\n")
    require(audit_hot_path(kernel) == hot_path_sha, "Kernel hot path changed")
    report.mkdir(parents=True, exist_ok=True)
    (report / "selinux-source.json").write_text(json.dumps({
        "strategy": "Real generation tracking with a synchronized independent app view",
        "hot_path_sha256": hot_path_sha,
        "services_sha256": hashlib.sha256(services_path.read_bytes()).hexdigest(),
        "access_function": "susfs_sel_write_access",
        "files": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (fs_path, rules_path, hide_path, services_path, status_path)},
    }, indent=2) + "\n")
    print("SELinux source checks passed; avd_init preserves policy generation and early boot zero")


def verify(kernel, report, objdump):
    for relative in ("security/selinux/selinuxfs.i", "security/selinux/status.i", "security/selinux/ss/services.i",
                     "drivers/kernelsu/selinux/rules.i", "drivers/kernelsu/feature/selinux_hide.i",
                     "security/selinux/selinuxfs.o", "drivers/kernelsu/feature/selinux_hide.o"):
        (report / Path(relative).name).write_bytes((kernel / "out" / relative).read_bytes())
    original = json.loads((report / "selinux-source.json").read_text())
    require(audit_hot_path(kernel) == original["hot_path_sha256"], "A later patch changed the kernel AVC hot path")
    services_path = kernel / "security/selinux/ss/services.c"
    require(hashlib.sha256(services_path.read_bytes()).hexdigest() == original["services_sha256"],
            "A later patch changed services.c")
    for path, expected in original["files"].items():
        require(hashlib.sha256(Path(path).read_bytes()).hexdigest() == expected,
                f"A later patch changed {path}")
    config = (kernel / "out/.config").read_text()
    require("CONFIG_KSU_SUSFS=y\n" in config, "SUSFS did not resolve to built-in")
    fs = (kernel / "out/security/selinux/selinuxfs.i").read_text()
    require("susfs_access_seqno(avd.seqno)" in function(fs, "susfs_sel_write_access"),
            "Preprocessor dropped the access fix")
    access = function(fs, "susfs_sel_write_access")
    require(access.index("susfs_security_compute_av_user(") < access.index("susfs_access_seqno(")
            < access.index("scnprintf("), "Access generation clamp does not cover the tclass=0 return path")
    for name in ("sel_open_handle_status", "handle_sel_write_access"):
        require("susfs_hide_user_queries()" in function(fs, name), f"{name} uses a different hide predicate")
    rules = (kernel / "out/drivers/kernelsu/selinux/rules.i").read_text()
    cache = function(rules, "reset_avc_cache")
    for call in ("avc_ss_reset(seqno)", "selnl_notify_policyload(seqno)", "selinux_status_update_policyload(seqno)"):
        require(call in cache, f"Real generation notification is missing: {call}")
    for name in ("apply_kernelsu_rules", "handle_sepolicy"):
        body = function(rules, name)
        require("old_pol->latest_granting + 1" in body and "reset_avc_cache(seqno)" in body,
                f"{name} does not publish and notify the same generation")
    status = (kernel / "out/security/selinux/status.i").read_text()
    require("selinux_kernel_status_page();" in cache,
            "Policy notification can occur before the real status page exists")
    for name in ("selinux_status_update_policyload", "selinux_status_update_setenforce"):
        body = function(status, name)
        require(body.count("status->sequence++") == 2 and "susfs_is_current_ksu_domain" not in body,
                f"{name} lost real status sequence notifications")
    services = (kernel / "out/security/selinux/ss/services.i").read_text()
    for name in ("selinux_policy_commit", "security_set_bools"):
        body = function(services, name)
        require("bypass_orig" not in body and "oldpolicy->latest_granting + 1" in body,
                f"{name} still bypasses generation tracking")
    hide = (kernel / "out/drivers/kernelsu/feature/selinux_hide.i").read_text()
    require(re.search(r"new_status->sequence = 0;\s*new_status->policyload = 0;",
                      function(hide, "initialize_fake_status")),
            "Preprocessor dropped the native 6.6 fake status sentinel")
    obj = kernel / "out/security/selinux/selinuxfs.o"
    fs_object = object_code(objdump, obj)
    output = function_code(fs_object, "susfs_access_seqno")
    require("<susfs_access_seqno>:" in output, "Compatibility function is absent from object code")
    # Clang can simplify min(unsigned_seqno, 1) to unsigned_seqno != 0.
    boolean_clamp = (re.search(r"\bcmp\s+w0,\s*#(?:0x)?0\b", output)
                     and re.search(r"\bcset\s+w0,\s*ne\b", output))
    threshold_clamp = (re.search(r"\b(cmp|subs)\b[^\n]*#(?:0x)?1\b", output)
                       and re.search(r"\b(csel|csinc|b\.[a-z]+)\b", output))
    require(boolean_clamp or threshold_clamp, "Object code lacks the generation clamp")
    (report / "susfs_access_seqno.disasm").write_text(output)
    for name in ("sel_open_handle_status", "handle_sel_write_access"):
        calls = function_code(fs_object, name)
        require("susfs_hide_user_queries" in calls, f"{name} lost the shared predicate call in object code")
        (report / (name + ".disasm")).write_text(calls)
    access_calls = (function_code(fs_object, "handle_sel_write_access")
                    + function_code(fs_object, "susfs_sel_write_access", optional=True))
    require("susfs_access_seqno" in access_calls, "Compiled access path lost the generation clamp call")
    (report / "access-calls.disasm").write_text(access_calls)
    predicate_code = function_code(fs_object, "susfs_hide_user_queries")
    (report / "fake-status-acquire.disasm").write_text(predicate_code)
    hide_obj = kernel / "out/drivers/kernelsu/feature/selinux_hide.o"
    hide_object = object_code(objdump, hide_obj)
    publish_code = function_code(hide_object, "initialize_fake_status")
    (report / "fake-status-publish.disasm").write_text(publish_code)
    (report / "selinux-hide-full.disasm").write_text(hide_object[1])
    require(re.search(r"\b(ldar|ldapr)\b", predicate_code), "Fake page loads lost acquire ordering")
    require(re.search(r"\bstlr\b", publish_code), "Fake page publication lost release ordering")
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
