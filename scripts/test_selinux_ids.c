/* Host-only fixtures for the real injected C helpers; no device connection. */
typedef unsigned int u32;
typedef unsigned short u16;
typedef int s32;
typedef unsigned long size_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define noinline __attribute__((noinline))
#define unlikely(x) (x)
#define ARRAY_SIZE(x) (sizeof(x) / sizeof((x)[0]))
#define cond_resched() ((void)0)
#define lockdep_assert_held(x) ((void)(x))
#define lockdep_is_held(x) true
#define rcu_dereference_protected(x, condition) (x)
#define smp_load_acquire(x) (*(x))
#define pr_notice(...) ((void)0)
#define EINVAL 22
#define SECSID_NULL 0
#define AVD_FLAGS_PERMISSIVE 1
#define SYM_CLASSES 1
#define SYM_ROLES 2
#define SYM_TYPES 3
#define SYM_USERS 4
#define SYM_LEVELS 6
#define SYM_CATS 7
#define MAX 8
#define CHECK(x) do { if (!(x)) return __LINE__; } while (0)
static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; }
    return (unsigned char)*a - (unsigned char)*b;
}
static void copy(char *to, const char *from) { while ((*to++ = *from++)) {} }
void *memset(void *to, int value, size_t length) {
    unsigned char *p = to; while (length--) *p++ = value; return to;
}
void *memcpy(void *to, const void *from, size_t length) {
    unsigned char *p = to; const unsigned char *q = from;
    while (length--) *p++ = *q++; return to;
}
struct hashtab_node { void *key, *datum; struct hashtab_node *next; };
struct hashtab { struct hashtab_node **htable; u32 size, nel; };
struct symtab { struct hashtab table; u32 nprim; };
struct perm_datum { u32 value; };
struct common_datum { u32 value; struct symtab permissions; };
struct class_datum { u32 value; char *comkey; struct common_datum *comdatum; struct symtab permissions; };
struct user_datum { u32 value; };
struct role_datum { u32 value; };
struct type_datum { u32 value; unsigned char primary, attribute; };
struct level { u32 sens; };
struct level_datum { struct level *level; unsigned char isalias; };
struct cat_datum { u32 value; unsigned char isalias; };
struct ebitmap { u32 bits; };
struct policydb { struct symtab symtab[8]; char *sym_val_to_name[8][MAX];
    struct type_datum *type_val_to_struct[MAX];
    struct class_datum *class_val_to_struct[MAX]; bool mls_enabled, allow_unknown;
    struct ebitmap permissive_map; u32 reject_type; };
#define p_users symtab[SYM_USERS]
#define p_roles symtab[SYM_ROLES]
#define p_types symtab[SYM_TYPES]
#define p_levels symtab[SYM_LEVELS]
#define p_cats symtab[SYM_CATS]
#define p_classes symtab[SYM_CLASSES]
struct selinux_policy { struct policydb policydb; u32 latest_granting;
    bool user_query_ids_compatible; const struct selinux_policy *user_query_ids_snapshot; };
struct context { u32 user, role, type, len; };
struct sidtab { int unused; };
struct av_decision { u32 allowed, auditallow, auditdeny, seqno, flags; };
static struct selinux_policy *backup_sepolicy;
static struct { int policy_mutex; struct selinux_policy *policy; } selinux_state;
static unsigned compute_calls, map_calls, destroy_calls, mls_calls, symbol_lookups;
static char *sym_name(struct policydb *p, unsigned int kind, unsigned int value) {
    return p->sym_val_to_name[kind][value];
}
static void *symtab_search(struct symtab *table, const char *key) {
    symbol_lookups++;
    if (!table->table.size) return NULL;
    for (struct hashtab_node *n = table->table.htable[0]; n; n = n->next)
        if (!strcmp(n->key, key)) return n->datum;
    return NULL;
}
static void context_init(struct context *c) { memset(c, 0, sizeof(*c)); }
static void context_destroy(struct context *c) { (void)c; destroy_calls++; }
static int mls_context_to_sid(struct policydb *p, char oldc, char *text,
    struct context *c, struct sidtab *s, u32 def_sid) {
    (void)p; (void)oldc; (void)text; (void)c; (void)s; (void)def_sid; mls_calls++; return 0;
}
static bool policydb_context_isvalid(struct policydb *p, struct context *c) {
    return c->user && c->role && c->type && c->user <= p->p_users.nprim &&
        c->role <= p->p_roles.nprim && c->type <= p->p_types.nprim && c->type != p->reject_type;
}
static bool ebitmap_get_bit(struct ebitmap *e, u32 index) { return index < 32 && (e->bits & (1U << index)); }
static void context_struct_compute_av(struct policydb *p, struct context *s,
    struct context *t, u16 class, struct av_decision *out, void *xperms) {
    (void)p; (void)s; (void)t; (void)xperms;
    compute_calls++; out->allowed = class == 1 ? 1 : 0;
    out->auditallow = out->allowed; out->auditdeny = ~out->allowed;
}
static int security_project_context(struct selinux_policy *from, struct selinux_policy *to,
    struct context *source, struct context *target) {
    (void)from; map_calls++; *target = *source;
    return policydb_context_isvalid(&to->policydb, target) ? 0 : -EINVAL;
}

