#ifndef XCY_PROC_GUARD_MEMORY_SNAPSHOT_H
#define XCY_PROC_GUARD_MEMORY_SNAPSHOT_H

#include "xcy_reclaim_port.h"

#define XCY_MEMORY_MAX_PROCESSES 4096U
#define XCY_MEMORY_MAX_WIRE_BYTES (4U * 1024U * 1024U)
#define XCY_MEMORY_MAX_LINE_BYTES 4096U
#define XCY_MEMORY_TOKEN_BYTES 65U
#define XCY_MEMORY_PACKAGE_BYTES 256U
#define XCY_MEMORY_PROCESS_BYTES 512U
#define XCY_MEMORY_COMPONENT_BYTES 512U
#define XCY_MEMORY_PACKAGE_LIST_BYTES 1024U

enum {
    XCY_MEMORY_CLASS_UNKNOWN = 0,
    XCY_MEMORY_CLASS_THIRD_PARTY = 1,
    XCY_MEMORY_CLASS_SYSTEM = 2,
    XCY_MEMORY_CLASS_UPDATED_SYSTEM = 3,
    XCY_MEMORY_CLASS_PRIVILEGED = 4,
    XCY_MEMORY_CLASS_SHARED_UID = 5,
    XCY_MEMORY_CLASS_XPOSED_MODULE = 6,
    XCY_MEMORY_CLASS_PRELOADED = 7
};

enum {
    XCY_MEMORY_IDENTITY_VALID = 1,
    XCY_MEMORY_PSS_VALID = 2
};

typedef struct {
    uint32_t pid;
    /* AMS owner UID is preserved; V4 carries the actual /proc UID separately. */
    uint32_t uid;
    uint32_t kernel_uid;
    uint8_t kernel_uid_present;
    uint64_t starttime;
    uint64_t sampled_elapsed_ms;
    uint64_t rss_kb;
    uint64_t pss_kb;
    int32_t importance;
    int32_t procstate;
    uint32_t classification;
    uint32_t flags;
    char pkg[XCY_MEMORY_PACKAGE_BYTES];
    char process[XCY_MEMORY_PROCESS_BYTES];
    uint32_t framework_flags;
    int32_t lru;
    int32_t reason_pid;
    int32_t reason_code;
    char reason_component[XCY_MEMORY_COMPONENT_BYTES];
    char pkg_list[XCY_MEMORY_PACKAGE_LIST_BYTES];
} xcy_memory_process_t;

static inline uint32_t xcy_memory_kernel_uid(const xcy_memory_process_t *process) {
    return process->kernel_uid_present ? process->kernel_uid : process->uid;
}

typedef struct {
    char nonce[XCY_MEMORY_TOKEN_BYTES];
    char epoch[XCY_MEMORY_TOKEN_BYTES];
    /* 1/2: resident PSS. 3/4: Framework API PSS, including its swap/memtrack
     * accounting. Cache owners must reject mixing different wire versions. */
    uint32_t wire_version;
    uint64_t observed_elapsed_ms;
    size_t count;
    xcy_memory_process_t *items;
} xcy_memory_snapshot_t;

typedef struct {
    char pkg[XCY_MEMORY_PACKAGE_BYTES];
    uint32_t uid;
    int32_t importance;
    int32_t procstate;
    size_t pid_count;
    uint64_t rss_kb;
    uint64_t pss_kb;
    uint64_t sampled_elapsed_ms;
    int pss_complete;
} xcy_memory_bucket_t;

typedef struct {
    size_t count;
    xcy_memory_bucket_t *items;
} xcy_memory_buckets_t;

/* The validator must read the current PID's UID and /proc/PID/stat starttime.
 * It is called only after the complete frame is structurally valid, once for
 * every identity-valid row, including system/shared/unknown rows. Return 1 only
 * on an exact match. It must not send signals or publish partial state. */
typedef int (*xcy_memory_identity_validator_t)(const xcy_memory_process_t *, void *);

typedef struct {
    const char *text;
    size_t len;
} xcy_memory_token_t;

static inline void xcy_memory_snapshot_free(xcy_memory_snapshot_t *snapshot) {
    if (snapshot == NULL) return;
    xcy_snapshot_free(snapshot->items);
    memset(snapshot, 0, sizeof(*snapshot));
}

static inline void xcy_memory_buckets_free(xcy_memory_buckets_t *buckets) {
    if (buckets == NULL) return;
    xcy_snapshot_free(buckets->items);
    memset(buckets, 0, sizeof(*buckets));
}

