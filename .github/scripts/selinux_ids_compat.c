/* Injected inside CONFIG_KSU_SUSFS after the canonical mapper declaration. */
static u32 security_user_symbol_value(unsigned int kind, const void *datum)
{
	if (!datum)
		return 0;
	switch (kind) {
	case SYM_USERS:
		return ((const struct user_datum *)datum)->value;
	case SYM_ROLES:
		return ((const struct role_datum *)datum)->value;
	case SYM_TYPES:
		return ((const struct type_datum *)datum)->value;
	case SYM_LEVELS:
		return ((const struct level_datum *)datum)->level ?
			((const struct level_datum *)datum)->level->sens : 0;
	case SYM_CATS:
		return ((const struct cat_datum *)datum)->value;
	case SYM_CLASSES:
		return ((const struct class_datum *)datum)->value;
	default:
		return 0;
	}
}

/* Check both hash tables, not just the primary-name arrays: aliases matter. */
static bool security_user_symbols_match(struct policydb *snapshot,
					struct policydb *live, unsigned int kind)
{
	struct symtab *old = &snapshot->symtab[kind], *new = &live->symtab[kind];
	struct hashtab_node *node;
	u32 bucket, value;
	void *other;

	if (new->nprim < old->nprim)
		return false;
	for (value = 0; value < old->nprim; value++) {
		const char *old_name = sym_name(snapshot, kind, value);
		const char *new_name = sym_name(live, kind, value);

		/* Some compiled attribute slots have no primary-name entry. */
		if ((old_name || new_name) &&
		    (!old_name || !new_name || strcmp(old_name, new_name)))
			return false;
	}
	for (bucket = 0; bucket < old->table.size; bucket++) {
		for (node = old->table.htable[bucket]; node; node = node->next) {
			value = security_user_symbol_value(kind, node->datum);
			other = symtab_search(new, node->key);
			if (!value || value > old->nprim ||
			    security_user_symbol_value(kind, other) != value)
				return false;
			if (kind == SYM_TYPES &&
			    ((struct type_datum *)node->datum)->attribute !=
			    ((struct type_datum *)other)->attribute)
				return false;
		}
		cond_resched();
	}
	for (bucket = 0; bucket < new->table.size; bucket++) {
		for (node = new->table.htable[bucket]; node; node = node->next) {
			value = security_user_symbol_value(kind, node->datum);
			other = symtab_search(old, node->key);
			if (!value || value > new->nprim)
				return false;
			/* A live-only alias of an existing ID defeats an upper-bound gate. */
			if (value <= old->nprim &&
			    security_user_symbol_value(kind, other) != value)
				return false;
			if (value > old->nprim && other)
				return false;
		}
		cond_resched();
	}
	return true;
}

static bool security_user_permissions_match(struct symtab *old, struct symtab *new)
{
	struct hashtab_node *node;
	u32 bucket;

	if (old->nprim != new->nprim || old->table.nel != new->table.nel)
		return false;
	for (bucket = 0; bucket < old->table.size; bucket++) {
		for (node = old->table.htable[bucket]; node; node = node->next) {
			struct perm_datum *a = node->datum;
			struct perm_datum *b = symtab_search(new, node->key);

			if (!a->value || a->value > 32 || !b || a->value != b->value)
				return false;
		}
	}
	return true;
}

static bool security_user_classes_match(struct policydb *snapshot, struct policydb *live)
{
	u32 index;

	/* A changed class namespace also changes the meaning of numeric /access input. */
	if (snapshot->p_classes.nprim != live->p_classes.nprim ||
	    snapshot->p_classes.table.nel != live->p_classes.table.nel ||
	    !security_user_symbols_match(snapshot, live, SYM_CLASSES))
		return false;
	for (index = 0; index < snapshot->p_classes.nprim; index++) {
		struct class_datum *a = snapshot->class_val_to_struct[index];
		struct class_datum *b = live->class_val_to_struct[index];

		if (!a || !b || !security_user_permissions_match(&a->permissions, &b->permissions))
			return false;
		if (!!a->comdatum != !!b->comdatum || !!a->comkey != !!b->comkey)
			return false;
		if (a->comkey && strcmp(a->comkey, b->comkey))
			return false;
		if (a->comdatum && !security_user_permissions_match(
			&a->comdatum->permissions, &b->comdatum->permissions))
			return false;
	}
	return true;
}

static noinline bool security_user_policy_ids_match(struct policydb *snapshot,
						    struct policydb *live)
{
	static const unsigned int kinds[] = {
		SYM_USERS, SYM_ROLES, SYM_TYPES, SYM_LEVELS, SYM_CATS,
	};
	unsigned int index;

	if (snapshot->mls_enabled != live->mls_enabled)
		return false;
	for (index = 0; index < ARRAY_SIZE(kinds); index++)
		if (!security_user_symbols_match(snapshot, live, kinds[index]))
			return false;
	return security_user_classes_match(snapshot, live);
}

/* Called only on an unpublished policy under policy_mutex, never in RCU readers. */
noinline void security_prepare_user_policy(struct selinux_policy *policy)
{
	struct selinux_policy *snapshot = smp_load_acquire(&backup_sepolicy);

	lockdep_assert_held(&selinux_state.policy_mutex);
	/* kmemdup-based writers may have copied a previous policy's successful proof. */
	policy->user_query_ids_compatible = false;
	policy->user_query_ids_snapshot = snapshot;
	if (snapshot && security_user_policy_ids_match(&snapshot->policydb, &policy->policydb))
		policy->user_query_ids_compatible = true;
	pr_notice("SELinux user-policy prepared: generation=%u ids_compatible=%u snapshot=%p snapshot_generation=%u\n",
		  policy->latest_granting, policy->user_query_ids_compatible, snapshot,
		  snapshot ? snapshot->latest_granting : 0);
}