/* ACTUAL_COMPAT_HELPERS */
/* ACTUAL_NATIVE_AND_GUARDED_PARSERS */
/* ACTUAL_BATCH_CLASSIFIER */

union datum { struct user_datum user; struct role_datum role; struct type_datum type;
    struct level_datum level; struct cat_datum cat; };
struct fixture { struct selinux_policy policy; union datum values[8][MAX];
    struct level levels[MAX]; struct class_datum classes[MAX];
    struct hashtab_node nodes[8][MAX], *heads[8]; unsigned used[8];
    struct perm_datum perms[MAX]; struct hashtab_node perm_nodes[MAX], *perm_head; };
static void add_symbol(struct fixture *f, unsigned kind, char *name, u32 value, bool alias) {
    unsigned slot = f->used[kind]++; union datum *d = &f->values[kind][slot];
    struct hashtab_node *node = &f->nodes[kind][slot];
    if (kind == SYM_LEVELS) { f->levels[slot].sens = value; d->level.level = &f->levels[slot]; }
    else d->user.value = value;
    node->key = name; node->datum = kind == SYM_CLASSES ? (void *)&f->classes[value-1] : (void *)d;
    node->next = f->heads[kind]; f->heads[kind] = node;
    struct symtab *table = &f->policy.policydb.symtab[kind];
    table->table.htable = &f->heads[kind]; table->table.size = 1; table->table.nel++;
    if (!alias) { table->nprim = value; f->policy.policydb.sym_val_to_name[kind][value-1] = name; }
    if (kind == SYM_TYPES && !alias) f->policy.policydb.type_val_to_struct[value-1] = &d->type;
    if (kind == SYM_CLASSES) {
        f->classes[value-1].value = value;
        f->policy.policydb.class_val_to_struct[value-1] = &f->classes[value-1];
    }
}
static void add_perm(struct fixture *f, unsigned slot, char *name, u32 value) {
    f->perms[slot].value = value;
    f->perm_nodes[slot].key = name; f->perm_nodes[slot].datum = &f->perms[slot];
    f->perm_nodes[slot].next = f->perm_head; f->perm_head = &f->perm_nodes[slot];
    f->classes[0].permissions.table.htable = &f->perm_head;
    f->classes[0].permissions.table.size = 1; f->classes[0].permissions.table.nel++;
    if (value > f->classes[0].permissions.nprim) f->classes[0].permissions.nprim = value;
}
static void init(struct fixture *f) {
    memset(f, 0, sizeof(*f)); f->policy.policydb.mls_enabled = true; f->policy.latest_granting = 9;
    add_symbol(f, SYM_USERS, "u", 1, false); add_symbol(f, SYM_ROLES, "r", 1, false);
    add_symbol(f, SYM_TYPES, "app", 1, false); add_symbol(f, SYM_LEVELS, "s0", 1, false);
    add_symbol(f, SYM_CATS, "c0", 1, false); add_symbol(f, SYM_CLASSES, "file", 1, false);
    add_perm(f, 0, "read", 1); add_perm(f, 1, "write", 2);
}
static struct fixture a, b, c;
static struct av_decision decision(void) {
    struct av_decision d = {0,0,0xffffffff,1,0}; return d;
}