static inline int xcy_memory_token_equals(xcy_memory_token_t token, const char *text) {
    size_t n = strlen(text);
    return token.len == n && memcmp(token.text, text, n) == 0;
}

static inline int xcy_memory_uint(xcy_memory_token_t token, uint64_t *value) {
    size_t i;
    uint64_t n = 0;
    if (token.len == 0) return 0;
    for (i = 0; i < token.len; ++i) {
        unsigned int digit;
        if (token.text[i] < '0' || token.text[i] > '9') return 0;
        digit = (unsigned int)(token.text[i] - '0');
        if (n > (XCY_RECLAIM_UINT64_MAX - digit) / 10U) return 0;
        n = n * 10U + digit;
    }
    *value = n;
    return 1;
}

static inline int xcy_memory_int32(xcy_memory_token_t token, int32_t *value) {
    int negative = token.len > 0 && token.text[0] == '-';
    uint64_t magnitude;
    if (negative) { ++token.text; --token.len; }
    if (!xcy_memory_uint(token, &magnitude) ||
        magnitude > (uint64_t)XCY_RECLAIM_INT32_MAX + (unsigned int)negative) return 0;
    if (!negative) *value = (int32_t)magnitude;
    else *value = magnitude == (uint64_t)XCY_RECLAIM_INT32_MAX + 1U ? XCY_RECLAIM_INT32_MIN : -(int32_t)magnitude;
    return 1;
}

static inline int xcy_memory_ascii_word(xcy_memory_token_t token) {
    size_t i;
    for (i = 0; i < token.len; ++i) {
        unsigned char c = (unsigned char)token.text[i];
        if (c < 33U || c > 126U) return 0;
    }
    return token.len > 0;
}

static inline int xcy_memory_copy_word(char *out, size_t capacity,
                                      xcy_memory_token_t token) {
    if (token.len >= capacity || !xcy_memory_ascii_word(token)) return 0;
    memcpy(out, token.text, token.len);
    out[token.len] = '\0';
    return 1;
}

static inline int xcy_memory_identifier(xcy_memory_token_t token) {
    size_t i;
    if (token.len == 0 || token.len >= XCY_MEMORY_TOKEN_BYTES) return 0;
    for (i = 0; i < token.len; ++i) {
        char c = token.text[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-' || c == '_' ||
              c == '.' || c == ':')) return 0;
    }
    return 1;
}

static inline int xcy_memory_package(xcy_memory_token_t token) {
    size_t i;
    int segment_start = 1;
    if (token.len == 0 || token.len >= XCY_MEMORY_PACKAGE_BYTES) return 0;
    for (i = 0; i < token.len; ++i) {
        char c = token.text[i];
        if (c == '.') {
            if (segment_start) return 0;
            segment_start = 1;
        } else if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_') {
            segment_start = 0;
        } else if (c < '0' || c > '9' || segment_start) {
            return 0;
        }
    }
    return !segment_start;
}

/* Metadata only: each comma-separated AMS package is syntactically valid.
 * A third-party owner additionally requires exactly one matching raw package;
 * neither process names, reason components nor this list establish ownership. */
static inline int xcy_memory_package_list(xcy_memory_token_t token) {
    size_t begin = 0, i;
    if (xcy_memory_token_equals(token, "-")) return 1;
    if (token.len == 0 || token.len >= XCY_MEMORY_PACKAGE_LIST_BYTES) return 0;
    for (i = 0; i <= token.len; ++i) {
        if (i == token.len || token.text[i] == ',') {
            xcy_memory_token_t package;
            package.text = token.text + begin;
            package.len = i - begin;
            if (!xcy_memory_package(package)) return 0;
            begin = i + 1;
        }
    }
    return 1;
}

/* Framework can report an isolated renderer under its verified application's
 * UID. This is not a UID equality exception for arbitrary processes: only a
 * same-user Android isolated UID plus one verified package can be mapped. */