static noinline bool security_user_ids_compatible(struct selinux_policy *live,
						 struct selinux_policy *snapshot)
{
	/* Both fields belong to the same immutable policy selected by this RCU reader. */
	return live->user_query_ids_compatible && live->user_query_ids_snapshot == snapshot;
}

static bool security_user_context_valid(struct policydb *snapshot, struct context *context)
{
	struct type_datum *type;

	/* policydb_context_isvalid bounds-checks user/role/type and MLS before indexing. */
	if (context->len || !policydb_context_isvalid(snapshot, context))
		return false;
	type = snapshot->type_val_to_struct[context->type - 1];
	return type && !type->attribute;
}

/* Separate symbol permits short, removable trace validation without query counters. */
static noinline void security_compute_av_user_ids(struct selinux_policy *snapshot,
	struct context *source, struct context *target, u16 tclass, struct av_decision *avd)
{
	struct policydb *db = &snapshot->policydb;

	if (!security_user_context_valid(db, source) || !security_user_context_valid(db, target))
		return;
	if (ebitmap_get_bit(&db->permissive_map, source->type))
		avd->flags |= AVD_FLAGS_PERMISSIVE;
	if (unlikely(!tclass)) {
		if (db->allow_unknown)
			avd->allowed = 0xffffffff;
		return;
	}
	context_struct_compute_av(db, source, target, tclass, avd, NULL);
}

static struct perm_datum *security_user_find_permission(struct class_datum *class,
							const char *name)
{
	struct perm_datum *perm = symtab_search(&class->permissions, name);

	if (!perm && class->comdatum)
		perm = symtab_search(&class->comdatum->permissions, name);
	return perm;
}

static void security_user_permission_map(struct symtab *live,
					 struct class_datum *snapshot, s32 map[32])
{
	struct hashtab_node *node;
	u32 bucket;

	for (bucket = 0; bucket < live->table.size; bucket++) {
		for (node = live->table.htable[bucket]; node; node = node->next) {
			struct perm_datum *from = node->datum;
			struct perm_datum *to;
			s32 *slot;

			if (!from->value || from->value > 32)
				continue;
			slot = &map[from->value - 1];
			to = security_user_find_permission(snapshot, node->key);
			if (!to || !to->value || to->value > 32 ||
			    (*slot && *slot != to->value))
				*slot = -1;
			else
				*slot = to->value;
		}
	}
}

static void security_user_translate_decision(struct class_datum *live,
	struct class_datum *snapshot, struct av_decision *source, struct av_decision *target)
{
	s32 map[32] = { 0 };
	u32 index;

	security_user_permission_map(&live->permissions, snapshot, map);
	if (live->comdatum)
		security_user_permission_map(&live->comdatum->permissions, snapshot, map);
	*target = *source;
	target->allowed = 0;
	target->auditallow = 0;
	target->auditdeny = 0xffffffff;
	for (index = 0; index < ARRAY_SIZE(map); index++) {
		u32 live_bit = 1U << index, snapshot_bit;

		if (map[index] <= 0)
			continue;
		snapshot_bit = 1U << (map[index] - 1);
		if (source->allowed & snapshot_bit)
			target->allowed |= live_bit;
		if (source->auditallow & snapshot_bit)
			target->auditallow |= live_bit;
		if (!(source->auditdeny & snapshot_bit))
			target->auditdeny &= ~live_bit;
	}
}

/* Preserve the canonical fallback when symbols or class/permission IDs differ. */
static noinline void security_compute_av_user_mapped(struct selinux_policy *live,
	struct selinux_policy *snapshot, struct context *source, struct context *target,
	u16 tclass, struct av_decision *avd)
{
	struct context scontext, tcontext;
	struct class_datum *live_class, *snapshot_class;
	struct av_decision decision;
	const char *name;

	if (security_project_context(live, snapshot, source, &scontext))
		return;
	if (security_project_context(live, snapshot, target, &tcontext))
		goto out_source;
	if (ebitmap_get_bit(&snapshot->policydb.permissive_map, scontext.type))
		avd->flags |= AVD_FLAGS_PERMISSIVE;
	if (!tclass) {
		if (snapshot->policydb.allow_unknown)
			avd->allowed = 0xffffffff;
		goto out;
	}
	if (tclass > live->policydb.p_classes.nprim)
		goto out;
	live_class = live->policydb.class_val_to_struct[tclass - 1];
	name = sym_name(&live->policydb, SYM_CLASSES, tclass - 1);
	if (!live_class || !name)
		goto out;
	snapshot_class = symtab_search(&snapshot->policydb.p_classes, name);
	if (!snapshot_class)
		goto out;
	decision = *avd;
	context_struct_compute_av(&snapshot->policydb, &scontext, &tcontext,
				  snapshot_class->value, &decision, NULL);
	security_user_translate_decision(live_class, snapshot_class, &decision, avd);
out:
	context_destroy(&tcontext);
out_source:
	context_destroy(&scontext);
}