int test_append_only(void) {
    init(&a); init(&b); backup_sepolicy = &a.policy;
    CHECK(!security_user_ids_compatible(&b.policy, &a.policy));
    security_prepare_user_policy(&b.policy); CHECK(security_user_ids_compatible(&b.policy, &a.policy));
    add_symbol(&b, SYM_TYPES, "ksu", 2, false);
    security_prepare_user_policy(&b.policy); CHECK(security_user_ids_compatible(&b.policy, &a.policy));
    init(&c); CHECK(!security_user_ids_compatible(&b.policy, &c.policy));
    return 0;
}
int test_alias_and_copied_proof(void) {
    init(&a); init(&b); backup_sepolicy = &a.policy; security_prepare_user_policy(&b.policy);
    CHECK(b.policy.user_query_ids_compatible);
    add_symbol(&b, SYM_TYPES, "alias_app", 1, true);
    struct selinux_policy copied = b.policy;
    CHECK(copied.user_query_ids_compatible); security_prepare_user_policy(&copied);
    CHECK(!copied.user_query_ids_compatible);
    init(&a); init(&b); add_symbol(&a, SYM_TYPES, "alias_app", 1, true);
    CHECK(!security_user_policy_ids_match(&a.policy.policydb, &b.policy.policydb));
    add_symbol(&b, SYM_TYPES, "alias_app", 1, true);
    CHECK(security_user_policy_ids_match(&a.policy.policydb, &b.policy.policydb));
    return 0;
}
int test_unnamed_attribute_slots(void) {
    init(&a); init(&b);
    add_symbol(&a,SYM_TYPES,"attribute",2,false); add_symbol(&b,SYM_TYPES,"attribute",2,false);
    a.values[SYM_TYPES][1].type.attribute = b.values[SYM_TYPES][1].type.attribute = 1;
    a.policy.policydb.sym_val_to_name[SYM_TYPES][1] = b.policy.policydb.sym_val_to_name[SYM_TYPES][1] = NULL;
    CHECK(security_user_policy_ids_match(&a.policy.policydb,&b.policy.policydb));
    struct context attribute = {1,1,2,0}; struct av_decision d = decision();
    compute_calls = 0;
    security_compute_av_user_ids(&a.policy,&attribute,&attribute,1,&d);
    CHECK(!compute_calls && d.allowed == 0);
    return 0;
}
int test_id_and_schema_changes(void) {
    const unsigned kinds[] = {SYM_USERS,SYM_ROLES,SYM_TYPES,SYM_LEVELS,SYM_CATS};
    for (unsigned i = 0; i < ARRAY_SIZE(kinds); i++) {
        init(&a); init(&b); unsigned kind = kinds[i];
        if (kind == SYM_LEVELS) b.levels[0].sens = 2; else b.values[kind][0].user.value = 2;
        CHECK(!security_user_policy_ids_match(&a.policy.policydb, &b.policy.policydb));
    }
    init(&a); init(&b); b.policy.policydb.mls_enabled = false;
    CHECK(!security_user_policy_ids_match(&a.policy.policydb, &b.policy.policydb));
    init(&a); init(&b); b.perms[0].value = 2; b.perms[1].value = 1;
    CHECK(!security_user_policy_ids_match(&a.policy.policydb, &b.policy.policydb));
    init(&a); init(&b); add_perm(&b, 2, "ioctl", 3);
    CHECK(!security_user_policy_ids_match(&a.policy.policydb, &b.policy.policydb));
    init(&a); init(&b); b.values[SYM_TYPES][0].type.attribute = 1;
    CHECK(!security_user_policy_ids_match(&a.policy.policydb, &b.policy.policydb));
    return 0;
}
int test_common_permission_schema_and_fallback(void) {
    init(&a); init(&b);
    struct common_datum old_common = {1,a.classes[0].permissions};
    struct common_datum new_common = {1,b.classes[0].permissions};
    memset(&a.classes[0].permissions,0,sizeof(a.classes[0].permissions));
    memset(&b.classes[0].permissions,0,sizeof(b.classes[0].permissions));
    a.classes[0].comkey = b.classes[0].comkey = "common_file";
    a.classes[0].comdatum = &old_common; b.classes[0].comdatum = &new_common;
    CHECK(security_user_policy_ids_match(&a.policy.policydb,&b.policy.policydb));
    b.perms[0].value = 2; b.perms[1].value = 1;
    CHECK(!security_user_policy_ids_match(&a.policy.policydb,&b.policy.policydb));
    struct context context = {1,1,1,0}; struct av_decision d = decision();
    security_compute_av_user_mapped(&b.policy,&a.policy,&context,&context,1,&d);
    CHECK(d.allowed == 2 && d.auditdeny == ~2U);
    return 0;
}
int test_invalid_prefixes_resolve_the_same_fixed_symbols(void) {
    init(&a); init(&b);
    char text[96]; struct context context; struct sidtab sidtab;
    const char *invalid[] = {"u:r:missing:s0", "\n:r:missing:s0", "missing:r:app:s0", "u:missing:app:s0"};
    for (unsigned i = 0; i < sizeof(invalid)/sizeof(invalid[0]); i++) {
        copy(text, invalid[i]); symbol_lookups = 0; mls_calls = 0;
        CHECK(string_to_context_struct_user_ids(&b.policy.policydb,&sidtab,text,&context,0,&a.policy.policydb) == -EINVAL);
        CHECK(symbol_lookups == 3 && mls_calls == 0);
    }
    return 0;
}

