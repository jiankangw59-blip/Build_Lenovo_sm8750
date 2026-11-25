#ifndef XCY_RECLAIM_STATE_H
#define XCY_RECLAIM_STATE_H

#ifdef __KERNEL__
#include <linux/limits.h>
#include <linux/types.h>
typedef s64 xcy_reclaim_s64;
typedef s32 xcy_reclaim_s32;
#define XCY_RECLAIM_TIME_MAX S64_MAX
#else
#include <stddef.h>
#include <stdint.h>
typedef int64_t xcy_reclaim_s64;
typedef int32_t xcy_reclaim_s32;
#define XCY_RECLAIM_TIME_MAX INT64_MAX
#endif

typedef enum {
    XCY_SYSTEM_PRESSURE_UNKNOWN = -1,
    XCY_SYSTEM_PRESSURE_HEALTHY = 0,
    XCY_SYSTEM_PRESSURE_WARNING = 1,
    XCY_SYSTEM_PRESSURE_STRESS = 2,
    XCY_SYSTEM_PRESSURE_CRITICAL = 3
} xcy_system_pressure_level_t;

typedef struct {
    /* Required /proc/meminfo values. Use -1 for a missing value; zero
     * MemAvailable is valid, but zero MemTotal is not. */
    xcy_reclaim_s64 total_kb;
    xcy_reclaim_s64 available_kb;
    /* Required memory PSI avg10 values, in basis points: 1.00% = 100.
     * Use -1 for a missing value. The valid range is 0..10000. */
    xcy_reclaim_s32 some_bp;
    xcy_reclaim_s32 full_bp;
} xcy_system_pressure_t;

/* Pure, experimental diagnostics only: these are not LMKD settings or
 * permission to kill, page out, collect PSS, or schedule another sample.
 * Swap usage deliberately is not an input and is never added to RAM usage.
 * All four inputs must be valid, including both PSI values. */
static inline xcy_system_pressure_level_t xcy_system_pressure_classify(
        const xcy_system_pressure_t *pressure) {
    xcy_reclaim_s64 hundreds, remainder;
    if (pressure == NULL || pressure->total_kb <= 0 ||
        pressure->available_kb < 0 ||
        pressure->available_kb > pressure->total_kb ||
        pressure->some_bp < 0 || pressure->some_bp > 10000 ||
        pressure->full_bp < 0 || pressure->full_bp > 10000)
        return XCY_SYSTEM_PRESSURE_UNKNOWN;

    /* Compare against floor(total * percent / 100), without overflowing
     * or rounding an available percentage down across a threshold. */
    hundreds = pressure->total_kb / 100;
    remainder = pressure->total_kb % 100;
    if (pressure->available_kb <= hundreds * 8 + remainder * 8 / 100 &&
        (pressure->some_bp >= 500 || pressure->full_bp >= 100))
        return XCY_SYSTEM_PRESSURE_CRITICAL;
    if (pressure->available_kb <= hundreds * 15 + remainder * 15 / 100 &&
        (pressure->some_bp >= 100 || pressure->full_bp >= 50))
        return XCY_SYSTEM_PRESSURE_STRESS;
    if (pressure->available_kb <= hundreds * 22 + remainder * 22 / 100 &&
        (pressure->some_bp >= 50 || pressure->full_bp >= 10))
        return XCY_SYSTEM_PRESSURE_WARNING;
    return XCY_SYSTEM_PRESSURE_HEALTHY;
}

static inline xcy_reclaim_s64 xcy_system_ram_reserve(xcy_reclaim_s64 total)
{
    return (total / 100) * 12 + ((total % 100) * 12 + 99) / 100;
}

/* Both values use the caller's unit, including pages in the kernel. */
static inline xcy_reclaim_s64 xcy_system_ram_deficit(xcy_reclaim_s64 total,
                                                    xcy_reclaim_s64 available)
{
    xcy_reclaim_s64 reserve;
    if (total <= 0 || available < 0 || available > total)
        return 0;
    reserve = xcy_system_ram_reserve(total);
    return available < reserve ? reserve - available : 0;
}

/* Action gate: RAM only. PSI is an event source/diagnostic, not a condition
 * for PAGEOUT or kill. HEALTHY means available >= 30%; STRESS is [12%, 30%);
 * CRITICAL is strictly below 12%. Missing/invalid meminfo is UNKNOWN.
 * Comparing with ceil(total * percent / 100) preserves strict boundaries
 * without overflow or lossy percentage rounding. Swap is never an input. */
