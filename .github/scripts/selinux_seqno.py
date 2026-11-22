#!/usr/bin/env python3
"""Apply and verify the BakaSU/SUSFS userspace generation compatibility fix."""
import argparse
import hashlib
import json
import re
import subprocess
from pathlib import Path


def function_span(source, name):
    # Require a declaration prefix, so an earlier if (function(...)) is not
    # mistaken for the definition when scanning a source file.
    attribute = r"__attribute__\s*\(\([^\n;{}]*?\)\)"
    match = re.search(r"^[ \t]*(?:" + attribute + r"[ \t]*|[A-Za-z_]\w*[ \t]+)+\**[ \t]*"
                      + re.escape(name) + r"\([^;{}]*\)\s*\{", source, re.M)
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
    require("susfs_hide_user_queries" not in fs, "Compatibility fix was already applied")
    wrapper = function(fs, "handle_sel_write_access")
    require("current_uid().val >= 10000" in wrapper and "ksu_selinux_hide_running" in wrapper,
            "SUSFS access dispatch predicate changed")
    predicate = """
/* Keep status and access on the same view, including secondary users. */
static noinline bool susfs_hide_user_queries(void)
{
\tuid_t appid = current_uid().val % 100000;

\tif (appid < 10000 || !READ_ONCE(ksu_selinux_hide_running) ||
\t    !ksu_selinux_snapshot_ready())
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
    fs = fs.replace(declaration, declaration + """
