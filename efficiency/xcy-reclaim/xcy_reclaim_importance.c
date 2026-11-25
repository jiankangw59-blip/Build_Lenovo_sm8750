#include "xcy_reclaim_importance.h"

#ifdef __KERNEL__
#include <linux/bsearch.h>
#include <linux/sort.h>
#endif

/* Verified directly in the unmodified ROM framework.jar, SHA256
 * 38c6f92565bf8d339d3d95be5c8ccee07665635c4148ac60478fa48859c4701f:
 * classes.dex, android.app.ActivityManager$RunningAppProcessInfo static fields:
 * FLAG_CANT_SAVE_STATE=1, FLAG_PERSISTENT=2, FLAG_HAS_ACTIVITIES=4;
 * REASON_UNKNOWN=0, REASON_PROVIDER_IN_USE=1, REASON_SERVICE_IN_USE=2.
 * Local P0..P4 tiers are the requested observation policy, not native values. */
#define POLICY_CANT_SAVE 1U
#define POLICY_PERSISTENT 2U
#define POLICY_KNOWN_FLAGS 7U
#define POLICY_REASON_NONE 0
#define POLICY_REASON_PROVIDER 1
#define POLICY_REASON_SERVICE 2

#define POLICY_UNCERTAIN (XCY_POLICY_REASON_INPUT_INVALID | XCY_POLICY_REASON_IDENTITY_INVALID | \
    XCY_POLICY_REASON_OWNER_UNKNOWN | XCY_POLICY_REASON_UNKNOWN_IMPORTANCE | \
    XCY_POLICY_REASON_UNKNOWN_STATE | XCY_POLICY_REASON_GONE | XCY_POLICY_REASON_UNKNOWN_FLAGS | \
    XCY_POLICY_REASON_DEPENDENCY_MISSING | XCY_POLICY_REASON_DEPENDENCY_INVALID | \
    XCY_POLICY_REASON_DEPENDENCY_CYCLE | XCY_POLICY_REASON_DEPENDENCY_UNKNOWN | \
    XCY_POLICY_REASON_DEPENDENCY_FIELDS | XCY_POLICY_REASON_PACKAGE_EMPTY | \
    XCY_POLICY_REASON_RESOURCE_FAILURE | XCY_POLICY_REASON_UID_OWNER_CONFLICT)
#define POLICY_OWNER_VETO (XCY_POLICY_REASON_NOT_THIRD_PARTY | XCY_POLICY_REASON_SHARED_UID)

typedef struct {
    uint32_t key;
    uint32_t index;
} policy_key_t;

typedef struct {
    const xcy_memory_snapshot_t *snapshot;
    xcy_policy_result_t *results;
    int32_t *edges;
    size_t *path;
    unsigned char *colors;
    policy_key_t *keys;
} policy_context_t;

static xcy_policy_result_t policy_unknown(uint64_t reason) {
    xcy_policy_result_t result = {XCY_POLICY_UNKNOWN, 0, reason};
    return result;
}

static int policy_live(const xcy_memory_process_t *p) {
    return p->pid > 1 && p->starttime > 0 &&
        (p->flags & XCY_MEMORY_IDENTITY_VALID) != 0 && (p->flags & ~3U) == 0 &&
        xcy_memory_uid_mapping_valid(p);
}

static void policy_finish(xcy_policy_result_t *result) {
    if ((result->reasons & POLICY_UNCERTAIN) && result->tier != XCY_POLICY_P0_PROTECTED)
        result->tier = XCY_POLICY_UNKNOWN;
    result->eligible = (result->tier == XCY_POLICY_P3_CACHED || result->tier == XCY_POLICY_P4_EMPTY) &&
        !(result->reasons & (POLICY_UNCERTAIN | POLICY_OWNER_VETO | XCY_POLICY_REASON_DEPENDENCY_IMPORTANT));
}

static int policy_known_importance(int32_t importance) {
    switch (importance) {
        case 100: case 125: case 130: case 200: case 230:
        case 300: case 325: case 350: case 400: case 1000: return 1;
        default: return 0; /* Preserve unsupported/legacy/OEM values, but hold. */
    }
}

static int policy_hard_importance(int32_t importance) {
    switch (importance) {
        case 100: case 125: case 130: case 200: case 230: case 325: case 350: return 1;
        default: return 0;
    }
}