int test_guarded_parser_boundaries(void) {
    init(&a); init(&b); add_symbol(&b, SYM_TYPES, "ksu", 2, false);
    char text[64]; struct context context; struct sidtab sidtab;
    copy(text, "u:r:app:s0"); mls_calls = 0;
    CHECK(!string_to_context_struct_user_ids(&b.policy.policydb, &sidtab, text, &context, 0, &a.policy.policydb));
    CHECK(mls_calls == 1 && context.type == 1);
    copy(text, "u:r:ksu:s0:c0,c0,c0"); mls_calls = 0;
    CHECK(string_to_context_struct_user_ids(&b.policy.policydb, &sidtab, text, &context, 0, &a.policy.policydb) == -EINVAL);
    CHECK(mls_calls == 0);
    copy(text, "u:r:ksu:s0:c0,c0,c0");
    CHECK(!string_to_context_struct(&b.policy.policydb, &sidtab, text, &context, 0)); CHECK(mls_calls == 1);
    add_symbol(&b,SYM_USERS,"extra_user",2,false); copy(text,"extra_user:r:app:s0"); mls_calls = 0;
    CHECK(string_to_context_struct_user_ids(&b.policy.policydb,&sidtab,text,&context,0,&a.policy.policydb) == -EINVAL);
    CHECK(!mls_calls);
    add_symbol(&b,SYM_ROLES,"extra_role",2,false); copy(text,"u:extra_role:app:s0");
    CHECK(string_to_context_struct_user_ids(&b.policy.policydb,&sidtab,text,&context,0,&a.policy.policydb) == -EINVAL);
    CHECK(!mls_calls);
    a.policy.policydb.reject_type = 1; copy(text, "u:r:app:s0");
    CHECK(string_to_context_struct_user_ids(&b.policy.policydb, &sidtab, text, &context, 0, &a.policy.policydb) == -EINVAL);
    a.policy.policydb.reject_type = 0; b.policy.policydb.reject_type = 1; copy(text, "u:r:app:s0");
    CHECK(string_to_context_struct_user_ids(&b.policy.policydb, &sidtab, text, &context, 0, &a.policy.policydb) == -EINVAL);
    return 0;
}
int test_direct_bounds_and_class0(void) {
    init(&a); struct context valid = {1,1,1,0}, added = {1,1,2,0};
    struct av_decision d = decision(); compute_calls = map_calls = 0;
    security_compute_av_user_ids(&a.policy, &valid, &valid, 1, &d);
    CHECK(compute_calls == 1 && map_calls == 0 && d.allowed == 1 && d.seqno == 1);
    d = decision(); security_compute_av_user_ids(&a.policy, &valid, &added, 1, &d);
    CHECK(compute_calls == 1 && d.allowed == 0);
    d = decision(); security_compute_av_user_ids(&a.policy, &valid, &valid, 0, &d);
    CHECK(compute_calls == 1 && d.allowed == 0 && d.auditdeny == 0xffffffff && d.seqno == 1);
    return 0;
}
int test_fallback_permission_reordering(void) {
    init(&a); init(&b); b.perms[0].value = 2; b.perms[1].value = 1;
    struct context context = {1,1,1,0}; struct av_decision d = decision(); map_calls = 0;
    security_compute_av_user_mapped(&b.policy, &a.policy, &context, &context, 1, &d);
    CHECK(map_calls == 2 && d.allowed == 2 && d.auditallow == 2 && d.auditdeny == ~2U && d.seqno == 1);
    add_perm(&b, 2, "unknown_alias_of_read", 2); d = decision();
    security_compute_av_user_mapped(&b.policy, &a.policy, &context, &context, 1, &d);
    CHECK(d.allowed == 0 && d.auditdeny == 0xffffffff);
    return 0;
}
int test_fallback_class_reordering(void) {
    init(&a); init(&b);
    /* Keep file's datum/permissions, but publish it at live class ID 2. */
    b.classes[1] = b.classes[0]; b.classes[1].value = 2;
    b.policy.policydb.class_val_to_struct[1] = &b.classes[1];
    b.policy.policydb.sym_val_to_name[SYM_CLASSES][0] = "new_class";
    b.policy.policydb.sym_val_to_name[SYM_CLASSES][1] = "file";
    b.policy.policydb.p_classes.nprim = 2;
    struct context context = {1,1,1,0}; struct av_decision d = decision();
    security_compute_av_user_mapped(&b.policy, &a.policy, &context, &context, 2, &d);
    CHECK(d.allowed == 1 && d.seqno == 1);
    d = decision();
    security_compute_av_user_mapped(&b.policy, &a.policy, &context, &context, 1, &d);
    CHECK(d.allowed == 0);
    return 0;
}

