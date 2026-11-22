"""Add a policy-bound, validated ID fast path to the canonical query projection."""
from pathlib import Path


def apply(kernel, bakasu, function, replace_function, require):
    services_path = kernel / "security/selinux/ss/services.c"
    layout_path = kernel / "security/selinux/ss/services.h"
    header_path = kernel / "security/selinux/include/security.h"
    rules_path = bakasu / "kernel/selinux/rules.c"
    services = services_path.read_text(encoding="utf-8")
    layout = layout_path.read_text(encoding="utf-8")
    header = header_path.read_text(encoding="utf-8")
    rules = rules_path.read_text(encoding="utf-8")

    marker = "\tu32 latest_granting;\n} __randomize_layout;"
    require(layout.count(marker) == 1, "Policy structure changed")
    layout = layout.replace(marker, """\tu32 latest_granting;
#ifdef CONFIG_KSU_SUSFS
	const struct selinux_policy *user_query_ids_snapshot;
	bool user_query_ids_compatible;
#endif
} __randomize_layout;

#ifdef CONFIG_KSU_SUSFS
/* Preserve the accepted kernel's existing prefix for precompiled consumers. */
struct selinux_policy_user_ids_legacy_prefix {
	struct sidtab *sidtab;
	struct policydb policydb;
	struct selinux_map map;
	u32 latest_granting;
};
""" + "\n".join(
        f'_Static_assert(__builtin_offsetof(struct selinux_policy, {field}) == '
        f'__builtin_offsetof(struct selinux_policy_user_ids_legacy_prefix, {field}), '
        f'"selinux_policy prefix changed: {field}");'
        for field in ("sidtab", "policydb", "map", "latest_granting")) + "\n#endif", 1)
    marker = "struct selinux_policy *security_user_query_policy(void);"
    require(header.count(marker) == 1, "User selector declaration changed")
    header = header.replace(marker, marker + "\nvoid security_prepare_user_policy(struct selinux_policy *policy);\n"
                            "void security_prepare_user_policy_reuse(struct selinux_policy *policy,\n"
                            "\tstruct selinux_policy *previous, bool schema_unchanged);", 1)

    marker = "\t\t\t\t    struct context *projected);"
    require(services.count(marker) == 1, "Canonical mapper declaration changed")
    helpers = Path(__file__).with_name("selinux_ids_compat.c").read_text(encoding="utf-8")
    services = services.replace(marker, marker + "\n\n" + helpers, 1)
    projected = """static noinline void security_compute_av_user_projected(
	struct selinux_policy *live, struct selinux_policy *snapshot,
	u32 ssid, u32 tsid, u16 tclass, struct av_decision *avd)
{
	struct context *source, *target;

	avd_init(snapshot, avd);
	source = sidtab_search(live->sidtab, ssid);
	target = sidtab_search(live->sidtab, tsid);
	if (!source || !target)
		return;
	if (!security_user_ids_compatible(live, snapshot)) {
		security_compute_av_user_mapped(live, snapshot, source, target, tclass, avd);
		return;
	}
	security_compute_av_user_ids(snapshot, source, target, tclass, avd);
}"""
    services = replace_function(services, "security_compute_av_user_projected", projected)

    # Generate the guarded parser from the actual upstream function. Native callers
    # retain the exact original function; only the explicit user fast path uses this.
    native = function(services, "string_to_context_struct")
    guarded = native.replace("static int string_to_context_struct(",
                             "static noinline int string_to_context_struct_user_ids(", 1)
    require("string_to_context_struct_user_ids(" in guarded, "Native parser declaration changed")
    guarded = guarded.replace("u32 def_sid)", "u32 def_sid, struct policydb *view)", 1)
    for field, bound in (("user", "p_users"), ("role", "p_roles"), ("type", "p_types")):
        value = {"user": "usrdatum", "role": "role", "type": "typdatum"}[field]
        marker = f"\tctx->{field} = {value}->value;"
        require(guarded.count(marker) == 1, f"Native {field} extraction changed")
        guarded = guarded.replace(marker, marker + f"\n\tif (ctx->{field} > view->{bound}.nprim)\n\t\tgoto out;", 1)
    marker = "\tif (!policydb_context_isvalid(pol, ctx))\n\t\tgoto out;"
    require(guarded.count(marker) == 1, "Native validity check changed")
    guarded = guarded.replace(marker, marker + "\n\tif (!policydb_context_isvalid(view, ctx))\n\t\tgoto out;", 1)
    services = services.replace(native, native + "\n\n#ifdef CONFIG_KSU_SUSFS\n" + guarded + "\n#endif\n", 1)
    body = function(services, "security_parse_user_context")
    marker = "\t/* text is the core's private writable copy; the caller's input is intact. */"
    require(body.count(marker) == 1, "Canonical fallback entry changed")
    body = body.replace(marker, """	if (security_user_ids_compatible(live, snapshot))
		return string_to_context_struct_user_ids(&live->policydb, live->sidtab,
			text, context, SECSID_NULL, &snapshot->policydb);
""" + marker, 1)
    services = replace_function(services, "security_parse_user_context", body)

    # Full loads revalidate; native boolean updates preserve the entire ID schema.
    for name in ("selinux_policy_commit", "security_set_bools"):
        body = function(services, name)
        marker = "\t/* Install the new policy"
        require(body.count(marker) == 1, f"Policy installation changed: {name}")
        preparation = ("security_prepare_user_policy(newpolicy);" if name == "selinux_policy_commit"
                       else "security_prepare_user_policy_reuse(newpolicy, oldpolicy, true);")
        body = body.replace(marker, "#ifdef CONFIG_KSU_SUSFS\n\t" + preparation + "\n#endif\n\n" + marker, 1)
        services = replace_function(services, name, body)
    for name in ("apply_kernelsu_rules", "handle_sepolicy"):
        body = function(rules, name)
        marker = "    rcu_assign_pointer(selinux_state.policy, pol);"
        require(body.count(marker) == 1, f"BakaSU publication changed: {name}")
        preparation = ("security_prepare_user_policy(pol);" if name == "apply_kernelsu_rules"
                       else "security_prepare_user_policy_reuse(pol, old_pol, user_query_schema_unchanged);")
        if name == "handle_sepolicy":
            declaration = "    u32 cmd_index = 0;"
            require(body.count(declaration) == 1, "Policy batch declaration changed")
            body = body.replace(declaration, declaration + "\n#ifdef CONFIG_KSU_SUSFS\n"
                                "    bool user_query_schema_unchanged = true;\n#endif", 1)
            command = "        ret = apply_one_sepolicy_cmd(db, &header, args);"
            require(body.count(command) == 1, "Policy batch mutation boundary changed")
            body = body.replace(command, "#ifdef CONFIG_KSU_SUSFS\n"
                                "        user_query_schema_unchanged &= sepol_cmd_preserves_user_ids(header.cmd);\n"
                                "#endif\n" + command, 1)
        body = body.replace(marker, "#ifdef CONFIG_KSU_SUSFS\n    " + preparation + "\n#endif\n" + marker, 1)
        rules = replace_function(rules, name, body)
    helper = """#ifdef CONFIG_KSU_SUSFS
/* All other commands, including failures and future extensions, require a full proof. */
static bool sepol_cmd_preserves_user_ids(u32 cmd)
{
    return cmd == KSU_SEPOLICY_CMD_NORMAL_PERM || cmd == KSU_SEPOLICY_CMD_XPERM;
}
#endif

"""
    marker = function(rules, "handle_sepolicy")
    rules = rules.replace(marker, helper + marker, 1)
    for path, source in ((services_path, services), (layout_path, layout),
                         (header_path, header), (rules_path, rules)):
        path.write_text(source, encoding="utf-8", newline="\n")