/* ROM procStateToImportance(I)I, classes.dex code_item 0x1c430c. This check
 * reports disagreement only; known stronger protection remains authoritative. */
static int32_t policy_expected_importance(int32_t state) {
    if (state < 0 || state > 20) return -1;
    if (state == 20) return 1000;
    if (state >= 14) return 400;
    if (state == 13) return 350;
    if (state == 12) return 325;
    if (state >= 10) return 300;
    if (state >= 8) return 230;
    if (state >= 6) return 200;
    if (state >= 4) return 125;
    return 100;
}

static xcy_policy_result_t policy_base(const xcy_memory_process_t *p) {
    xcy_policy_result_t result = policy_unknown(0);
    int importance_known = policy_known_importance(p->importance);
    int state_known = p->procstate >= 0 && p->procstate <= 20;
    xcy_memory_token_t package = {p->pkg, strlen(p->pkg)};
    if (!policy_live(p)) result.reasons |= XCY_POLICY_REASON_IDENTITY_INVALID;
    if (p->classification == XCY_MEMORY_CLASS_SHARED_UID)
        result.reasons |= XCY_POLICY_REASON_SHARED_UID;
    else if (p->classification != XCY_MEMORY_CLASS_THIRD_PARTY)
        result.reasons |= p->classification == XCY_MEMORY_CLASS_UNKNOWN ?
            XCY_POLICY_REASON_OWNER_UNKNOWN : XCY_POLICY_REASON_NOT_THIRD_PARTY;
    if (p->classification == XCY_MEMORY_CLASS_THIRD_PARTY &&
        (!xcy_memory_package(package) || strcmp(p->pkg_list, p->pkg) != 0 ||
         p->uid % 100000U < 10000U || p->uid % 100000U >= 99000U))
        result.reasons |= XCY_POLICY_REASON_OWNER_UNKNOWN;
    if (!importance_known) result.reasons |= XCY_POLICY_REASON_UNKNOWN_IMPORTANCE;
    if (!state_known) result.reasons |= XCY_POLICY_REASON_UNKNOWN_STATE;
    if (p->importance == 1000 || p->procstate == 20) result.reasons |= XCY_POLICY_REASON_GONE;
    if (p->framework_flags & ~POLICY_KNOWN_FLAGS) result.reasons |= XCY_POLICY_REASON_UNKNOWN_FLAGS;
    if (importance_known && state_known && policy_expected_importance(p->procstate) != p->importance)
        result.reasons |= XCY_POLICY_REASON_FIELDS_DISAGREE;
    if (p->framework_flags & POLICY_CANT_SAVE) result.reasons |= XCY_POLICY_REASON_CANT_SAVE_FLAG;
    if (p->framework_flags & POLICY_PERSISTENT) result.reasons |= XCY_POLICY_REASON_PERSISTENT_FLAG;
    if (policy_hard_importance(p->importance)) result.reasons |= XCY_POLICY_REASON_IMPORTANCE_PROTECT;
    if ((p->procstate >= 0 && p->procstate <= 6) || p->procstate == 12 ||
        p->procstate == 13 || p->procstate == 14) result.reasons |= XCY_POLICY_REASON_STATE_PROTECT;
    if (result.reasons & (XCY_POLICY_REASON_CANT_SAVE_FLAG | XCY_POLICY_REASON_PERSISTENT_FLAG |
                         XCY_POLICY_REASON_IMPORTANCE_PROTECT | XCY_POLICY_REASON_STATE_PROTECT))
        result.tier = XCY_POLICY_P0_PROTECTED;
    else if (p->importance == 300 || p->procstate == 7 || p->procstate == 9 || p->procstate == 10) {
        result.tier = XCY_POLICY_P1_IMPORTANT;
        result.reasons |= XCY_POLICY_REASON_IMPORTANT_BUSINESS;
    } else if (p->procstate == 8 || p->procstate == 11) {
        result.tier = XCY_POLICY_P2_TRANSIENT;
        result.reasons |= XCY_POLICY_REASON_TRANSIENT;
    } else if (p->importance == 400 && p->procstate >= 15 && p->procstate <= 18) {
        result.tier = XCY_POLICY_P3_CACHED;
        result.reasons |= XCY_POLICY_REASON_CACHED;
    } else if (p->importance == 400 && p->procstate == 19) {
        result.tier = XCY_POLICY_P4_EMPTY;
        result.reasons |= XCY_POLICY_REASON_EMPTY;
    }
    policy_finish(&result);
    return result;
}

