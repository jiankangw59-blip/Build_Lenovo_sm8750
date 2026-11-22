"""Keep user queries on the live SID table and project only their decisions."""
import hashlib
import re


def unify_current_attr(body, require):
    """Parse nonempty current labels before credentials; retain clear semantics elsewhere."""
    marker = "\tchar *str = value;"
    require(body.count(marker) == 1, "Procattr value declaration changed")
    body = body.replace(marker, marker + '\n\tbool current_attr = !strcmp(name, "current");', 1)
    require(body.count('else if (!strcmp(name, "current"))') == 2,
            "Procattr current dispatch changed")
    body = body.replace('else if (!strcmp(name, "current"))', 'else if (current_attr)')
    marker = "\tif (size && str[0] && str[0] != '\\n') {"
    require(body.count(marker) == 1, "Procattr newline/clear dispatch changed")
    body = body.replace(marker, "\tif (size && str[0] && (current_attr || str[0] != '\\n')) {", 1)
    marker = "\tnew = prepare_creds();"
    require(body.count(marker) == 1, "Procattr credential preparation changed")
    body = body.replace(marker, "\t/* current cannot be cleared; reject before allocating credentials. */\n"
                        "\tif (current_attr && !sid)\n\t\treturn -EINVAL;\n\n" + marker, 1)
    return body