static inline xcy_system_pressure_level_t xcy_system_ram_pressure_classify(
        xcy_reclaim_s64 total_kb, xcy_reclaim_s64 available_kb) {
    xcy_reclaim_s64 hundreds, remainder;
    if (total_kb <= 0 || available_kb < 0 || available_kb > total_kb)
        return XCY_SYSTEM_PRESSURE_UNKNOWN;
    hundreds = total_kb / 100;
    remainder = total_kb % 100;
    if (available_kb < xcy_system_ram_reserve(total_kb))
        return XCY_SYSTEM_PRESSURE_CRITICAL;
    if (available_kb < hundreds * 30 + (remainder * 30 + 99) / 100)
        return XCY_SYSTEM_PRESSURE_STRESS;
    return XCY_SYSTEM_PRESSURE_HEALTHY;
}

/* floor((MemTotal - MemAvailable) * 10000 / MemTotal), or -1 for invalid
 * RAM inputs. This report-only helper does not depend on PSI or swap.
 * A bounded integer search avoids a potentially overflowing scaled product
 * and preserves exact results even at XCY_RECLAIM_TIME_MAX. */
static inline int xcy_system_ram_usage_bp(xcy_reclaim_s64 total_kb,
                                         xcy_reclaim_s64 available_kb) {
    int low = 0, high = 10000;
    xcy_reclaim_s64 used_kb;
    if (total_kb <= 0 || available_kb < 0 || available_kb > total_kb)
        return -1;
    used_kb = total_kb - available_kb;
    while (low < high) {
        int middle = low + (high - low + 1) / 2;
        /* ceil(total * middle / 10000), always in [0, total]. */
        xcy_reclaim_s64 threshold = (total_kb / 10000) * middle +
            ((total_kb % 10000) * middle + 9999) / 10000;
        if (used_kb >= threshold)
            low = middle;
        else
            high = middle - 1;
    }
    return low;
}

enum {
    XCY_SYSTEM_RECLAIM_OBSERVE_MS = 3000,
    XCY_SYSTEM_RECLAIM_EPISODE_GAP_MS = 30000,
    XCY_SYSTEM_RECLAIM_MAX_ACTIONS = 2,
    XCY_SYSTEM_RECLAIM_MAX_CRITICAL_ROUNDS = 4
};

typedef enum {
    XCY_SYSTEM_RECLAIM_INVALID = -1,
    XCY_SYSTEM_RECLAIM_NONE = 0,
    XCY_SYSTEM_RECLAIM_WAIT = 1,
    XCY_SYSTEM_RECLAIM_READY = 2,
    XCY_SYSTEM_RECLAIM_FALLBACK = 3,
    XCY_SYSTEM_RECLAIM_RECLAIM_READY = 4
} xcy_system_reclaim_decision_t;

typedef enum {
    XCY_SYSTEM_RECLAIM_IDLE = 0,
    XCY_SYSTEM_RECLAIM_RATE_WAIT = 1,
    XCY_SYSTEM_RECLAIM_RECLAIM_PENDING = 2,
    XCY_SYSTEM_RECLAIM_RECLAIM_OBSERVING = 3,
    XCY_SYSTEM_RECLAIM_LOW_IDLE = 4,
    XCY_SYSTEM_RECLAIM_ACTION_PENDING = 5,
    XCY_SYSTEM_RECLAIM_OBSERVING = 6,
    XCY_SYSTEM_RECLAIM_FALLBACK_LATCHED = 7,
    XCY_SYSTEM_RECLAIM_FAULT_LATCHED = 8
} xcy_system_reclaim_phase_t;

/* Zero-initialize once for the controller lifetime; do not reinitialize on each
 * event or recovery, because the cross-episode action gap must survive.
 * This state is global, contains no application identity or per-app timer,
 * and must be accessed serially by the caller. */
typedef struct {
    xcy_system_reclaim_phase_t phase;
    unsigned int episode_actions;
    /* Frozen when PAGEOUT is requested; later observations cannot relabel it. */
    xcy_system_pressure_level_t pending_pressure;
    /* A CRITICAL PAGEOUT completed, or its capacity check declined it. */
    int critical_reclaim_handled;
    unsigned int critical_reclaim_rounds;
    /* Accepted PAGEOUT bytes > 0; this does not mean RAM was released. */
    int last_reclaim_succeeded;
    xcy_reclaim_s64 deadline_ms;
    xcy_reclaim_s64 last_now_ms;
    xcy_reclaim_s64 last_action_ms;
    int clock_seen;
    int action_seen;
} xcy_system_reclaim_episode_t;