static void policy_context_free(policy_context_t *context) {
    xcy_snapshot_free(context->results);
    xcy_snapshot_free(context->edges);
    xcy_snapshot_free(context->path);
    xcy_snapshot_free(context->colors);
    xcy_snapshot_free(context->keys);
    memset(context, 0, sizeof(*context));
}

static int policy_compare_keys(const void *left, const void *right) {
    uint32_t first = ((const policy_key_t *)left)->key;
    uint32_t second = ((const policy_key_t *)right)->key;

    return (first > second) - (first < second);
}

static void policy_sort_keys(policy_key_t *keys, size_t count) {
#ifdef __KERNEL__
    sort(keys, count, sizeof(*keys), policy_compare_keys, NULL);
#else
    qsort(keys, count, sizeof(*keys), policy_compare_keys);
#endif
}

static int policy_uid_owner(const xcy_memory_process_t *process) {
    return policy_live(process) && process->classification == XCY_MEMORY_CLASS_THIRD_PARTY &&
        strcmp(process->pkg_list, process->pkg) == 0;
}

static void policy_check_ownership(policy_context_t *context, const xcy_memory_snapshot_t *snapshot) {
    const policy_key_t *keys = context->keys;
    size_t first = 0;

    while (first < snapshot->count) {
        const char *package = snapshot->items[keys[first].index].pkg;
        size_t end = first;
        size_t untrusted = 0;
        int mixed_packages = 0;
        size_t i;

        do {
            const xcy_memory_process_t *process = &snapshot->items[keys[end].index];

            mixed_packages |= strcmp(package, process->pkg) != 0;
            untrusted += !policy_uid_owner(process);
            ++end;
        } while (end < snapshot->count && keys[end].key == keys[first].key);

        for (i = first; i < end; ++i) {
            uint32_t index = keys[i].index;
            const xcy_memory_process_t *process = &snapshot->items[index];

            if (process->classification == XCY_MEMORY_CLASS_THIRD_PARTY &&
                (mixed_packages || untrusted > (size_t)!policy_uid_owner(process)))
                context->results[index].reasons |= XCY_POLICY_REASON_UID_OWNER_CONFLICT;
        }
        first = end;
    }
}

static int policy_context_build(policy_context_t *context, const xcy_memory_snapshot_t *snapshot) {
    size_t i;
    memset(context, 0, sizeof(*context));
    context->snapshot = snapshot;
    context->results = xcy_snapshot_calloc(snapshot->count, sizeof(*context->results));
    context->edges = xcy_snapshot_calloc(snapshot->count, sizeof(*context->edges));
    context->path = xcy_snapshot_calloc(snapshot->count, sizeof(*context->path));
    context->colors = xcy_snapshot_calloc(snapshot->count, sizeof(*context->colors));
    context->keys = xcy_snapshot_calloc(snapshot->count, sizeof(*context->keys));
    if (!context->results || !context->edges || !context->path || !context->colors || !context->keys) return 0;
    for (i = 0; i < snapshot->count; ++i) {
        context->results[i] = policy_base(&snapshot->items[i]);
        context->keys[i] = (policy_key_t){snapshot->items[i].uid, (uint32_t)i};
    }
    policy_sort_keys(context->keys, snapshot->count);
    policy_check_ownership(context, snapshot);

    for (i = 0; i < snapshot->count; ++i)
        context->keys[i] = (policy_key_t){snapshot->items[i].pid, (uint32_t)i};
    policy_sort_keys(context->keys, snapshot->count);
    for (i = 1; i < snapshot->count; ++i)
        if (context->keys[i - 1].key == context->keys[i].key) return -1;

    for (i = 0; i < snapshot->count; ++i) {
        const xcy_memory_process_t *p = &snapshot->items[i];
        xcy_policy_result_t *result = &context->results[i];
        context->edges[i] = -1;
        if (p->reason_code == POLICY_REASON_NONE) {
            if (p->reason_pid != 0 || strcmp(p->reason_component, "-") != 0)
                result->reasons |= XCY_POLICY_REASON_DEPENDENCY_FIELDS;
        } else if (p->reason_code == POLICY_REASON_PROVIDER || p->reason_code == POLICY_REASON_SERVICE) {
            result->reasons |= XCY_POLICY_REASON_DEPENDENCY_PRESENT;
            if (strcmp(p->reason_component, "-") == 0)
                result->reasons |= XCY_POLICY_REASON_DEPENDENCY_FIELDS;
            if (p->reason_pid <= 1) result->reasons |= XCY_POLICY_REASON_DEPENDENCY_MISSING;
            else {
                policy_key_t key = {.key = (uint32_t)p->reason_pid};
                const policy_key_t *found = bsearch(&key, context->keys, snapshot->count,
                                                   sizeof(key), policy_compare_keys);
                if (!found) result->reasons |= XCY_POLICY_REASON_DEPENDENCY_MISSING;
                else if (found->index == i) result->reasons |= XCY_POLICY_REASON_DEPENDENCY_CYCLE;
                else if (!policy_live(&snapshot->items[found->index])) result->reasons |= XCY_POLICY_REASON_DEPENDENCY_INVALID;
                else context->edges[i] = (int32_t)found->index;
            }
        } else result->reasons |= XCY_POLICY_REASON_DEPENDENCY_UNKNOWN;
        policy_finish(result);
    }
    return 1;
}