static inline int xcy_memory_uid_mapping_valid(const xcy_memory_process_t *process) {
    uint32_t kernel_uid = xcy_memory_kernel_uid(process);
    uint32_t owner_app_id = process->uid % 100000U;
    uint32_t kernel_app_id = kernel_uid % 100000U;
    xcy_memory_token_t package = {process->pkg, strlen(process->pkg)};
    if (kernel_uid == process->uid) return 1;
    return process->kernel_uid_present &&
        (process->flags & XCY_MEMORY_IDENTITY_VALID) &&
        process->uid / 100000U == kernel_uid / 100000U &&
        owner_app_id >= 10000U && owner_app_id <= 19999U &&
        kernel_app_id >= 90000U && kernel_app_id <= 99999U &&
        process->classification != XCY_MEMORY_CLASS_UNKNOWN &&
        process->classification != XCY_MEMORY_CLASS_SHARED_UID &&
        process->classification <= XCY_MEMORY_CLASS_PRELOADED &&
        xcy_memory_package(package) && strcmp(process->pkg, process->pkg_list) == 0;
}

/* A line is bounded independently of the frame. Only horizontal separators
 * are accepted; NUL/control bytes cannot hide a second record or field. */
static inline int xcy_memory_line(const char *text, size_t len, size_t *offset,
                                  xcy_memory_token_t *tokens, size_t expected) {
    size_t start = *offset, end = start, n = 0, p;
    if (start >= len) return 0;
    while (end < len && text[end] != '\n') ++end;
    if (end == len || end - start > XCY_MEMORY_MAX_LINE_BYTES) return 0;
    *offset = end + 1;
    if (end > start && text[end - 1] == '\r') --end;
    p = start;
    while (p < end) {
        size_t token_start;
        while (p < end && (text[p] == ' ' || text[p] == '\t')) ++p;
        if (p == end) break;
        token_start = p;
        while (p < end && text[p] != ' ' && text[p] != '\t') {
            unsigned char c = (unsigned char)text[p];
            if (c < 33U || c > 126U) return 0;
            ++p;
        }
        if (n >= expected) return 0;
        tokens[n].text = text + token_start;
        tokens[n].len = p - token_start;
        ++n;
    }
    return n == expected;
}

static inline int xcy_memory_fresh(uint64_t stamp, uint64_t now, uint64_t max_age) {
    return stamp > 0 && stamp <= now && now - stamp <= max_age;
}

/* A unique third-party owner cannot also belong to a declared shared UID, or
 * share its exact UID with another package. The helper must additionally use
 * PM's full UID package list: absence from this frame proves no exclusivity. */
static inline int xcy_memory_owners_consistent(const xcy_memory_snapshot_t *snapshot) {
    size_t i, j;
    for (i = 0; i < snapshot->count; ++i) {
        const xcy_memory_process_t *a = &snapshot->items[i];
        for (j = 0; j < i; ++j) {
            const xcy_memory_process_t *b = &snapshot->items[j];
            if (a->pid == b->pid) return 0;
            if (!(a->flags & XCY_MEMORY_IDENTITY_VALID) ||
                !(b->flags & XCY_MEMORY_IDENTITY_VALID))
                continue;
            /* A single Linux isolated UID cannot authorize two different
             * owners, including a second row pretending to be unassociated. */
            if (xcy_memory_kernel_uid(a) == xcy_memory_kernel_uid(b) &&
                (xcy_memory_kernel_uid(a) != a->uid || xcy_memory_kernel_uid(b) != b->uid) &&
                (a->uid != b->uid || strcmp(a->pkg, b->pkg) != 0)) return 0;
            if (a->uid != b->uid) continue;
            if (a->classification != XCY_MEMORY_CLASS_THIRD_PARTY &&
                b->classification != XCY_MEMORY_CLASS_THIRD_PARTY) continue;
            if (a->classification == XCY_MEMORY_CLASS_SHARED_UID ||
                b->classification == XCY_MEMORY_CLASS_SHARED_UID) return 0;
            if (strcmp(a->pkg, "-") != 0 && strcmp(b->pkg, "-") != 0 &&
                strcmp(a->pkg, b->pkg) != 0) return 0;
        }
    }
    return 1;
}