static inline xcy_system_reclaim_decision_t xcy_system_reclaim_fail_closed(
        xcy_system_reclaim_episode_t *episode) {
    if (episode != NULL) {
        episode->phase = XCY_SYSTEM_RECLAIM_FAULT_LATCHED;
        episode->deadline_ms = 0;
    }
    return XCY_SYSTEM_RECLAIM_INVALID;
}

/* A backward/negative clock or invalid state permanently disables this
 * instance. The caller should restore system LMKD on INVALID or FALLBACK;
 * neither result performs an external action here. */
static inline int xcy_system_reclaim_clock_valid(
        xcy_system_reclaim_episode_t *episode, xcy_reclaim_s64 now_ms) {
    if (episode == NULL ||
        episode->phase < XCY_SYSTEM_RECLAIM_IDLE ||
        episode->phase >= XCY_SYSTEM_RECLAIM_FAULT_LATCHED ||
        episode->episode_actions > XCY_SYSTEM_RECLAIM_MAX_ACTIONS ||
        episode->critical_reclaim_rounds > XCY_SYSTEM_RECLAIM_MAX_CRITICAL_ROUNDS ||
        (episode->critical_reclaim_handled != 0 &&
         episode->critical_reclaim_handled != 1) ||
        (episode->last_reclaim_succeeded != 0 &&
         episode->last_reclaim_succeeded != 1) ||
        (episode->phase == XCY_SYSTEM_RECLAIM_RECLAIM_PENDING &&
         episode->pending_pressure != XCY_SYSTEM_PRESSURE_STRESS &&
         episode->pending_pressure != XCY_SYSTEM_PRESSURE_CRITICAL) ||
        now_ms < 0 || (episode->clock_seen && now_ms < episode->last_now_ms)) {
        xcy_system_reclaim_fail_closed(episode);
        return 0;
    }
    episode->last_now_ms = now_ms;
    episode->clock_seen = 1;
    return 1;
}

static inline int xcy_system_reclaim_set_deadline(
        xcy_system_reclaim_episode_t *episode, xcy_reclaim_s64 now_ms, int delay_ms) {
    if (now_ms > XCY_RECLAIM_TIME_MAX - delay_ms) {
        xcy_system_reclaim_fail_closed(episode);
        return 0;
    }
    episode->deadline_ms = now_ms + delay_ms;
    return 1;
}

/* Feed fresh xcy_system_ram_pressure_classify() results, not the diagnostic
 * PSI classifier. Below 30%, one PAGEOUT attempt comes first; after its 3s
 * feedback wait, only RAM below 12% can grant READY for one package kill.
 * A PAGEOUT requested in STRESS does not satisfy the CRITICAL prerequisite:
 * entering CRITICAL requests PAGEOUT and its own 3s feedback. If the kernel
 * accepted bytes and fresh RAM is still below 12% at that feedback, request
 * another round, up to four CRITICAL attempts in this episode. Accepted bytes
 * are not measured RAM release. Each round is bounded by the caller (128MiB).
 * No accepted bytes, a capacity skip or noncritical feedback stops the chain;
 * pressure fluctuations cannot replenish the round count or restart it.
 * At [12%,30%) the episode is quiet without polling, repeated PAGEOUT or a
 * replenished kill budget. Only RAM >= 30% ends the episode.
 *
 * RECLAIM_READY and READY are each issued once per pending action. The caller
 * independently validates whitelist/protection and PID identity, and selects
 * the eligible background package with the oldest BOOTTIME for a kill.
 * Complete actions synchronously. No external operation occurs in this file.
 *
 * The 30s gap applies between episodes, measured from the last PAGEOUT
 * attempt or successful kill. Actions within one episode use 3s feedback.
 * There is one global deadline and no per-application timer. */