/* Single-client references form a bounded functional graph. Iterative path
 * coloring avoids recursion even for a full 4096-process dependency chain. */
static void policy_resolve(policy_context_t *context, size_t index) {
    size_t length = 0, i;
    int32_t cursor = (int32_t)index;
    while (cursor >= 0 && context->colors[cursor] == 0) {
        context->colors[cursor] = 1;
        context->path[length++] = (size_t)cursor;
        cursor = context->edges[cursor];
    }
    if (cursor >= 0 && context->colors[cursor] == 1) {
        for (i = 0; i < length && context->path[i] != (size_t)cursor; ++i) { }
        for (; i < length; ++i) {
            context->results[context->path[i]].reasons |= XCY_POLICY_REASON_DEPENDENCY_CYCLE;
            policy_finish(&context->results[context->path[i]]);
        }
    }
    while (length > 0) {
        size_t node = context->path[--length];
        int32_t next = context->edges[node];
        xcy_policy_result_t *result = &context->results[node];
        if (next >= 0) {
            const xcy_policy_result_t *client = &context->results[next];
            if (client->reasons & XCY_POLICY_REASON_DEPENDENCY_CYCLE) {
                /* An unresolved cycle cannot propagate a traversal-order-
                 * dependent tier. Preserve this node's own P0 evidence only. */
                result->reasons |= XCY_POLICY_REASON_DEPENDENCY_CYCLE | XCY_POLICY_REASON_DEPENDENCY_UNKNOWN;
            } else if (client->tier <= XCY_POLICY_P2_TRANSIENT) {
                result->reasons |= XCY_POLICY_REASON_DEPENDENCY_IMPORTANT;
                if (client->tier < result->tier) result->tier = client->tier;
            }
            if (client->reasons & POLICY_UNCERTAIN ||
                (client->reasons & POLICY_OWNER_VETO && client->tier >= XCY_POLICY_P3_CACHED) ||
                client->tier == XCY_POLICY_UNKNOWN)
                result->reasons |= XCY_POLICY_REASON_DEPENDENCY_UNKNOWN;
            else if (client->eligible) result->reasons |= XCY_POLICY_REASON_DEPENDENCY_SAFE;
        }
        policy_finish(result);
        context->colors[node] = 2;
    }
}

static int policy_snapshot_valid(const xcy_memory_snapshot_t *snapshot) {
    size_t i;
    if (!snapshot || !snapshot->items || !snapshot->count || snapshot->count > XCY_MEMORY_MAX_PROCESSES)
        return 0;
    for (i = 0; i < snapshot->count; ++i) {
        const xcy_memory_process_t *p = &snapshot->items[i];
        if (!memchr(p->pkg, 0, sizeof(p->pkg)) || !memchr(p->pkg_list, 0, sizeof(p->pkg_list)) ||
            !memchr(p->reason_component, 0, sizeof(p->reason_component))) return 0;
    }
    return 1;
}