/* Wire V1/V2/V3/V4:
 * XCY_MEM_V1|XCY_MEM_V2|XCY_MEM_V3|XCY_MEM_V4 nonce epoch observed_elapsed_ms count status\n
 * P pid uid starttime sampled_elapsed_ms rss_kb pss_kb importance procstate class flags pkg process\n
 * V2/V3 append six fields before that row's newline:
 *   framework_flags lru importance_reason_pid importance_reason_code importance_reason_component raw_pkg_list
 * V4 additionally appends kernel_uid, preserving the AMS owner uid field.
 * END nonce epoch count\n
 *
 * `out` must be zero-initialized or a previously accepted snapshot. Parsing,
 * allocation, freshness, trailer and current PID identity checks all finish
 * before replacing it. On any error the old snapshot is completely unchanged.
 * flags=0 rows preserve requested-row completeness and are never validated or
 * aggregated. Inventory rows have no valid PSS; no name-based ownership exists.
 * V1 defaults its absent metadata to zero, lru=-1, and both strings="-".
 * Framework flags are metadata, entirely separate from the identity/PSS flags.
 * V2/V3 Framework importance/state/LRU/reason integers remain signed int32 values,
 * including UNKNOWN=-1 and OEM values; the parser never assigns policy tiers.
 * V1/V2 resident PSS cannot exceed RSS. V3 preserves Framework API PSS without
 * that constraint; the API may include SwapPss/memtrack. RSS and native fields
 * are never rewritten to make the two metrics agree. wire_version identifies
 * the accepted whole frame; production callers must enforce V4 before caching.
 */