static inline xcy_system_reclaim_decision_t xcy_system_reclaim_observe(
        xcy_system_reclaim_episode_t *episode,
        xcy_system_pressure_level_t pressure, xcy_reclaim_s64 now_ms) {
    if (!xcy_system_reclaim_clock_valid(episode, now_ms))
        return XCY_SYSTEM_RECLAIM_INVALID;
    if (pressure != XCY_SYSTEM_PRESSURE_UNKNOWN &&
        pressure != XCY_SYSTEM_PRESSURE_HEALTHY &&
        pressure != XCY_SYSTEM_PRESSURE_STRESS &&
        pressure != XCY_SYSTEM_PRESSURE_CRITICAL)
        return xcy_system_reclaim_fail_closed(episode);

    if (pressure == XCY_SYSTEM_PRESSURE_HEALTHY) {
        episode->phase = XCY_SYSTEM_RECLAIM_IDLE;
        episode->episode_actions = 0;
        episode->pending_pressure = XCY_SYSTEM_PRESSURE_HEALTHY;
        episode->critical_reclaim_handled = 0;
        episode->critical_reclaim_rounds = 0;
        episode->last_reclaim_succeeded = 0;
        episode->deadline_ms = 0;
        return XCY_SYSTEM_RECLAIM_NONE;
    }
    if (pressure == XCY_SYSTEM_PRESSURE_UNKNOWN) {
        /* Missing meminfo cannot prove recovery or replenish the budget.
         * Missing PSI does not reach this branch: it is not in the gate. */
        episode->phase = XCY_SYSTEM_RECLAIM_FALLBACK_LATCHED;
        episode->deadline_ms = 0;
        return XCY_SYSTEM_RECLAIM_FALLBACK;
    }

    if (episode->phase == XCY_SYSTEM_RECLAIM_FALLBACK_LATCHED)
        return XCY_SYSTEM_RECLAIM_FALLBACK;
    if (episode->phase == XCY_SYSTEM_RECLAIM_RECLAIM_PENDING)
        return XCY_SYSTEM_RECLAIM_NONE;
    if (episode->phase == XCY_SYSTEM_RECLAIM_IDLE) {
        xcy_reclaim_s64 gap_deadline;
        if (episode->action_seen) {
            if (episode->last_action_ms >
                    XCY_RECLAIM_TIME_MAX - XCY_SYSTEM_RECLAIM_EPISODE_GAP_MS)
                return xcy_system_reclaim_fail_closed(episode);
            gap_deadline = episode->last_action_ms +
                XCY_SYSTEM_RECLAIM_EPISODE_GAP_MS;
            if (gap_deadline > now_ms) {
                episode->deadline_ms = gap_deadline;
                episode->phase = XCY_SYSTEM_RECLAIM_RATE_WAIT;
                return XCY_SYSTEM_RECLAIM_WAIT;
            }
        }
        if (now_ms > XCY_RECLAIM_TIME_MAX - XCY_SYSTEM_RECLAIM_OBSERVE_MS)
            return xcy_system_reclaim_fail_closed(episode);
        episode->pending_pressure = pressure;
        episode->critical_reclaim_handled = 0;
        episode->critical_reclaim_rounds = 0;
        episode->last_reclaim_succeeded = 0;
        episode->phase = XCY_SYSTEM_RECLAIM_RECLAIM_PENDING;
        return XCY_SYSTEM_RECLAIM_RECLAIM_READY;
    }
    if (episode->phase == XCY_SYSTEM_RECLAIM_RATE_WAIT) {
        if (now_ms < episode->deadline_ms)
            return XCY_SYSTEM_RECLAIM_WAIT;
        if (now_ms > XCY_RECLAIM_TIME_MAX - XCY_SYSTEM_RECLAIM_OBSERVE_MS)
            return xcy_system_reclaim_fail_closed(episode);
        episode->deadline_ms = 0;
        episode->pending_pressure = pressure;
        /* RATE_WAIT also starts a new bounded round after caller cooldown. */
        episode->critical_reclaim_handled = 0;
        episode->critical_reclaim_rounds = 0;
        episode->last_reclaim_succeeded = 0;
        episode->phase = XCY_SYSTEM_RECLAIM_RECLAIM_PENDING;
        return XCY_SYSTEM_RECLAIM_RECLAIM_READY;
    }
    if ((episode->phase == XCY_SYSTEM_RECLAIM_RECLAIM_OBSERVING ||
         episode->phase == XCY_SYSTEM_RECLAIM_OBSERVING) &&
        now_ms < episode->deadline_ms)
        return XCY_SYSTEM_RECLAIM_WAIT;

    episode->deadline_ms = 0;
    if (pressure == XCY_SYSTEM_PRESSURE_STRESS) {
        episode->phase = XCY_SYSTEM_RECLAIM_LOW_IDLE;
        return XCY_SYSTEM_RECLAIM_NONE;
    }
    if (episode->phase == XCY_SYSTEM_RECLAIM_ACTION_PENDING)
        return XCY_SYSTEM_RECLAIM_NONE;
    if (episode->episode_actions >= XCY_SYSTEM_RECLAIM_MAX_ACTIONS) {
        episode->phase = XCY_SYSTEM_RECLAIM_FALLBACK_LATCHED;
        return XCY_SYSTEM_RECLAIM_FALLBACK;
    }
    if (now_ms > XCY_RECLAIM_TIME_MAX - XCY_SYSTEM_RECLAIM_OBSERVE_MS)
        return xcy_system_reclaim_fail_closed(episode);
    if (!episode->critical_reclaim_handled ||
        (episode->phase == XCY_SYSTEM_RECLAIM_RECLAIM_OBSERVING &&
         episode->last_reclaim_succeeded &&
         episode->critical_reclaim_rounds < XCY_SYSTEM_RECLAIM_MAX_CRITICAL_ROUNDS)) {
        episode->pending_pressure = pressure;
        episode->phase = XCY_SYSTEM_RECLAIM_RECLAIM_PENDING;
        return XCY_SYSTEM_RECLAIM_RECLAIM_READY;
    }
    episode->phase = XCY_SYSTEM_RECLAIM_ACTION_PENDING;
    return XCY_SYSTEM_RECLAIM_READY;
}