def apply(kernel, function, replace_function, require):
    services_path = kernel / "security/selinux/ss/services.c"
    header_path = kernel / "security/selinux/include/security.h"
    hooks_path = kernel / "security/selinux/hooks.c"
    fs_path = kernel / "security/selinux/selinuxfs.c"
    services = services_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    hooks = hooks_path.read_text(encoding="utf-8")
    fs = fs_path.read_text(encoding="utf-8")

    # The selector may run with RCU held: no allocation, sleeping or AVC checks.
    selector = """
#ifdef CONFIG_KSU_SUSFS
extern bool ksu_selinux_hide_running;
extern struct selinux_policy *backup_sepolicy;

/* The published snapshot is immutable and retained until shutdown. */
noinline struct selinux_policy *security_user_query_policy(void)
{
	if (current_uid().val % 100000 < 10000 ||
	    !READ_ONCE(ksu_selinux_hide_running))
		return NULL;
	return smp_load_acquire(&backup_sepolicy);
}

static int security_project_context(struct selinux_policy *source_policy,
				    struct selinux_policy *target_policy,
				    struct context *source,
				    struct context *projected);

/* Caller holds RCU; no SID is ever allocated in the snapshot. */
static noinline void security_compute_av_user_projected(
	struct selinux_policy *live, struct selinux_policy *snapshot,
	u32 ssid, u32 tsid, u16 tclass, struct av_decision *avd)
{
	struct context *source, *target, projected_source, projected_target;
	struct policydb *db = &snapshot->policydb;

	avd_init(snapshot, avd);
	source = sidtab_search(live->sidtab, ssid);
	target = sidtab_search(live->sidtab, tsid);
	if (!source || !target)
		return;
	if (security_project_context(live, snapshot, source, &projected_source))
		return;
	if (security_project_context(live, snapshot, target, &projected_target))
		goto out_source;
	if (ebitmap_get_bit(&db->permissive_map, projected_source.type))
		avd->flags |= AVD_FLAGS_PERMISSIVE;
	if (unlikely(!tclass)) {
		if (db->allow_unknown)
			avd->allowed = 0xffffffff;
		goto out;
	}
	context_struct_compute_av(db, &projected_source, &projected_target,
				  tclass, avd, NULL);
out:
	context_destroy(&projected_target);
out_source:
	context_destroy(&projected_source);
}
#endif

"""
    # Put the helper immediately before the user-only entry, after avd_init.
    body = function(services, "security_compute_av_user")
    services = services.replace(body, selector + body, 1)
    body = function(services, "security_compute_av_user")
    marker = "\tpolicy = rcu_dereference(selinux_state.policy);"
    require(body.count(marker) == 1, "User AV entry changed")
    body = body.replace(marker, marker + """
#ifdef CONFIG_KSU_SUSFS
	if (selinux_initialized()) {
		struct selinux_policy *snapshot = security_user_query_policy();

		if (snapshot) {
			security_compute_av_user_projected(policy, snapshot, ssid,
						   tsid, tclass, avd);
			goto out;
		}
	}
#endif""", 1)
    services = replace_function(services, "security_compute_av_user", body)

    helpers = """
#ifdef CONFIG_KSU_SUSFS
/* Symbol values may change on reload. Translate by name, never by numeric ID. */
static int security_project_context(struct selinux_policy *source_policy,
				    struct selinux_policy *target_policy,
				    struct context *source,
				    struct context *projected)
{
	char *text;
	u32 len;
	int rc;

	rc = context_struct_to_string(&source_policy->policydb, source, &text, &len);
	if (rc)
		return rc;
	/* def_sid is NULL, so the parser cannot dereference a snapshot SID table. */
	rc = string_to_context_struct(&target_policy->policydb, NULL, text,
				      projected, SECSID_NULL);
	kfree(text);
	return rc;
}

/* Parse the raw string once; map its canonical form into the live namespace. */
static noinline int security_parse_user_context(struct selinux_policy *live,
	struct selinux_policy *snapshot, char *text, struct context *context)
{
	struct context clean;
	int rc;

	/* text is the core's private writable copy; the caller's input is intact. */
	rc = string_to_context_struct(&snapshot->policydb, NULL, text,
				      &clean, SECSID_NULL);
	if (rc)
		return rc;
	/* Duplicate MLS categories are collapsed before the live parser sees them.
	 * This retains native live validation without rescanning a long raw string.
	 * The mapper uses names and GFP_ATOMIC allocations under the caller's RCU.
	 */
	rc = security_project_context(snapshot, live, &clean, context);
	context_destroy(&clean);
	return rc;
}
#endif

"""
    core = function(services, "security_context_to_sid_core")
    services = services.replace(core, helpers + core, 1)
    core = function(services, "security_context_to_sid_core")
    core = core.replace("int force)", "int force, bool user_query)", 1)
    core = core.replace("\tstruct selinux_policy *policy;", "\tstruct selinux_policy *policy;\n"
                        "#ifdef CONFIG_KSU_SUSFS\n\tstruct selinux_policy *snapshot;\n#endif", 1)
    marker = "\trc = string_to_context_struct(policydb, sidtab, scontext2,"
    require(core.count(marker) == 1, "Native context parser call changed")
    core = core.replace(marker, """#ifdef CONFIG_KSU_SUSFS
	snapshot = user_query ? security_user_query_policy() : NULL;
	if (snapshot)
		rc = security_parse_user_context(policy, snapshot, scontext2, &context);
	else
#endif
""" + marker, 1)
    # A policy replacement can cause ESTALE after the parser consumed its copy.
    stale = "\t\tcontext_destroy(&context);\n\t\tgoto retry;"
    require(core.count(stale) == 1, "Context conversion retry cleanup changed")
    core = core.replace(stale, "\t\tcontext_destroy(&context);\n"
                        "\t\tmemcpy(scontext2, scontext, scontext_len);\n"
                        "\t\tscontext2[scontext_len] = '\\0';\n\t\tgoto retry;", 1)
    services = replace_function(services, "security_context_to_sid_core", core)
    for name in ("security_context_to_sid", "security_context_to_sid_default",
                 "security_context_to_sid_force"):
        body = function(services, name)
        body, count = re.subn(r"(sid, (?:SECSID_NULL|def_sid), (?:gfp|gfp_flags|GFP_KERNEL), [01])\);",
                              r"\1, false);", body)
        require(count == 1, f"Native context wrapper changed: {name}")
        services = replace_function(services, name, body)
    wrappers = """

/* Explicit user-input boundary; all successful conversions use the live SID table. */
int security_context_to_sid_user(const char *scontext, u32 scontext_len,
				 u32 *sid, gfp_t gfp)
{
	return security_context_to_sid_core(scontext, scontext_len, sid,
					    SECSID_NULL, gfp, 0, true);
}

int security_context_str_to_sid_user(const char *scontext, u32 *sid, gfp_t gfp)
{
	return security_context_to_sid_user(scontext, strlen(scontext), sid, gfp);
}
"""
    body = function(services, "security_context_str_to_sid")
    services = services.replace(body, body + wrappers, 1)

    marker = "struct selinux_policy;"
    require(header.count(marker) == 1, "Security Server policy declaration changed")
    header = header.replace(marker, marker + """
#ifdef CONFIG_KSU_SUSFS
struct selinux_policy *security_user_query_policy(void);
#endif
int security_context_to_sid_user(const char *scontext, u32 scontext_len,
				 u32 *sid, gfp_t gfp);
int security_context_str_to_sid_user(const char *scontext, u32 *sid, gfp_t gfp);
""", 1)

    # Remove SUSFS's copies/wrappers, rather than maintaining a second VFS/LSM flow.
    for name in ("susfs_sel_write_context", "handle_sel_write_context",
                 "susfs_sel_write_access", "handle_sel_write_access"):
        fs = replace_function(fs, name, "")
    # Empty CONFIG blocks are removed below. The table points at native handlers.
    for node in ("CONTEXT", "ACCESS"):
        native = "sel_write_" + node.lower()
        pattern = (r"#ifdef CONFIG_KSU_SUSFS\n\s*\[SEL_" + node
                   + r"\] = handle_" + native + r",\n#else\n(\s*\[SEL_"
                   + node + r"\] = " + native + r",\n)#endif[^\n]*")
        fs, count = re.subn(pattern, r"\1", fs)
        require(count == 1, f"Native transaction table entry changed: {node}")
    # Preserve permission checks, transaction order, formatting and error codes.
    for name in ("sel_write_context", "sel_write_access", "sel_write_create",
                 "sel_write_relabel", "sel_write_member", "sel_write_user",
                 "sel_write_validatetrans"):
        body = function(fs, name)
        body = re.sub(r"\bsecurity_context_to_sid\(", "security_context_to_sid_user(", body)
        body = re.sub(r"\bsecurity_context_str_to_sid\(", "security_context_str_to_sid_user(", body)
        fs = replace_function(fs, name, body)
    # Status allocation happens here, outside the shared selector and RCU.
    fs = replace_function(fs, "sel_open_handle_status", """static int sel_open_handle_status(struct inode *inode, struct file *filp)
{
	struct page *status = selinux_kernel_status_page();

#ifdef CONFIG_KSU_SUSFS
	if (security_user_query_policy()) {
		if (!smp_load_acquire(&fake_status))
			initialize_fake_status();
		if (!smp_load_acquire(&fake_status))
			return -ENOMEM;
		filp->private_data = smp_load_acquire(&fake_status);
		return 0;
	}
#endif
	if (!status)
		return -ENOMEM;
	filp->private_data = status;
	return 0;
}""")
    for declaration in (
        "static ssize_t handle_sel_write_access(struct file *file, char *buf, size_t size);",
        "extern bool ksu_selinux_snapshot_ready(void);",
        "extern ssize_t my_write_access(struct file *file, char *buf, size_t size);",
        "extern ssize_t my_write_context(struct file *file, char *buf, size_t size);",
        "extern int susfs_security_context_to_sid(const char *scontext, u32 scontext_len, u32 *sid, gfp_t gfp);",
        "extern int susfs_security_context_str_to_sid(const char *scontext, u32 *sid, gfp_t gfp);",
        "extern void susfs_security_compute_av_user(u32 ssid, u32 tsid, u16 tclass, struct av_decision *avd);",
    ):
        fs = fs.replace(declaration + "\n", "")
    fs = re.sub(r"#ifdef CONFIG_KSU_SUSFS\n\s*#endif[^\n]*\n", "", fs)

    for name in ("susfs_selinux_inode_setxattr", "susfs_selinux_inode_post_setxattr",
                 "handle_selinux_inode_setxattr", "handle_selinux_inode_post_setxattr",
                 "susfs_selinux_setprocattr", "handle_selinux_setprocattr"):
        hooks = replace_function(hooks, name, "")
    hooks = re.sub(r"#ifdef CONFIG_KSU_SUSFS\n\s*#endif[^\n]*\n", "", hooks)
    for hook, native in (("inode_setxattr", "selinux_inode_setxattr"),
                         ("inode_post_setxattr", "selinux_inode_post_setxattr"),
                         ("setprocattr", "selinux_setprocattr")):
        pattern = (r"#ifdef CONFIG_KSU_SUSFS\n\s*LSM_HOOK_INIT\(" + hook
                   + ", handle_" + native + r"\),\n#else\n(\s*LSM_HOOK_INIT\("
                   + hook + ", " + native + r"\),\n)#endif[^\n]*")
        # inode_setxattr and post_setxattr share one conditional block.
        if hook != "setprocattr":
            hooks = hooks.replace("LSM_HOOK_INIT(" + hook + ", handle_" + native + ")",
                                  "LSM_HOOK_INIT(" + hook + ", " + native + ")")
        else:
            hooks, count = re.subn(pattern, r"\1", hooks)
            require(count == 1, "Native procattr LSM registration changed")
    hooks = re.sub(r"#ifdef CONFIG_KSU_SUSFS\n(\s*LSM_HOOK_INIT\(inode_setxattr, selinux_inode_setxattr\),\n"
                   r"\s*LSM_HOOK_INIT\(inode_post_setxattr, selinux_inode_post_setxattr\),\n)"
                   r"#else\n\1#endif[^\n]*", r"\1", hooks)
    body = function(hooks, "selinux_setprocattr")
    require(body.count("security_context_to_sid(value, size,") == 1, "Native procattr parser changed")
    # Keep permissions first and live transitions; malformed current strings use
    # the parser instead of allocating and then aborting credentials.
    body = body.replace("security_context_to_sid(value, size,", "security_context_to_sid_user(value, size,", 1)
    body = unify_current_attr(body, require)
    hooks = replace_function(hooks, "selinux_setprocattr", body)
    require("handle_selinux_" not in hooks and "susfs_selinux_" not in hooks,
            "SUSFS still replaces a native LSM entry")

    for path, text in ((services_path, services), (header_path, header),
                       (hooks_path, hooks), (fs_path, fs)):
        path.write_text(text, encoding="utf-8", newline="\n")
    return {str(path): hashlib.sha256(path.read_bytes()).hexdigest()
            for path in (services_path, header_path, hooks_path, fs_path)}