xcy_policy_result_t xcy_policy_process(const xcy_memory_snapshot_t *snapshot, size_t index) {
    policy_context_t context;
    xcy_policy_result_t result;
    int status;
    if (!policy_snapshot_valid(snapshot) || index >= snapshot->count)
        return policy_unknown(XCY_POLICY_REASON_INPUT_INVALID);
    status = policy_context_build(&context, snapshot);
    if (status != 1) {
        policy_context_free(&context);
        return policy_unknown(status < 0 ? XCY_POLICY_REASON_INPUT_INVALID : XCY_POLICY_REASON_RESOURCE_FAILURE);
    }
    policy_resolve(&context, index);
    result = context.results[index];
    policy_context_free(&context);
    return result;
}

int xcy_policy_snapshot(const xcy_memory_snapshot_t *snapshot,
                        xcy_policy_result_t *results, size_t capacity) {
    policy_context_t context;
    size_t i;
    int status;
    if (!snapshot || snapshot->count > capacity || snapshot->count > XCY_MEMORY_MAX_PROCESSES)
        return 0;
    if (!snapshot->count) return 1;
    if (!results || !policy_snapshot_valid(snapshot)) return 0;
    status = policy_context_build(&context, snapshot);
    if (status != 1) {
        policy_context_free(&context);
        return 0;
    }
    for (i = 0; i < snapshot->count; ++i) policy_resolve(&context, i);
    memcpy(results, context.results, snapshot->count * sizeof(*results));
    policy_context_free(&context);
    return 1;
}

xcy_policy_result_t xcy_policy_package_results(const xcy_memory_snapshot_t *snapshot,
                                              const xcy_policy_result_t *results,
                                              const char *pkg, uint32_t uid) {
    xcy_policy_result_t result = policy_unknown(0);
    size_t i, members = 0;
    int unknown_member = 0;
    if (!policy_snapshot_valid(snapshot) || !results || !pkg || !*pkg)
        return policy_unknown(XCY_POLICY_REASON_INPUT_INVALID);
    for (i = 0; i < snapshot->count; ++i) {
        const xcy_memory_process_t *p = &snapshot->items[i];
        const xcy_policy_result_t *member;
        if (p->uid != uid) continue;
        ++members;
        member = &results[i];
        result.reasons |= member->reasons;
        if (strcmp(p->pkg, pkg) != 0) result.reasons |= XCY_POLICY_REASON_UID_OWNER_CONFLICT;
        if (member->tier < result.tier) result.tier = member->tier;
        if (member->tier == XCY_POLICY_UNKNOWN) unknown_member = 1;
        if (!member->eligible) result.reasons |= XCY_POLICY_REASON_PACKAGE_MEMBER_HOLD;
    }
    if (!members) result.reasons |= XCY_POLICY_REASON_PACKAGE_EMPTY;
    if (unknown_member && result.tier != XCY_POLICY_P0_PROTECTED) result.tier = XCY_POLICY_UNKNOWN;
    policy_finish(&result);
    if (result.reasons & XCY_POLICY_REASON_PACKAGE_MEMBER_HOLD) result.eligible = 0;
    return result;
}

static int policy_package_list_contains(const char *list, const char *pkg) {
    const char *cursor = list;
    size_t package_length = strlen(pkg);
    while (*cursor) {
        const char *end = strchr(cursor, ',');
        size_t length = end ? (size_t)(end - cursor) : strlen(cursor);
        if (length == package_length && !memcmp(cursor, pkg, length)) return 1;
        if (!end) break;
        cursor = end + 1;
    }
    return 0;
}