/* Complete the pending PAGEOUT attempt, including no candidates, partial
 * progress or failure. Every outcome gets the same 3s feedback wait; failure
 * is never permission to bypass PAGEOUT's wait and kill immediately.
 * Pass 1 only for accepted bytes > 0, not an estimate of actual RAM release;
 * pass 0 for no progress (including an executor's 0 or -2 result). */
static inline xcy_system_reclaim_decision_t xcy_system_reclaim_complete_reclaim(
        xcy_system_reclaim_episode_t *episode, xcy_reclaim_s64 now_ms,
        int action_succeeded) {
    if (!xcy_system_reclaim_clock_valid(episode, now_ms))
        return XCY_SYSTEM_RECLAIM_INVALID;
    if (episode->phase != XCY_SYSTEM_RECLAIM_RECLAIM_PENDING ||
        (episode->pending_pressure == XCY_SYSTEM_PRESSURE_CRITICAL &&
         episode->critical_reclaim_rounds >= XCY_SYSTEM_RECLAIM_MAX_CRITICAL_ROUNDS) ||
        (action_succeeded != 0 && action_succeeded != 1))
        return xcy_system_reclaim_fail_closed(episode);
    episode->action_seen = 1;
    episode->last_action_ms = now_ms;
    if (!xcy_system_reclaim_set_deadline(episode, now_ms,
                                        XCY_SYSTEM_RECLAIM_OBSERVE_MS))
        return XCY_SYSTEM_RECLAIM_INVALID;
    if (episode->pending_pressure == XCY_SYSTEM_PRESSURE_CRITICAL) {
        episode->critical_reclaim_handled = 1;
        ++episode->critical_reclaim_rounds;
    }
    episode->last_reclaim_succeeded = action_succeeded;
    episode->pending_pressure = XCY_SYSTEM_PRESSURE_HEALTHY;
    episode->phase = XCY_SYSTEM_RECLAIM_RECLAIM_OBSERVING;
    return XCY_SYSTEM_RECLAIM_WAIT;
}

/* Only use after the caller confirms zram cannot hold this pending PAGEOUT.
 * Feed a fresh RAM classification: CRITICAL may grant one kill immediately;
 * STRESS becomes quiet without a kill or a fake 3s PAGEOUT feedback timer.
 * No memory operation happened, so the last actual action time is preserved.
 * Capacity checks and all candidate protections remain the caller's duty. */
