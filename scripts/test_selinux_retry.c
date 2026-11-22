/* Execute the injected conversion core; allocation/RCU/SID boundaries are stubs. */
typedef unsigned int u32;
typedef unsigned long size_t;
typedef int gfp_t;
typedef _Bool bool;
#define true 1
#define false 0
#define NULL ((void *)0)
#define CONFIG_KSU_SUSFS 1
#define EINVAL 22
#define ENOMEM 12
#define ESTALE 116
#define SECSID_NULL 0
#define SECINITSID_KERNEL 1
#define SECINITSID_NUM 2
#define rcu_dereference(p) (p)
#define CHECK(p) do { if (!(p)) return __LINE__; } while (0)
struct policydb { int unused; };
struct sidtab { int unused; };
struct selinux_policy { struct policydb policydb; struct sidtab *sidtab; };
struct context { char *str; u32 len; };
static struct selinux_policy live, snapshot;
static struct { struct selinux_policy *policy; } selinux_state = { &live };
static const char *initial_sid_to_string[] = { NULL, "kernel" };
static unsigned allocations, restores, parse_calls, sid_calls, locks, stale_count;
static char scratch[1024], forced[1024];
static int strcmp(const char *a, const char *b) {
    while (*a && *a == *b) { a++; b++; } return (unsigned char)*a - (unsigned char)*b;
}
static size_t strlen(const char *s) { size_t n = 0; while (s[n]) n++; return n; }
void *memcpy(void *to, const void *from, size_t n) {
    unsigned char *p = to; const unsigned char *q = from; restores++;
    while (n--) *p++ = *q++; return to;
}
static char *kmemdup_nul(const char *s, size_t n, gfp_t flags) {
    (void)flags; allocations++; if (n >= sizeof(scratch)) return NULL;
    for (size_t i = 0; i < n; i++) scratch[i] = s[i]; scratch[n] = 0; return scratch;
}
static char *kstrdup(const char *s, gfp_t flags) {
    (void)flags; size_t n = strlen(s); if (n >= sizeof(forced)) return NULL;
    for (size_t i = 0; i <= n; i++) forced[i] = s[i]; return forced;
}
static void kfree(void *p) { (void)p; }
static bool selinux_initialized(void) { return true; }
static void rcu_read_lock(void) { locks++; }
static void rcu_read_unlock(void) { locks--; }
static void context_destroy(struct context *c) { c->str = NULL; }
static struct selinux_policy *security_user_query_policy(void) { return &snapshot; }
static int parse(char *text, struct context *context) {
    parse_calls++; context->str = NULL; context->len = 0;
    if (strcmp(text, "u:r:app:s0")) return -EINVAL;
    text[1] = 0; text[3] = 0; return 0;
}
static int security_parse_user_context(struct selinux_policy *a, struct selinux_policy *b,
                                      char *s, struct context *c) {
    (void)a; (void)b; return parse(s, c);
}
static int string_to_context_struct(struct policydb *p, struct sidtab *t, char *s,
                                    struct context *c, u32 def) {
    (void)p; (void)t; (void)def; return parse(s, c);
}
static int sidtab_context_to_sid(struct sidtab *t, struct context *c, u32 *sid) {
    (void)t; (void)c; sid_calls++;
    if (stale_count) { stale_count--; return -ESTALE; } *sid = 42; return 0;
}
/* ACTUAL_CONTEXT_TO_SID_CORE */
static void reset(void) { allocations = restores = parse_calls = sid_calls = locks = stale_count = 0; }
int test_normal_parse_has_no_restore_copy(void) {
    reset(); u32 sid = 0;
    CHECK(!security_context_to_sid_core("u:r:app:s0", 10, &sid, 0, 0, 0, true));
    CHECK(sid == 42 && allocations == 1 && restores == 0 && parse_calls == 1 && locks == 0);
    return 0;
}
int test_stale_retry_restores_consumed_input(void) {
    reset(); stale_count = 2; u32 sid = 0;
    CHECK(!security_context_to_sid_core("u:r:app:s0", 10, &sid, 0, 0, 0, true));
    CHECK(sid == 42 && allocations == 1 && restores == 2 && parse_calls == 3 && locks == 0);
    return 0;
}
int test_invalid_input_has_no_sid_or_restore(void) {
    reset(); u32 sid = 0;
    CHECK(security_context_to_sid_core("invalid", 7, &sid, 0, 0, 0, true) == -EINVAL);
    CHECK(sid == 0 && parse_calls == 1 && sid_calls == 0 && restores == 0 && locks == 0);
    return 0;
}
int test_empty_input_has_no_allocation(void) {
    reset(); u32 sid = 0;
    CHECK(security_context_to_sid_core("", 0, &sid, 0, 0, 0, true) == -EINVAL);
    CHECK(allocations == 0 && restores == 0 && locks == 0);
    return 0;
}
int test_retry_accepts_nonterminated_input(void) {
    reset(); stale_count = 1; u32 sid = 0;
    const char raw[10] = {'u', ':', 'r', ':', 'a', 'p', 'p', ':', 's', '0'};
    CHECK(!security_context_to_sid_core(raw, sizeof(raw), &sid, 0, 0, 0, true));
    CHECK(sid == 42 && allocations == 1 && restores == 1 && parse_calls == 2 && locks == 0);
    return 0;
}