int test_reuse_avoids_symbol_rescan(void) {
    init(&a); init(&b); backup_sepolicy = &a.policy;
    security_prepare_user_policy(&b.policy); selinux_state.policy = &b.policy;
    c.policy = b.policy; c.policy.latest_granting++;
    unsigned before = symbol_lookups;
    security_prepare_user_policy_reuse(&c.policy, &b.policy, true);
    CHECK(symbol_lookups == before);
    CHECK(security_user_ids_compatible(&c.policy, &a.policy));
    return 0;
}
int test_dirty_alias_requires_full_proof(void) {
    init(&a); init(&b); init(&c); backup_sepolicy = &a.policy;
    security_prepare_user_policy(&b.policy); selinux_state.policy = &b.policy;
    c.policy.latest_granting = b.policy.latest_granting + 1;
    c.policy.user_query_ids_compatible = true; c.policy.user_query_ids_snapshot = &a.policy;
    add_symbol(&c, SYM_TYPES, "new_alias", 1, true);
    unsigned before = symbol_lookups;
    security_prepare_user_policy_reuse(&c.policy, &b.policy, false);
    CHECK(symbol_lookups > before && !c.policy.user_query_ids_compatible);
    return 0;
}
int test_reuse_rejects_wrong_active_policy(void) {
    init(&a); init(&b); init(&c); backup_sepolicy = &a.policy;
    security_prepare_user_policy(&b.policy); selinux_state.policy = &a.policy;
    c.policy.latest_granting = b.policy.latest_granting + 1;
    add_symbol(&c, SYM_TYPES, "new_alias", 1, true);
    security_prepare_user_policy_reuse(&c.policy, &b.policy, true);
    CHECK(!c.policy.user_query_ids_compatible);
    return 0;
}
int test_reuse_rejects_generation_and_missing_proof(void) {
    init(&a); init(&b); init(&c); backup_sepolicy = &a.policy;
    security_prepare_user_policy(&b.policy); selinux_state.policy = &b.policy;
    add_symbol(&c, SYM_TYPES, "new_alias", 1, true);
    security_prepare_user_policy_reuse(&c.policy, &b.policy, true);
    CHECK(!c.policy.user_query_ids_compatible);
    c.policy.latest_granting++;
    b.policy.user_query_ids_compatible = false;
    security_prepare_user_policy_reuse(&c.policy, &b.policy, true);
    CHECK(!c.policy.user_query_ids_compatible);
    return 0;
}
int test_reuse_rejects_changed_snapshot_and_wrap(void) {
    init(&a); init(&b); init(&c); backup_sepolicy = &a.policy;
    security_prepare_user_policy(&b.policy); selinux_state.policy = &b.policy;
    c.policy.latest_granting++;
    add_symbol(&c, SYM_TYPES, "new_alias", 1, true);
    b.policy.user_query_ids_snapshot = &c.policy;
    security_prepare_user_policy_reuse(&c.policy, &b.policy, true);
    CHECK(!c.policy.user_query_ids_compatible);
    b.policy.user_query_ids_snapshot = &a.policy;
    b.policy.latest_granting = ~0U; c.policy.latest_granting = 0;
    security_prepare_user_policy_reuse(&c.policy, &b.policy, true);
    CHECK(!c.policy.user_query_ids_compatible);
    return 0;
}
int test_only_audited_batch_commands_reuse(void) {
    CHECK(sepol_cmd_preserves_user_ids(KSU_SEPOLICY_CMD_NORMAL_PERM));
    CHECK(sepol_cmd_preserves_user_ids(KSU_SEPOLICY_CMD_XPERM));
    CHECK(!sepol_cmd_preserves_user_ids(KSU_SEPOLICY_CMD_TYPE));
    CHECK(!sepol_cmd_preserves_user_ids(KSU_SEPOLICY_CMD_TYPE_ATTR));
    CHECK(!sepol_cmd_preserves_user_ids(0xffffffff));
    return 0;
}