int xcy_policy_pageout_package_results(const xcy_memory_snapshot_t *snapshot,
                                       const xcy_policy_result_t *results,
                                       const char *pkg, uint32_t uid) {
    size_t i;
    int cached_member = 0;
    xcy_memory_token_t package;
    if (!policy_snapshot_valid(snapshot) || !results || !pkg || !*pkg) return 0;
    package.text = pkg; package.len = strlen(pkg);
    if (!xcy_memory_package(package)) return 0;
    for (i = 0; i < snapshot->count; ++i) {
        const xcy_memory_process_t *p = &snapshot->items[i];
        const xcy_policy_result_t *member = &results[i];
        int related = p->uid == uid || (p->uid / 100000U == uid / 100000U &&
            (!strcmp(p->pkg, pkg) || policy_package_list_contains(p->pkg_list, pkg)));
        if (!related) continue;
        /* A same-user association under another owner cannot be silently
         * discarded just because only this owner's cached subset is wanted. */
        if (p->uid != uid || !policy_live(p) ||
            p->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
            strcmp(p->pkg, pkg) || strcmp(p->pkg_list, pkg) ||
            (member->reasons & (POLICY_UNCERTAIN | POLICY_OWNER_VETO))) return 0;
        if (member->tier == XCY_POLICY_P1_IMPORTANT) continue;
        if ((member->tier != XCY_POLICY_P3_CACHED && member->tier != XCY_POLICY_P4_EMPTY) ||
            !member->eligible || (member->reasons & XCY_POLICY_REASON_DEPENDENCY_IMPORTANT)) return 0;
        cached_member = 1;
    }
    return cached_member;
}

int xcy_policy_kill_package_results(const xcy_memory_snapshot_t *snapshot,
                                    const xcy_policy_result_t *results,
                                    const char *pkg, uint32_t uid) {
    size_t i, members = 0;
    xcy_memory_token_t package;
    if (!policy_snapshot_valid(snapshot) || !results || !pkg || !*pkg) return 0;
    package.text = pkg; package.len = strlen(pkg);
    if (!xcy_memory_package(package)) return 0;
    for (i = 0; i < snapshot->count; ++i) {
        const xcy_memory_process_t *p = &snapshot->items[i];
        const xcy_policy_result_t *member = &results[i];
        int related = p->uid == uid || (p->uid / 100000U == uid / 100000U &&
            (!strcmp(p->pkg, pkg) || policy_package_list_contains(p->pkg_list, pkg)));
        if (!related) continue;
        if (p->uid != uid || !policy_live(p) ||
            p->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
            strcmp(p->pkg, pkg) || strcmp(p->pkg_list, pkg) ||
            (member->reasons & (POLICY_UNCERTAIN | POLICY_OWNER_VETO)) ||
            member->tier < XCY_POLICY_P1_IMPORTANT || member->tier > XCY_POLICY_P4_EMPTY)
            return 0;
        ++members;
    }
    return members != 0;
}

xcy_policy_result_t xcy_policy_package(const xcy_memory_snapshot_t *snapshot,
                                      const char *pkg, uint32_t uid) {
    policy_context_t context;
    xcy_policy_result_t result;
    size_t i;
    int status;
    if (!policy_snapshot_valid(snapshot) || !pkg || !*pkg)
        return policy_unknown(XCY_POLICY_REASON_INPUT_INVALID);
    status = policy_context_build(&context, snapshot);
    if (status != 1) {
        policy_context_free(&context);
        return policy_unknown(status < 0 ? XCY_POLICY_REASON_INPUT_INVALID : XCY_POLICY_REASON_RESOURCE_FAILURE);
    }
    for (i = 0; i < snapshot->count; ++i)
        if (snapshot->items[i].uid == uid) policy_resolve(&context, i);
    result = xcy_policy_package_results(snapshot, context.results, pkg, uid);
    policy_context_free(&context);
    return result;
}

const char *xcy_policy_tier_name(xcy_policy_tier_t tier) {
    switch (tier) {
        case XCY_POLICY_P0_PROTECTED: return "P0_PROTECTED";
        case XCY_POLICY_P1_IMPORTANT: return "P1_IMPORTANT";
        case XCY_POLICY_P2_TRANSIENT: return "P2_TRANSIENT";
        case XCY_POLICY_P3_CACHED: return "P3_CACHED";
        case XCY_POLICY_P4_EMPTY: return "P4_EMPTY";
        default: return "UNKNOWN";
    }
}