def verify(kernel, report, objdump, function, function_code, object_code, require):
    source = (kernel / "security/selinux/ss/services.c").read_text()
    compiled = (kernel / "out/security/selinux/ss/services.i").read_text()
    layout = (kernel / "security/selinux/ss/services.h").read_text()
    require("user_query_ids_snapshot" in layout and "user_query_ids_compatible" in layout,
            "The ID proof is not bound to each policy")
    selector = function(compiled, "security_user_ids_compatible")
    require("live->user_query_ids_snapshot == snapshot" in selector
            and "live->user_query_ids_compatible" in selector,
            "The fast-path proof can select another policy/snapshot")
    prepare = function(source, "security_prepare_user_policy")
    require(prepare.index("user_query_ids_compatible = false")
            < prepare.index("security_user_policy_ids_match("), "A copied proof remains enabled during validation")
    for path, names, variable in (
        (source, ("selinux_policy_commit", "security_set_bools"), "newpolicy"),
        ((kernel / "drivers/kernelsu/selinux/rules.c").read_text(),
         ("apply_kernelsu_rules", "handle_sepolicy"), "pol"),
    ):
        for name in names:
            body = function(path, name)
            preparation = ("security_prepare_user_policy_reuse(" if name in ("security_set_bools", "handle_sepolicy")
                           else "security_prepare_user_policy(")
            require(body.index(preparation) < body.index("rcu_assign_pointer("),
                    f"{name} publishes an unvalidated or copied ID proof")
    reuse = function(compiled, "security_prepare_user_policy_reuse")
    require("security_prepare_user_policy(policy)" in reuse
            and "previous->latest_granting + 1" in reuse
            and "security_user_ids_compatible(previous, snapshot)" in reuse
            and "rcu_dereference_protected(selinux_state.policy" in function(source, "security_prepare_user_policy_reuse"),
            "Proof inheritance lacks the active policy, generation, snapshot or full-validation fallback")
    rules = (kernel / "drivers/kernelsu/selinux/rules.c").read_text()
    batch = function(rules, "handle_sepolicy")
    require(batch.index("user_query_schema_unchanged &=") < batch.index("ret = apply_one_sepolicy_cmd("),
            "A partially failed schema mutation could inherit the old proof")
    classifier = function(rules, "sepol_cmd_preserves_user_ids")
    require(classifier.count("KSU_SEPOLICY_CMD_") == 2 and "NORMAL_PERM" in classifier and "XPERM" in classifier,
            "Unreviewed schema mutations are eligible for proof reuse")
    compatibility = function(compiled, "security_user_symbols_match")
    require(compatibility.count("symtab_search(") == 2
            and "new->table.htable" in compatibility and "old->table.htable" in compatibility,
            "Alias names are missing from the compatibility proof")
    parser = function(compiled, "string_to_context_struct_user_ids")
    require(parser.index("ctx->type > view->") < parser.index("mls_context_to_sid("),
            "A hidden type reaches MLS parsing")
    require("policydb_context_isvalid(pol, ctx)" in parser
            and "policydb_context_isvalid(view, ctx)" in parser,
            "The parser lost live or snapshot context validity")
    direct = function(compiled, "security_compute_av_user_ids")
    require("security_user_context_valid(db, source)" in direct
            and "security_user_context_valid(db, target)" in direct,
            "The direct decision can index snapshot arrays with live-only IDs")
    require("security_project_context(" not in direct and "context_destroy(" not in direct,
            "The decision fast path still copies or translates contexts")
    require("security_compute_av_user_mapped(" in function(compiled, "security_compute_av_user_projected"),
            "ID/schema mismatch has no canonical fallback")
    require("security_user_translate_decision(" in function(compiled, "security_compute_av_user_mapped"),
            "The fallback assumes unchanged class/permission indices")
    obj = object_code(objdump, kernel / "out/security/selinux/ss/services.o")
    for name in ("security_prepare_user_policy", "security_prepare_user_policy_reuse", "security_user_policy_ids_match",
                 "security_user_ids_compatible", "security_compute_av_user_mapped",
                 "security_compute_av_user_ids", "string_to_context_struct_user_ids"):
        calls = function_code(obj, name)
        (report / (name + ".disasm")).write_text(calls)
    print("Policy-bound ID proof, direct decisions and canonical fallback gates passed")