def verify(kernel, report, objdump, function, object_code, function_code, require):
    fs = (kernel / "out/security/selinux/selinuxfs.i").read_text()
    hooks = (kernel / "out/security/selinux/hooks.i").read_text()
    services = (kernel / "out/security/selinux/ss/services.i").read_text()
    fs_source = (kernel / "security/selinux/selinuxfs.c").read_text()
    hooks_source = (kernel / "security/selinux/hooks.c").read_text()
    source = (kernel / "security/selinux/ss/services.c").read_text()
    require("[SEL_ACCESS] = sel_write_access," in fs and "my_write_access" not in fs
            and "[SEL_CONTEXT] = sel_write_context," in fs and "my_write_context" not in fs,
            "A VFS snapshot replacement is still installed")
    require("handle_selinux_setprocattr" not in hooks and "susfs_selinux_" not in hooks,
            "A second LSM state machine is still installed")
    entries = ("sel_write_context", "sel_write_access", "sel_write_create",
               "sel_write_relabel", "sel_write_member", "sel_write_user",
               "sel_write_validatetrans")
    for name in entries:
        body = function(fs, name)
        require("_to_sid_user(" in body, f"{name} misses explicit user-input validation")
        require("avc_has_perm(" in body, f"{name} lost its native permission check")
    require("security_context_to_sid_user(value, size," in function(hooks, "selinux_setprocattr"),
            "Procattr does not share user-input validation")
    # Other native bodies remain exact. current has the audited rejection-flow delta.
    for path, input_name, names in (
        (fs_source, "selinuxfs-input.c", entries),
        (hooks_source, "hooks-input.c", ("selinux_setprocattr", "selinux_inode_setxattr",
                                        "selinux_inode_post_setxattr", "selinux_secctx_to_secid")),
    ):
        original = (report / input_name).read_text()
        for name in names:
            normalized = function(path, name).replace("security_context_to_sid_user(", "security_context_to_sid(")
            normalized = normalized.replace("security_context_str_to_sid_user(", "security_context_str_to_sid(")
            expected = function(original, name)
            if name == "selinux_setprocattr":
                expected = unify_current_attr(expected, require)
            require(normalized == expected, f"Unexpected execution/order changed: {name}")
    for name in ("security_context_to_sid", "security_context_to_sid_default", "security_context_to_sid_force"):
        require(", false);" in function(source, name), f"Internal conversion enters projection: {name}")
    require(", true);" in function(source, "security_context_to_sid_user"),
            "The user API does not explicitly select validation")
    core_source = function(source, "security_context_to_sid_core")
    require(core_source.count("memcpy(scontext2, scontext, scontext_len)") == 1
            and core_source.index("memcpy(scontext2") > core_source.index("if (rc == -ESTALE)"),
            "Input restoration escaped the stale SID retry branch")
    core = function(services, "security_context_to_sid_core")
    require("user_query ? security_user_query_policy()" in core and "security_parse_user_context(policy," in core,
            "The compiled core lost explicit user-query validation")
    require(core.index("security_parse_user_context(") < core.index("sidtab_context_to_sid("),
            "Rejected labels can enter the live SID table")
    validator = function(services, "security_parse_user_context")
    require(validator.count("string_to_context_struct(") == 1
            and "security_project_context(snapshot, live, &clean, context)" in validator
            and "context_destroy(&clean)" in validator
            and "sidtab_context_to_sid" not in validator and "kmemdup" not in validator,
            "User parsing rescans the raw string or registers a private SID")
    mapper = function(services, "security_project_context")
    require("context_struct_to_string(&source_policy->policydb" in mapper
            and "string_to_context_struct(&target_policy->policydb" in mapper
            and "kfree(text)" in mapper, "Snapshot symbol mapping assumes matching numeric IDs")
    projected = function(services, "security_compute_av_user_projected")
    require("avd_init(snapshot, avd)" in projected and "sidtab_search(live->sidtab" in projected
            and "security_compute_av_user_ids(snapshot," in projected,
            "User AV projection no longer reads live SIDs and snapshot rules")
    require("avd->seqno = 1" not in projected, "The projector hardcodes generation instead of using the snapshot")
    selector = function(services, "security_user_query_policy")
    require("100000 < 10000" in selector and "ksu_selinux_hide_running" in selector,
            "User/multi-user/isolated UID selection changed")
    require(not re.search(r"alloc|mutex_|avc_has_perm|initialize_fake", selector),
            "The shared selector can sleep, allocate or alter AVC counters")
    require("security_user_query_policy()" in function(services, "security_compute_av_user")
            and "security_user_query_policy()" in core
            and "security_user_query_policy()" in function(fs, "sel_open_handle_status"),
            "Status, context and access select different views")
    rules = (kernel / "drivers/kernelsu/selinux/rules.c").read_text()
    capture = function(rules, "apply_kernelsu_rules")
    require("snapshot->sidtab = NULL;" in capture and "policydb_load_isids(&snapshot" not in capture,
            "The snapshot owns or aliases a second SID table")
    require(capture.index("snapshot->sidtab = NULL;")
            < capture.index("smp_store_release(&backup_sepolicy, snapshot)")
            < capture.index("ksu_type(db, KERNEL_SU_DOMAIN"),
            "Snapshot publication follows policy modification")
    # Inspect compiled calls, not just source text or preprocessor output.
    fs_obj = object_code(objdump, kernel / "out/security/selinux/selinuxfs.o")
    hooks_obj = object_code(objdump, kernel / "out/security/selinux/hooks.o")
    ss_obj = object_code(objdump, kernel / "out/security/selinux/ss/services.o")
    for name in entries:
        calls = function_code(fs_obj, name)
        require("_to_sid_user" in calls, f"Compiled {name} bypasses the user API")
        (report / (name + ".disasm")).write_text(calls)
    procattr = function_code(hooks_obj, "selinux_setprocattr")
    require("security_context_to_sid_user" in procattr, "Compiled procattr bypasses the user API")
    (report / "selinux_setprocattr.disasm").write_text(procattr)
    for name in ("security_user_query_policy", "security_parse_user_context",
                 "security_compute_av_user", "security_compute_av_user_projected",
                 "security_context_to_sid_user"):
        (report / (name + ".disasm")).write_text(function_code(ss_obj, name))
    require("security_compute_av_user_projected" in function_code(ss_obj, "security_compute_av_user"),
            "Compiled user AV entry does not call the projector")
    require("security_parse_user_context" in ss_obj[1], "Compiled context validation is unreachable")
    require(re.search(r"\b(ldar|ldapr)\b", function_code(ss_obj, "security_user_query_policy")),
            "Snapshot selector lost acquire ordering")
    status = function_code(fs_obj, "sel_open_handle_status")
    require("security_user_query_policy" in status and "initialize_fake_status" in status,
            "Compiled status selection/initialization changed")
    (report / "sel_open_handle_status.disasm").write_text(status)
    (report / "services-projection-full.disasm").write_text(ss_obj[1])