extern bool ksu_selinux_snapshot_ready(void);
extern ssize_t my_write_access(struct file *file, char *buf, size_t size);
extern ssize_t my_write_context(struct file *file, char *buf, size_t size);
""" + predicate, 1)
    fs = replace_function(fs, "handle_sel_write_access", """static ssize_t handle_sel_write_access(struct file *file, char *buf, size_t size)
{
\tif (susfs_hide_user_queries())
\t\treturn susfs_sel_write_access(file, buf, size);
\treturn sel_write_access(file, buf, size);
}""")
    context_wrapper = function(fs, "handle_sel_write_context")
    require("current_uid().val >= 10000" in context_wrapper,
            "SUSFS context dispatch predicate changed")
    fs = replace_function(fs, "handle_sel_write_context", """static ssize_t handle_sel_write_context(struct file *file, char *buf, size_t size)
{
\tif (susfs_hide_user_queries())
\t\treturn susfs_sel_write_context(file, buf, size);
\treturn sel_write_context(file, buf, size);
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
    # The sparse SUSFS registry does not cover additions between existing
    # public types. Use BakaSU's original policy object for the whole decision.
    fs = replace_function(fs, "susfs_sel_write_access", """static ssize_t susfs_sel_write_access(struct file *file, char *buf, size_t size)
{
\treturn my_write_access(file, buf, size);
}
""")
    require("susfs_security_context_to_sid" in function(fs, "susfs_sel_write_context"),
            "Unexpected SUSFS context validator")
    fs = replace_function(fs, "susfs_sel_write_context", """static ssize_t susfs_sel_write_context(struct file *file, char *buf, size_t size)
{
\treturn my_write_context(file, buf, size);
}""")

    rules_path = bakasu / "kernel/selinux/rules.c"
    rules = rules_path.read_text(encoding="utf-8")
    register_types = """
#ifdef CONFIG_KSU_SUSFS
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
extern int susfs_add_sus_type(const char *secctx, u32 type_value);

/* Register only the new concrete types in the policy about to be published. */
static noinline void ksu_susfs_register_new_types(struct selinux_policy *old,
                                               struct selinux_policy *new)
{
    u32 index;

    for (index = old->policydb.p_types.nprim;
         index < new->policydb.p_types.nprim; index++) {
        struct type_datum *type = new->policydb.type_val_to_struct[index];
        const char *name = new->policydb.sym_val_to_name[SYM_TYPES][index];

        if (type && name && !type->attribute)
            susfs_add_sus_type(name, type->value);
    }
}
#endif
#endif

"""
    require("ksu_susfs_register_new_types" not in rules, "Type registration was already patched")
    start, _ = function_span(rules, "reset_avc_cache")
    rules = rules[:start] + register_types + rules[start:]
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
        if name == "apply_kernelsu_rules":
            start = body.index("    backup_sepolicy = ksu_dup_sepolicy(old_pol);")
            end = body.index("    pol = ksu_dup_sepolicy(old_pol);", start)
            body = body[:start] + """    /* Capture once, before any KSU or module policy mutations. */
    if (!smp_load_acquire(&backup_sepolicy)) {
        struct selinux_policy *snapshot = ksu_dup_sepolicy(old_pol);

        if (IS_ERR(snapshot)) {
            pr_err("failed to create backup sepolicy: %ld\\n", PTR_ERR(snapshot));
        } else {
            snapshot->sidtab = kzalloc(sizeof(*snapshot->sidtab), GFP_KERNEL);
            if (!snapshot->sidtab) {
                ksu_destroy_sepolicy(snapshot);
            } else {
                int ret = policydb_load_isids(&snapshot->policydb, snapshot->sidtab);

                if (ret) {
                    kfree(snapshot->sidtab);
                    ksu_destroy_sepolicy(snapshot);
                } else {
                    /* Readers never observe a partially constructed SID table. */
                    smp_store_release(&backup_sepolicy, snapshot);
                    pr_notice("SELinux clean snapshot ready: generation=%u types=%u\\n",
                              snapshot->latest_granting, snapshot->policydb.p_types.nprim);
                }
            }
        }
    }
""" + body[end:]
        body = body.replace(publish, """    u32 seqno = old_pol->latest_granting + 1;
    pol->latest_granting = seqno;
#ifdef CONFIG_KSU_SUSFS
    ksu_susfs_register_new_types(old_pol, pol);
#endif
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
    snapshot_ready = """#ifdef CONFIG_KSU_SUSFS
/* The boot snapshot stays immutable and alive for the built-in SUSFS lifetime. */
noinline bool ksu_selinux_snapshot_ready(void)
{
#if LINUX_VERSION_CODE >= KERNEL_VERSION(5, 10, 0)
    return smp_load_acquire(&backup_sepolicy) != NULL;
#else
    return backup_policydb && backup_sidtab;
#endif
}
#endif

"""
    start, _ = function_span(hide, "my_write_context")
    hide = hide[:start] + snapshot_ready + hide[start:]
    for name in ("my_write_context", "my_write_access"):
        body = function(hide, name)
        marker = "#endif\n    char *"
        require(body.count(marker) == 1, f"{name} query entry changed")
        body = body.replace(marker, """#endif
#ifdef CONFIG_KSU_SUSFS
    if (!ksu_selinux_snapshot_ready())
        return -EAGAIN;
#endif
    char *""", 1)
        hide = replace_function(hide, name, body)
    # A pure query needs no live SID. The procattr path still supplies
    # orig_sid_p and keeps the upstream conversion/transition semantics.
    live_sid = """        if (!orig_rc) {
#ifdef KSU_COMPAT_HAS_CONTEXT_ADD_HASH
            orig_rc = context_add_hash(orig_policydb, &orig_context);
#endif
            if (!orig_rc) {
                // sync to global sidtab
                orig_rc = sidtab_context_to_sid(orig_sidtab, &orig_context, &orig_sid);
                if (orig_sid_p)
                    *orig_sid_p = orig_sid;
            }
            context_destroy(&orig_context);
        }"""
    require(hide.count(live_sid) == 1, "BakaSU live SID synchronization changed")
    hide = hide.replace(live_sid, """        if (!orig_rc) {
            /* Validation/access queries must not grow the live SID table. */
            if (orig_sid_p) {
#ifdef KSU_COMPAT_HAS_CONTEXT_ADD_HASH
                orig_rc = context_add_hash(orig_policydb, &orig_context);
#endif
                if (!orig_rc) {
                    orig_rc = sidtab_context_to_sid(orig_sidtab, &orig_context, &orig_sid);
                    if (!orig_rc)
                        *orig_sid_p = orig_sid;
                }
            }
            context_destroy(&orig_context);
        }""", 1)
    policy_lookup = """    rcu_read_lock();
    orig_policy = rcu_dereference(selinux_state.policy);
    orig_policydb = &orig_policy->policydb;
    orig_sidtab = orig_policy->sidtab;
    policydb = &policy->policydb;
    sidtab = policy->sidtab;"""
    require(hide.count(policy_lookup) == 1, "BakaSU query policy lookup changed")
    hide = hide.replace(policy_lookup, """    rcu_read_lock();
    policydb = &policy->policydb;
    sidtab = policy->sidtab;
    if (orig_sid_p) {
        orig_policy = rcu_dereference(selinux_state.policy);
        orig_policydb = &orig_policy->policydb;
        orig_sidtab = orig_policy->sidtab;
    } else {
        /* Pure queries parse only the snapshot, including symbol lookups. */
        orig_policydb = policydb;
        orig_sidtab = sidtab;
    }""", 1)
    drop = function(hide, "ksu_selinux_hide_drop_backup_if_unused")
    opening, closing = drop.index("{"), drop.rfind("}")
    drop = (drop[:opening + 1] + "\n#ifndef CONFIG_KSU_SUSFS\n" + drop[opening + 1:closing]
            + "#endif\n" + drop[closing:])
    hide = replace_function(hide, "ksu_selinux_hide_drop_backup_if_unused", drop)
    susfs_path = kernel / "fs/susfs.c"
    susfs = susfs_path.read_text(encoding="utf-8")
    declaration = "extern bool susfs_is_current_ksu_domain(void);"
    require(susfs.count(declaration) == 1, "SUSFS identity declarations changed")
    susfs = susfs.replace(declaration, declaration + """
#ifndef CONFIG_KSU_DISABLE_MANAGER
extern bool ksu_is_manager_uid(u32 uid);
#endif
""", 1)
    entry = "struct st_susfs_sus_type_hlist {\n\tstruct hlist_node                       node;\n};"
    require(susfs.count(entry) == 1, "SUSFS type registry layout changed")
    susfs = susfs.replace(entry, entry.replace("\n};", "\n\tu32 type_value;\n};"), 1)
    body = function(susfs, "susfs_add_sus_type")
    marker = "hash_for_each_possible_safe(SUS_TYPE_HLIST, tmp_entry, tmp_hlist_node, node, type_value) {"
    require(body.count(marker) == 1, "SUSFS type insertion loop changed")
    body = body.replace(marker, marker + "\n\t\tif (tmp_entry->type_value != type_value)\n\t\t\tcontinue;", 1)
    marker = "\thash_add_rcu(SUS_TYPE_HLIST, &new_entry->node, type_value);"
    require(body.count(marker) == 1, "SUSFS type publication changed")
    body = body.replace(marker, "\tnew_entry->type_value = type_value;\n" + marker, 1)
    susfs = replace_function(susfs, "susfs_add_sus_type", body)
    body = function(susfs, "susfs_is_sus_type_rcu")
    require(body.count("\t\treturn true;") == 1, "SUSFS type lookup changed")
    body = body.replace("\t\treturn true;", "\t\tif (entry->type_value == type_value)\n\t\t\treturn true;", 1)
    susfs = replace_function(susfs, "susfs_is_sus_type_rcu", body)
    body = function(susfs, "susfs_is_uid_allowed_for_sus_path")
    marker = "\tif (uid >= 90000)"
    require(body.count(marker) == 1, "SUSFS path UID filter changed")
    body = body.replace(marker, """#ifndef CONFIG_KSU_DISABLE_MANAGER
    /* A manager must retain access to its own private app data. */
    if (ksu_is_manager_uid(uid))
        return true;
#endif

""" + marker, 1)
    susfs = replace_function(susfs, "susfs_is_uid_allowed_for_sus_path", body)
    fs_path.write_text(fs, encoding="utf-8", newline="\n")
    rules_path.write_text(rules, encoding="utf-8", newline="\n")
    hide_path.write_text(hide, encoding="utf-8", newline="\n")
    services_path.write_text(services, encoding="utf-8", newline="\n")
    status_path.write_text(status, encoding="utf-8", newline="\n")
    susfs_path.write_text(susfs, encoding="utf-8", newline="\n")
    require(audit_hot_path(kernel) == hot_path_sha, "Kernel hot path changed")
    report.mkdir(parents=True, exist_ok=True)
    (report / "selinux-source.json").write_text(json.dumps({
        "strategy": "Real generation tracking; hidden status/context/access use an immutable boot policy snapshot",
        "hot_path_sha256": hot_path_sha,
        "services_sha256": hashlib.sha256(services_path.read_bytes()).hexdigest(),
        "access_function": "my_write_access",
        "context_function": "my_write_context",
        "files": {str(p): hashlib.sha256(p.read_bytes()).hexdigest()
                  for p in (fs_path, rules_path, hide_path, services_path, status_path, susfs_path)},
    }, indent=2) + "\n")
    print("SELinux source checks passed; avd_init preserves policy generation and early boot zero")


def verify(kernel, report, objdump):
    for relative in ("security/selinux/selinuxfs.i", "security/selinux/status.i", "security/selinux/ss/services.i",
                     "drivers/kernelsu/selinux/rules.i", "drivers/kernelsu/feature/selinux_hide.i",
                     "fs/susfs.i", "fs/susfs.o", "drivers/kernelsu/selinux/rules.o",
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
    for name, target in (("susfs_sel_write_access", "my_write_access"),
                         ("susfs_sel_write_context", "my_write_context")):
        body = function(fs, name)
        require(f"return {target}(file, buf, size);" in body
                and "susfs_security_" not in body,
                f"{name} still queries the live module-modified policy")
    for name in ("sel_open_handle_status", "handle_sel_write_access", "handle_sel_write_context"):
        require("susfs_hide_user_queries()" in function(fs, name), f"{name} uses a different hide predicate")
    require("ksu_selinux_snapshot_ready()" in function(fs, "susfs_hide_user_queries"),
            "The shared hide predicate can select an absent policy snapshot")
    rules = (kernel / "out/drivers/kernelsu/selinux/rules.i").read_text()
    rules_source = (kernel / "drivers/kernelsu/selinux/rules.c").read_text()
    cache = function(rules, "reset_avc_cache")
    for call in ("avc_ss_reset(seqno)", "selnl_notify_policyload(seqno)", "selinux_status_update_policyload(seqno)"):
        require(call in cache, f"Real generation notification is missing: {call}")
    for name in ("apply_kernelsu_rules", "handle_sepolicy"):
        body = function(rules, name)
        require("old_pol->latest_granting + 1" in body and "reset_avc_cache(seqno)" in body,
                f"{name} does not publish and notify the same generation")
        require("ksu_susfs_register_new_types(old_pol, pol)" in body,
                f"{name} lost type registration during preprocessing")
        raw_body = function(rules_source, name)
        require(raw_body.index("ksu_susfs_register_new_types(old_pol, pol)")
                < raw_body.index("rcu_assign_pointer("), f"{name} publishes types before registration")
    registry = function(rules, "ksu_susfs_register_new_types")
    require(re.search(r"old->policydb\.(?:p_types|symtab\[\s*(?:3|SYM_TYPES)\s*\])\.nprim", registry)
            and "!type->attribute" in registry
            and "susfs_add_sus_type(name, type->value)" in registry,
            "SUSFS new-type registration did not survive preprocessing")
    boot_rules = function(rules_source, "apply_kernelsu_rules")
    require("if (!smp_load_acquire(&backup_sepolicy))" in boot_rules,
            "The clean snapshot can be replaced by a later dirty policy")
    require(boot_rules.index("policydb_load_isids(&snapshot->policydb")
            < boot_rules.index("smp_store_release(&backup_sepolicy, snapshot)")
            < boot_rules.index("ksu_type(db, KERNEL_SU_DOMAIN"),
            "Snapshot initialization/publication must precede the first KSU rule")
    susfs = (kernel / "out/fs/susfs.i").read_text()
    require("entry->type_value == type_value" in function(susfs, "susfs_is_sus_type_rcu")
            and "tmp_entry->type_value != type_value" in function(susfs, "susfs_add_sus_type"),
            "SUSFS hash bucket collisions can hide unrelated policy types")
    require("ksu_is_manager_uid(uid)" in function(susfs, "susfs_is_uid_allowed_for_sus_path"),
            "SUSFS lost the manager private-data exception")
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
    for name in ("my_write_access", "my_write_context"):
        query = function(hide, name)
        require("ksu_selinux_snapshot_ready()" in query and re.search(r"return\s*-(?:11|EAGAIN);", query),
                f"{name} lacks its snapshot readiness guard")
        require("security_context_to_sid_with_policy(backup_sepolicy," in query,
                f"{name} lost the original-policy context lookup")
    access = function(hide, "my_write_access")
    require(access.index("security_compute_av_user_with_policy(backup_sepolicy,")
            < access.index("avd.seqno = 1;") < access.index("scnprintf("),
            "Snapshot access must preserve class0 generation1 before formatting")
    require("avd->seqno = 1;" in function(hide, "avd_init"),
            "The private snapshot decision initializer lost its clean generation")
    require("!tclass" in function(hide, "ksu_security_compute_av_user"),
            "The snapshot query lost the class0 branch")
    conversion = function(hide, "security_context_to_sid_with_policy")
    require("orig_policydb = policydb;" in conversion and "orig_sidtab = sidtab;" in conversion,
            "Pure queries can inspect live-only policy symbols")
    require(re.search(r"if \(orig_sid_p\)\s*\{(?:(?!context_destroy).)*"
                      r"sidtab_context_to_sid\(orig_sidtab", conversion, re.S),
            "Pure context queries can mutate the live SID table")
    require("backup_sepolicy" not in function(hide, "ksu_selinux_hide_drop_backup_if_unused"),
            "SUSFS queries can race with snapshot disposal")
    enable = function(hide, "ksu_selinux_hide_enable")
    require("ksu_patch_text(" not in enable and "selinux_write_op" not in enable,
            "SUSFS and BakaSU are both installing SELinux write hooks")
    obj = kernel / "out/security/selinux/selinuxfs.o"
    fs_object = object_code(objdump, obj)
    for name in ("sel_open_handle_status", "handle_sel_write_access", "handle_sel_write_context"):
        calls = function_code(fs_object, name)
        require("susfs_hide_user_queries" in calls, f"{name} lost the shared predicate call in object code")
        (report / (name + ".disasm")).write_text(calls)
    for name, wrapper, target in (("access", "handle_sel_write_access", "my_write_access"),
                                  ("context", "handle_sel_write_context", "my_write_context")):
        calls = (function_code(fs_object, wrapper)
                 + function_code(fs_object, "susfs_sel_write_" + name, optional=True))
        require(target in calls, f"Compiled {name} path lost the snapshot callback")
        require("susfs_security_" not in calls,
                f"Compiled {name} path still enters the live SUSFS policy query")
        (report / (name + "-calls.disasm")).write_text(calls)
    predicate_code = function_code(fs_object, "susfs_hide_user_queries")
    (report / "fake-status-acquire.disasm").write_text(predicate_code)
    hide_obj = kernel / "out/drivers/kernelsu/feature/selinux_hide.o"
    hide_object = object_code(objdump, hide_obj)
    readiness = function_code(hide_object, "ksu_selinux_snapshot_ready")
    require(re.search(r"\b(ldar|ldapr)\b", readiness), "Snapshot reads lost acquire ordering")
    (report / "snapshot-ready.disasm").write_text(readiness)
    for name in ("my_write_access", "my_write_context"):
        query_code = function_code(hide_object, name)
        require("backup_sepolicy" in query_code and "ksu_selinux_snapshot_ready" in query_code,
                f"Compiled {name} lost its guarded snapshot lookup")
        (report / (name + ".disasm")).write_text(query_code)
    rules_object = object_code(objdump, kernel / "out/drivers/kernelsu/selinux/rules.o")
    registration = function_code(rules_object, "ksu_susfs_register_new_types")
    require("susfs_add_sus_type" in registration, "Object code lost the type registry callback")
    (report / "new-types-registration.disasm").write_text(registration)
    susfs_object = object_code(objdump, kernel / "out/fs/susfs.o")
    manager_exception = function_code(susfs_object, "susfs_is_uid_allowed_for_sus_path")
    require("ksu_is_manager_uid" in manager_exception, "Object code lost the manager data exception")
    (report / "manager-data-exception.disasm").write_text(manager_exception)
    publish_code = function_code(hide_object, "initialize_fake_status")
    (report / "fake-status-publish.disasm").write_text(publish_code)
    (report / "selinux-hide-full.disasm").write_text(hide_object[1])
    require(re.search(r"\b(ldar|ldapr)\b", predicate_code), "Fake page loads lost acquire ordering")
    require(re.search(r"\bstlr\b", publish_code), "Fake page publication lost release ordering")
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