static inline xcy_system_reclaim_decision_t xcy_system_reclaim_skip_reclaim(
        xcy_system_reclaim_episode_t *episode,
        xcy_system_pressure_level_t pressure, xcy_reclaim_s64 now_ms) {
    if (!xcy_system_reclaim_clock_valid(episode, now_ms))
        return XCY_SYSTEM_RECLAIM_INVALID;
    if (episode->phase != XCY_SYSTEM_RECLAIM_RECLAIM_PENDING)
        return xcy_system_reclaim_fail_closed(episode);
    if (pressure != XCY_SYSTEM_PRESSURE_CRITICAL &&
        pressure != XCY_SYSTEM_PRESSURE_STRESS)
        return xcy_system_reclaim_observe(episode, pressure, now_ms);

    episode->pending_pressure = XCY_SYSTEM_PRESSURE_HEALTHY;
    episode->deadline_ms = 0;
    episode->last_reclaim_succeeded = 0;
    if (pressure == XCY_SYSTEM_PRESSURE_STRESS) {
        episode->phase = XCY_SYSTEM_RECLAIM_LOW_IDLE;
        return XCY_SYSTEM_RECLAIM_NONE;
    }
    if (episode->episode_actions >= XCY_SYSTEM_RECLAIM_MAX_ACTIONS) {
        episode->phase = XCY_SYSTEM_RECLAIM_FALLBACK_LATCHED;
        return XCY_SYSTEM_RECLAIM_FALLBACK;
    }
    if (now_ms > XCY_RECLAIM_TIME_MAX - XCY_SYSTEM_RECLAIM_OBSERVE_MS)
        return xcy_system_reclaim_fail_closed(episode);
    episode->critical_reclaim_handled = 1;
    episode->phase = XCY_SYSTEM_RECLAIM_ACTION_PENDING;
    return XCY_SYSTEM_RECLAIM_READY;
}

/* Proactive PAGEOUT is disabled. Keep the same RAM gate independent of
 * the available swap backing. */
static inline xcy_system_reclaim_decision_t xcy_reclaim_observe_without_pageout(
        xcy_system_reclaim_episode_t *episode,
        xcy_system_pressure_level_t pressure, xcy_reclaim_s64 now_ms)
{
    xcy_system_reclaim_decision_t decision =
        xcy_system_reclaim_observe(episode, pressure, now_ms);

    if (decision != XCY_SYSTEM_RECLAIM_RECLAIM_READY)
        return decision;
    return xcy_system_reclaim_skip_reclaim(episode, pressure, now_ms);
}

/* Complete exactly the most recent READY synchronously with its action.
 * Pass 1 once any signal was successfully sent for the selected package,
 * including a partially successful package action; pass 0 if no eligible
 * candidate or no successful signal exists. Failure falls back instead of
 * repeatedly seeking another candidate under the same severe pressure. */
static inline xcy_system_reclaim_decision_t xcy_system_reclaim_complete(
        xcy_system_reclaim_episode_t *episode, xcy_reclaim_s64 now_ms,
        int action_succeeded) {
    if (!xcy_system_reclaim_clock_valid(episode, now_ms))
        return XCY_SYSTEM_RECLAIM_INVALID;
    if (episode->phase != XCY_SYSTEM_RECLAIM_ACTION_PENDING ||
        (action_succeeded != 0 && action_succeeded != 1))
        return xcy_system_reclaim_fail_closed(episode);
    if (!action_succeeded) {
        episode->phase = XCY_SYSTEM_RECLAIM_FALLBACK_LATCHED;
        episode->deadline_ms = 0;
        return XCY_SYSTEM_RECLAIM_FALLBACK;
    }
    ++episode->episode_actions;
    episode->action_seen = 1;
    episode->last_action_ms = now_ms;
    if (!xcy_system_reclaim_set_deadline(episode, now_ms,
                                        XCY_SYSTEM_RECLAIM_OBSERVE_MS))
        return XCY_SYSTEM_RECLAIM_INVALID;
    episode->phase = XCY_SYSTEM_RECLAIM_OBSERVING;
    return XCY_SYSTEM_RECLAIM_WAIT;
}

/* Zero means no scheduled work. Timers only request a fresh observation;
 * reaching this time by itself never grants an action. */
static inline xcy_reclaim_s64 xcy_system_reclaim_deadline(
        const xcy_system_reclaim_episode_t *episode) {
    if (episode == NULL ||
        (episode->phase != XCY_SYSTEM_RECLAIM_RATE_WAIT &&
         episode->phase != XCY_SYSTEM_RECLAIM_RECLAIM_OBSERVING &&
         episode->phase != XCY_SYSTEM_RECLAIM_OBSERVING))
        return 0;
    return episode->deadline_ms;
}

/* One RAM feedback check follows a successful batch. A system still under
 * critical pressure can build another batch from fresh data. */
static inline xcy_system_reclaim_decision_t xcy_reclaim_complete_batch(
        xcy_system_reclaim_episode_t *episode, xcy_reclaim_s64 now_ms) {
    xcy_system_reclaim_decision_t decision =
        xcy_system_reclaim_complete(episode, now_ms, 1);

    if (decision == XCY_SYSTEM_RECLAIM_WAIT)
        episode->episode_actions = 0;
    return decision;
}

#endif
