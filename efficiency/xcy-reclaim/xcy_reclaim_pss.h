#ifndef XCY_RECLAIM_PSS_H
#define XCY_RECLAIM_PSS_H

#include "xcy_reclaim_snapshot.h"

/* The original emergency cutoff is independent of the 12% kill admission. */
static inline int64_t xcy_reclaim_urgent_reserve(int64_t total)
{
    return (total / 100) * 6 + ((total % 100) * 6 + 99) / 100;
}

static inline int xcy_reclaim_urgent(int64_t total, int64_t available)
{
    if (total <= 0 || available < 0 || available > total)
        return 0;
    return available < xcy_reclaim_urgent_reserve(total);
}

/* Pure batch-response join. Every requested member must retain its exact
 * PID/UID/starttime and unique owner and supply positive API PSS. A missing,
 * invalid, duplicate, changed, or overflowing member invalidates the entire
 * package. Unrelated packages in the same batch do not contribute to totals.
 * The caller supplies this round's validated query('P') response, never a
 * cached sample; the wire parser/query owns the current-time freshness gate. */
static inline int xcy_pressure_score_pss_total(const xcy_memory_snapshot_t *api,
        const xcy_memory_process_t *members, size_t count, uint64_t *total_out) {
    uint64_t total = 0;
    if (!api || !members || !total_out || !count || count > 512 || count > api->count ||
        (api->wire_version != 3 && api->wire_version != 4) ||
        !api->items || api->count > XCY_MEMORY_MAX_PROCESSES) return 0;
    for (size_t i = 0; i < count; ++i) {
        const xcy_memory_process_t *matched = NULL;
        if (members[i].pid <= 1 || !members[i].starttime || members[i].flags > 3 ||
            !(members[i].flags & XCY_MEMORY_IDENTITY_VALID) ||
            members[i].classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
            strcmp(members[i].pkg, members[0].pkg) || members[i].uid != members[0].uid ||
            strcmp(members[i].pkg_list, members[i].pkg)) return 0;
        for (size_t k = 0; k < i; ++k) if (members[k].pid == members[i].pid) return 0;
        for (size_t j = 0; j < api->count; ++j) {
            const xcy_memory_process_t *p = &api->items[j];
            if (p->pid != members[i].pid) continue;
            if (matched) return 0;
            matched = p;
        }
        if (!matched || matched->uid != members[i].uid ||
            xcy_memory_kernel_uid(matched) != xcy_memory_kernel_uid(&members[i]) ||
            matched->starttime != members[i].starttime ||
            matched->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
            (matched->flags & (XCY_MEMORY_IDENTITY_VALID | XCY_MEMORY_PSS_VALID)) !=
                (XCY_MEMORY_IDENTITY_VALID | XCY_MEMORY_PSS_VALID) ||
            strcmp(matched->pkg, members[i].pkg) || strcmp(matched->pkg_list, members[i].pkg) ||
            !matched->pss_kb || !matched->sampled_elapsed_ms ||
            matched->sampled_elapsed_ms > api->observed_elapsed_ms ||
            matched->pss_kb > (uint64_t)XCY_RECLAIM_INT64_MAX - total) return 0;
        total += matched->pss_kb;
    }
    *total_out = total;
    return 1;
}


#endif