int xcy_policy_confirm_update(xcy_policy_confirmation_t *state, uint64_t fingerprint,
                             uint64_t now_ms, int overlimit_eligible,
                             uint64_t *next_due_ms) {
    uint64_t next, target;
    if (next_due_ms) *next_due_ms = 0;
    if (!state) return 0;
    if (!overlimit_eligible) { memset(state, 0, sizeof(*state)); return 0; }
    if (!state->samples || state->fingerprint != fingerprint || now_ms < state->last_ms ||
        now_ms - state->last_ms > 60000U) {
        state->fingerprint = fingerprint; state->first_ms = now_ms;
        state->last_ms = now_ms; state->samples = 1;
    } else if (now_ms - state->last_ms >= 30000U) {
        state->last_ms = now_ms;
        if (state->samples < 4) ++state->samples;
    }
    if (state->samples >= 4 && now_ms - state->first_ms >= 90000U) return 1;
    if (state->last_ms > XCY_RECLAIM_UINT64_MAX - 30000U) return 0;
    next = state->last_ms + 30000U;
    if (state->first_ms <= XCY_RECLAIM_UINT64_MAX - 90000U) {
        target = state->first_ms + 90000U;
        if (target > now_ms && target < next) next = target;
    }
    if (next_due_ms) *next_due_ms = next;
    return 0;
}

size_t xcy_policy_format_reasons(uint64_t reasons, char *buf, size_t size) {
    static const struct { uint64_t bit; const char *name; } names[] = {
#define POLICY_REASON_NAME(name) {XCY_POLICY_REASON_##name, #name}
        POLICY_REASON_NAME(INPUT_INVALID), POLICY_REASON_NAME(IDENTITY_INVALID),
        POLICY_REASON_NAME(OWNER_UNKNOWN), POLICY_REASON_NAME(NOT_THIRD_PARTY),
        POLICY_REASON_NAME(SHARED_UID), POLICY_REASON_NAME(UNKNOWN_IMPORTANCE),
        POLICY_REASON_NAME(UNKNOWN_STATE), POLICY_REASON_NAME(GONE),
        POLICY_REASON_NAME(PERSISTENT_FLAG), POLICY_REASON_NAME(CANT_SAVE_FLAG),
        POLICY_REASON_NAME(IMPORTANCE_PROTECT), POLICY_REASON_NAME(STATE_PROTECT),
        POLICY_REASON_NAME(IMPORTANT_BUSINESS), POLICY_REASON_NAME(TRANSIENT),
        POLICY_REASON_NAME(CACHED), POLICY_REASON_NAME(EMPTY), POLICY_REASON_NAME(FIELDS_DISAGREE),
        POLICY_REASON_NAME(UNKNOWN_FLAGS), POLICY_REASON_NAME(DEPENDENCY_PRESENT),
        POLICY_REASON_NAME(DEPENDENCY_MISSING), POLICY_REASON_NAME(DEPENDENCY_INVALID),
        POLICY_REASON_NAME(DEPENDENCY_CYCLE), POLICY_REASON_NAME(DEPENDENCY_IMPORTANT),
        POLICY_REASON_NAME(DEPENDENCY_UNKNOWN), POLICY_REASON_NAME(DEPENDENCY_FIELDS),
        POLICY_REASON_NAME(PACKAGE_EMPTY), POLICY_REASON_NAME(PACKAGE_MEMBER_HOLD),
        POLICY_REASON_NAME(RESOURCE_FAILURE), POLICY_REASON_NAME(DEPENDENCY_SAFE),
        POLICY_REASON_NAME(UID_OWNER_CONFLICT)
#undef POLICY_REASON_NAME
    };
    size_t i, j, length = 0;
    uint64_t known = 0;
    if (!buf) size = 0;
    for (i = 0; i <= sizeof(names) / sizeof(names[0]); ++i) {
        const char *name;
        if (i == sizeof(names) / sizeof(names[0])) {
            if (!reasons) name = "none";
            else if (reasons & ~known) name = "unknown_reason_bits";
            else break;
        } else {
            known |= names[i].bit;
            if (!(reasons & names[i].bit)) continue;
            name = names[i].name;
        }
        if (length) { if (length + 1 < size) buf[length] = '|'; ++length; }
        for (j = 0; name[j]; ++j) {
            if (length + 1 < size) buf[length] = name[j];
            ++length;
        }
    }
    if (size) buf[length < size ? length : size - 1] = 0;
    return length;
}