static inline int xcy_memory_snapshot_parse(
    const char *text, size_t len, const char *expected_nonce, const char *expected_epoch,
    uint64_t now_elapsed_ms, uint64_t max_age_ms,
    xcy_memory_identity_validator_t validator, void *validator_context,
    xcy_memory_snapshot_t *out
) {
    xcy_memory_snapshot_t pending = {0};
    xcy_memory_token_t tokens[20];
    uint64_t count, status;
    size_t offset = 0, i, row_tokens;
    int version;
    if (text == NULL || expected_nonce == NULL || expected_epoch == NULL || out == NULL ||
        len == 0 || len > XCY_MEMORY_MAX_WIRE_BYTES || text[len - 1] != '\n' ||
        memchr(text, '\0', len) != NULL ||
        !xcy_memory_line(text, len, &offset, tokens, 6) ||
        (!xcy_memory_token_equals(tokens[0], "XCY_MEM_V1") &&
         !xcy_memory_token_equals(tokens[0], "XCY_MEM_V2") &&
         !xcy_memory_token_equals(tokens[0], "XCY_MEM_V3") &&
         !xcy_memory_token_equals(tokens[0], "XCY_MEM_V4")) ||
        !xcy_memory_identifier(tokens[1]) || !xcy_memory_identifier(tokens[2]) ||
        !xcy_memory_token_equals(tokens[1], expected_nonce) ||
        !xcy_memory_token_equals(tokens[2], expected_epoch) ||
        !xcy_memory_uint(tokens[3], &pending.observed_elapsed_ms) ||
        !xcy_memory_fresh(pending.observed_elapsed_ms, now_elapsed_ms, max_age_ms) ||
        !xcy_memory_uint(tokens[4], &count) || count > XCY_MEMORY_MAX_PROCESSES ||
        !xcy_memory_uint(tokens[5], &status) || status != 0) return 0;
    version = xcy_memory_token_equals(tokens[0], "XCY_MEM_V4") ? 4 :
        (xcy_memory_token_equals(tokens[0], "XCY_MEM_V3") ? 3 :
        (xcy_memory_token_equals(tokens[0], "XCY_MEM_V2") ? 2 : 1));
    pending.wire_version = (uint32_t)version;
    row_tokens = version == 4 ? 20U : (version >= 2 ? 19U : 13U);
    if (!xcy_memory_copy_word(pending.nonce, sizeof(pending.nonce), tokens[1]) ||
        !xcy_memory_copy_word(pending.epoch, sizeof(pending.epoch), tokens[2])) return 0;
    pending.count = (size_t)count;
    if (pending.count > 0) {
        pending.items = (xcy_memory_process_t *)xcy_snapshot_calloc(pending.count, sizeof(*pending.items));
        if (pending.items == NULL) return 0;
    }
    for (i = 0; i < pending.count; ++i) {
        xcy_memory_process_t *item = &pending.items[i];
        uint64_t numbers[10] = {0};
        size_t k;
        if (!xcy_memory_line(text, len, &offset, tokens, row_tokens) ||
            !xcy_memory_token_equals(tokens[0], "P")) goto invalid;
        for (k = 0; k < 10; ++k) {
            if (version >= 2 && (k == 6 || k == 7)) continue;
            if (!xcy_memory_uint(tokens[k + 1], &numbers[k])) goto invalid;
        }
        if (numbers[0] > XCY_RECLAIM_INT32_MAX || numbers[1] > XCY_RECLAIM_UINT32_MAX ||
            numbers[4] > XCY_RECLAIM_INT64_MAX || numbers[5] > XCY_RECLAIM_INT64_MAX ||
            numbers[6] > XCY_RECLAIM_INT32_MAX || numbers[7] > XCY_RECLAIM_INT32_MAX ||
            numbers[8] > XCY_MEMORY_CLASS_PRELOADED || numbers[9] > 3U ||
            !xcy_memory_copy_word(item->pkg, sizeof(item->pkg), tokens[11]) ||
            !xcy_memory_copy_word(item->process, sizeof(item->process), tokens[12]) ||
            (!xcy_memory_token_equals(tokens[11], "-") && !xcy_memory_package(tokens[11])))
            goto invalid;
        item->pid = (uint32_t)numbers[0];
        item->uid = (uint32_t)numbers[1];
        item->kernel_uid = item->uid;
        item->kernel_uid_present = 1;
        item->starttime = numbers[2];
        item->sampled_elapsed_ms = numbers[3];
        item->rss_kb = numbers[4];
        item->pss_kb = numbers[5];
        item->importance = (int32_t)numbers[6];
        item->procstate = (int32_t)numbers[7];
        item->classification = (uint32_t)numbers[8];
        item->flags = (uint32_t)numbers[9];
        item->lru = -1;
        memcpy(item->reason_component, "-", 2);
        memcpy(item->pkg_list, "-", 2);
        if (version >= 2) {
            uint64_t framework_flags;
            if (!xcy_memory_int32(tokens[7], &item->importance) ||
                !xcy_memory_int32(tokens[8], &item->procstate) ||
                !xcy_memory_uint(tokens[13], &framework_flags) || framework_flags > XCY_RECLAIM_UINT32_MAX ||
                !xcy_memory_int32(tokens[14], &item->lru) ||
                !xcy_memory_int32(tokens[15], &item->reason_pid) ||
                !xcy_memory_int32(tokens[16], &item->reason_code)) goto invalid;
            item->framework_flags = (uint32_t)framework_flags;
            if (!xcy_memory_copy_word(item->reason_component, sizeof(item->reason_component), tokens[17]) ||
                !xcy_memory_package_list(tokens[18]) ||
                !xcy_memory_copy_word(item->pkg_list, sizeof(item->pkg_list), tokens[18]) ||
                (item->classification == XCY_MEMORY_CLASS_THIRD_PARTY &&
                 strcmp(item->pkg_list, item->pkg) != 0)) goto invalid;
        }
        if (version == 4) {
            uint64_t kernel_uid;
            if (!xcy_memory_uint(tokens[19], &kernel_uid) || kernel_uid > XCY_RECLAIM_UINT32_MAX) goto invalid;
            item->kernel_uid = (uint32_t)kernel_uid;
        }
        if (item->flags & XCY_MEMORY_IDENTITY_VALID) {
            if (item->pid <= 1 || item->starttime == 0 || validator == NULL ||
                !xcy_memory_uid_mapping_valid(item)) goto invalid;
        } else if (item->sampled_elapsed_ms != 0 || item->pss_kb != 0) {
            goto invalid;
        }
        if (item->classification == XCY_MEMORY_CLASS_THIRD_PARTY &&
            (!(item->flags & XCY_MEMORY_IDENTITY_VALID) ||
             xcy_memory_token_equals(tokens[11], "-"))) goto invalid;
        if (item->flags & XCY_MEMORY_PSS_VALID) {
            if (!(item->flags & XCY_MEMORY_IDENTITY_VALID) || item->pss_kb == 0 ||
                (version < 3 && item->pss_kb > item->rss_kb) ||
                item->sampled_elapsed_ms > pending.observed_elapsed_ms ||
                !xcy_memory_fresh(item->sampled_elapsed_ms, now_elapsed_ms, max_age_ms))
                goto invalid;
        } else if (item->pss_kb != 0 ||
                   item->sampled_elapsed_ms > pending.observed_elapsed_ms) {
            goto invalid;
        }
    }
    if (!xcy_memory_line(text, len, &offset, tokens, 4) || offset != len ||
        !xcy_memory_token_equals(tokens[0], "END") ||
        !xcy_memory_token_equals(tokens[1], pending.nonce) ||
        !xcy_memory_token_equals(tokens[2], pending.epoch) ||
        !xcy_memory_uint(tokens[3], &count) || count != pending.count ||
        !xcy_memory_owners_consistent(&pending)) goto invalid;
    for (i = 0; i < pending.count; ++i) {
        if ((pending.items[i].flags & XCY_MEMORY_IDENTITY_VALID) &&
            validator(&pending.items[i], validator_context) != 1) goto invalid;
    }
    xcy_memory_snapshot_free(out);
    *out = pending;
    return 1;
invalid:
    xcy_memory_snapshot_free(&pending);
    return 0;
}

