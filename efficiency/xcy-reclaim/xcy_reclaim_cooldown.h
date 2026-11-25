#ifndef XCY_RECLAIM_COOLDOWN_H
#define XCY_RECLAIM_COOLDOWN_H

#include "xcy_reclaim_port.h"

#define XCY_KILL_COOLDOWN_MS XCY_RECLAIM_U64_C(120000)

typedef struct {
    uint32_t uid;
    char pkg[256];
    uint64_t killed_ms;
} xcy_kill_cooldown_entry_t;

static inline int xcy_kill_cooldown_active(const xcy_kill_cooldown_entry_t *entry,
                                           uint64_t now_ms) {
    return entry && entry->uid && entry->pkg[0] &&
        (now_ms < entry->killed_ms || now_ms - entry->killed_ms < XCY_KILL_COOLDOWN_MS);
}

/* This table only withholds permission; it cannot grant eligibility or bypass
 * PID/UID/starttime, whitelist, foreground, playback or sync protection.
 * A later verified foreground use ends respawn suppression, while the separate
 * recent-use policy still applies. Unknown/future usage does not lift a hold. */
static inline int xcy_kill_cooldown_blocked(xcy_kill_cooldown_entry_t *entries,
        size_t capacity, uint32_t uid, const char *pkg, uint64_t now_ms,
        uint64_t last_used_ms) {
    if (!entries || !uid || !pkg || !*pkg) return 0;
    for (size_t i = 0; i < capacity; ++i) {
        xcy_kill_cooldown_entry_t *entry = &entries[i];
        if (entry->uid != uid || strcmp(entry->pkg, pkg)) continue;
        if ((now_ms >= entry->killed_ms && last_used_ms > entry->killed_ms && last_used_ms <= now_ms) ||
            !xcy_kill_cooldown_active(entry, now_ms)) {
            memset(entry, 0, sizeof(*entry));
            return 0;
        }
        return 1;
    }
    return 0;
}

/* Call exactly once after the first successful signal in an application batch.
 * Reuse an expired/free slot, otherwise evict the oldest entry. A full table
 * must never disable reclaim for the daemon lifetime. No timer is registered. */
static inline int xcy_kill_cooldown_record(xcy_kill_cooldown_entry_t *entries,
        size_t capacity, uint32_t uid, const char *pkg, uint64_t now_ms) {
    size_t slot = SIZE_MAX, oldest = 0;
    if (!entries || !capacity || !uid || !pkg || !*pkg || strlen(pkg) >= sizeof(entries[0].pkg)) return 0;
    for (size_t i = 0; i < capacity; ++i) {
        if (entries[i].uid == uid && !strcmp(entries[i].pkg, pkg)) {
            if (now_ms > entries[i].killed_ms) entries[i].killed_ms = now_ms;
            return 1;
        }
        if (slot == SIZE_MAX && !xcy_kill_cooldown_active(&entries[i], now_ms)) slot = i;
        if (entries[i].killed_ms < entries[oldest].killed_ms) oldest = i;
    }
    if (slot == SIZE_MAX) slot = oldest;
    entries[slot].uid = uid;
    memcpy(entries[slot].pkg, pkg, strlen(pkg) + 1);
    entries[slot].killed_ms = now_ms;
    return 1;
}

#endif