/* Unknown/shared/system/module ownership remains visible in the accepted inventory
 * but never becomes a third-party bucket. An unknown identity-valid process
 * on an otherwise eligible UID prevents publishing a partial UID bucket. */
static inline int xcy_memory_third_party_uid_complete(
    const xcy_memory_snapshot_t *snapshot, const xcy_memory_process_t *target
) {
    size_t i;
    for (i = 0; i < snapshot->count; ++i) {
        const xcy_memory_process_t *item = &snapshot->items[i];
        if ((item->flags & XCY_MEMORY_IDENTITY_VALID) && item->uid == target->uid &&
            (item->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
             strcmp(item->pkg, target->pkg) != 0)) return 0;
    }
    return 1;
}

/* PSS is complete only when every identity-valid member has a fresh sample.
 * Missing samples produce pss_complete=0, pss_kb=0, sampled_elapsed_ms=0;
 * a partial sum is never advertised as an application's whole memory use.
 * `out` follows the same initialize/atomic-replacement contract as parse(). */
static inline int xcy_memory_snapshot_aggregate(const xcy_memory_snapshot_t *snapshot,
                                               xcy_memory_buckets_t *out) {
    xcy_memory_buckets_t pending = {0};
    size_t i;
    if (snapshot == NULL || out == NULL || snapshot->wire_version < 1 || snapshot->wire_version > 4 ||
        snapshot->count > XCY_MEMORY_MAX_PROCESSES ||
        (snapshot->count > 0 && snapshot->items == NULL) ||
        !xcy_memory_owners_consistent(snapshot)) return 0;
    if (snapshot->count > 0) {
        pending.items = (xcy_memory_bucket_t *)xcy_snapshot_calloc(snapshot->count, sizeof(*pending.items));
        if (pending.items == NULL) return 0;
    }
    for (i = 0; i < snapshot->count; ++i) {
        const xcy_memory_process_t *item = &snapshot->items[i];
        xcy_memory_bucket_t *bucket;
        size_t j;
        if ((item->flags & XCY_MEMORY_IDENTITY_VALID) && !xcy_memory_uid_mapping_valid(item)) goto invalid;
        if (!(item->flags & XCY_MEMORY_IDENTITY_VALID) ||
            item->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
            !xcy_memory_third_party_uid_complete(snapshot, item)) continue;
        for (j = 0; j < pending.count; ++j) {
            if (pending.items[j].uid == item->uid &&
                strcmp(pending.items[j].pkg, item->pkg) == 0) break;
        }
        bucket = &pending.items[j];
        if (j == pending.count) {
            memcpy(bucket->pkg, item->pkg, sizeof(bucket->pkg));
            bucket->uid = item->uid;
            bucket->importance = item->importance;
            bucket->procstate = item->procstate;
            bucket->pss_complete = 1;
            bucket->sampled_elapsed_ms = item->sampled_elapsed_ms;
            ++pending.count;
        }
        /* The Android importer stores KB totals in signed 64-bit longs. */
        if ((uint64_t)XCY_RECLAIM_INT64_MAX - bucket->rss_kb < item->rss_kb ||
            (uint64_t)XCY_RECLAIM_INT64_MAX - bucket->pss_kb < item->pss_kb) goto invalid;
        bucket->rss_kb += item->rss_kb;
        bucket->pss_kb += item->pss_kb;
        ++bucket->pid_count;
        if (item->importance < bucket->importance) bucket->importance = item->importance;
        if (item->procstate < bucket->procstate) bucket->procstate = item->procstate;
        if (!(item->flags & XCY_MEMORY_PSS_VALID)) bucket->pss_complete = 0;
        if (item->sampled_elapsed_ms < bucket->sampled_elapsed_ms)
            bucket->sampled_elapsed_ms = item->sampled_elapsed_ms;
    }
    for (i = 0; i < pending.count; ++i) {
        if (!pending.items[i].pss_complete) {
            pending.items[i].pss_kb = 0;
            pending.items[i].sampled_elapsed_ms = 0;
        }
    }
    xcy_memory_buckets_free(out);
    *out = pending;
    return 1;
invalid:
    xcy_memory_buckets_free(&pending);
    return 0;
}

#endif
