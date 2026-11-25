#ifndef XCY_PROCESS_IMPORTANCE_POLICY_H
#define XCY_PROCESS_IMPORTANCE_POLICY_H

#include "xcy_reclaim_snapshot.h"

/* Local observation tiers, never Android importance/processState replacements. */
typedef enum {
    XCY_POLICY_P0_PROTECTED = 0,
    XCY_POLICY_P1_IMPORTANT = 1,
    XCY_POLICY_P2_TRANSIENT = 2,
    XCY_POLICY_P3_CACHED = 3,
    XCY_POLICY_P4_EMPTY = 4,
    XCY_POLICY_UNKNOWN = 5
} xcy_policy_tier_t;

#define XCY_POLICY_REASON_INPUT_INVALID       (XCY_RECLAIM_U64_C(1) << 0)
#define XCY_POLICY_REASON_IDENTITY_INVALID    (XCY_RECLAIM_U64_C(1) << 1)
#define XCY_POLICY_REASON_OWNER_UNKNOWN       (XCY_RECLAIM_U64_C(1) << 2)
#define XCY_POLICY_REASON_NOT_THIRD_PARTY      (XCY_RECLAIM_U64_C(1) << 3)
#define XCY_POLICY_REASON_SHARED_UID          (XCY_RECLAIM_U64_C(1) << 4)
#define XCY_POLICY_REASON_UNKNOWN_IMPORTANCE  (XCY_RECLAIM_U64_C(1) << 5)
#define XCY_POLICY_REASON_UNKNOWN_STATE       (XCY_RECLAIM_U64_C(1) << 6)
#define XCY_POLICY_REASON_GONE                (XCY_RECLAIM_U64_C(1) << 7)
#define XCY_POLICY_REASON_PERSISTENT_FLAG     (XCY_RECLAIM_U64_C(1) << 8)
#define XCY_POLICY_REASON_CANT_SAVE_FLAG      (XCY_RECLAIM_U64_C(1) << 9)
#define XCY_POLICY_REASON_IMPORTANCE_PROTECT  (XCY_RECLAIM_U64_C(1) << 10)
#define XCY_POLICY_REASON_STATE_PROTECT       (XCY_RECLAIM_U64_C(1) << 11)
#define XCY_POLICY_REASON_IMPORTANT_BUSINESS  (XCY_RECLAIM_U64_C(1) << 12)
#define XCY_POLICY_REASON_TRANSIENT           (XCY_RECLAIM_U64_C(1) << 13)
#define XCY_POLICY_REASON_CACHED              (XCY_RECLAIM_U64_C(1) << 14)
#define XCY_POLICY_REASON_EMPTY               (XCY_RECLAIM_U64_C(1) << 15)
#define XCY_POLICY_REASON_FIELDS_DISAGREE     (XCY_RECLAIM_U64_C(1) << 16)
#define XCY_POLICY_REASON_UNKNOWN_FLAGS       (XCY_RECLAIM_U64_C(1) << 17)
#define XCY_POLICY_REASON_DEPENDENCY_PRESENT  (XCY_RECLAIM_U64_C(1) << 18)
#define XCY_POLICY_REASON_DEPENDENCY_MISSING  (XCY_RECLAIM_U64_C(1) << 19)
#define XCY_POLICY_REASON_DEPENDENCY_INVALID  (XCY_RECLAIM_U64_C(1) << 20)
#define XCY_POLICY_REASON_DEPENDENCY_CYCLE    (XCY_RECLAIM_U64_C(1) << 21)
#define XCY_POLICY_REASON_DEPENDENCY_IMPORTANT (XCY_RECLAIM_U64_C(1) << 22)
#define XCY_POLICY_REASON_DEPENDENCY_UNKNOWN  (XCY_RECLAIM_U64_C(1) << 23)
#define XCY_POLICY_REASON_DEPENDENCY_FIELDS   (XCY_RECLAIM_U64_C(1) << 24)
#define XCY_POLICY_REASON_PACKAGE_EMPTY       (XCY_RECLAIM_U64_C(1) << 25)
#define XCY_POLICY_REASON_PACKAGE_MEMBER_HOLD (XCY_RECLAIM_U64_C(1) << 26)
#define XCY_POLICY_REASON_RESOURCE_FAILURE    (XCY_RECLAIM_U64_C(1) << 27)
#define XCY_POLICY_REASON_DEPENDENCY_SAFE     (XCY_RECLAIM_U64_C(1) << 28)
#define XCY_POLICY_REASON_UID_OWNER_CONFLICT  (XCY_RECLAIM_U64_C(1) << 29)

typedef struct {
    xcy_policy_tier_t tier;
    /* P3/P4 eligibility for later checks only. This module never reclaims. */
    int eligible;
    uint64_t reasons;
} xcy_policy_result_t;

typedef struct {
    uint64_t fingerprint;
    uint64_t first_ms;
    uint64_t last_ms;
    unsigned int samples;
} xcy_policy_confirmation_t;

/* Input must be one accepted, identity-validated native snapshot. No I/O, time,
 * PSS threshold, pressure policy, signal, field mutation, or global state exists
 * in these functions. Missing/ambiguous dependency clients cannot grant entry.
 * Known P0 evidence remains visible even alongside UNKNOWN reasons; those
 * reasons still veto eligibility at both PID and package levels. uid remains
 * the Framework owner UID; a verified isolated kernel_uid is another member
 * of that owner, never an exemption from native state or dependency checks. */
xcy_policy_result_t xcy_policy_process(const xcy_memory_snapshot_t *snapshot, size_t index);
/* Preferred observation API: builds/resolves the bounded graph once. Output
 * capacity must cover count; failure leaves the caller's output untouched. */
int xcy_policy_snapshot(const xcy_memory_snapshot_t *snapshot,
                        xcy_policy_result_t *results, size_t capacity);
xcy_policy_result_t xcy_policy_package(const xcy_memory_snapshot_t *snapshot,
                                      const char *pkg, uint32_t uid);
/* results must contain snapshot->count results from that exact snapshot. */
xcy_policy_result_t xcy_policy_package_results(const xcy_memory_snapshot_t *snapshot,
                                              const xcy_policy_result_t *results,
                                              const char *pkg, uint32_t uid);
/* PAGEOUT-only admission: a verified background P1 service may coexist with
 * eligible P3/P4 members. Only those eligible cached members may be selected;
 * this does not authorize reclaiming the P1 service or killing the package.
 * Visibility, temporary protection and allowlists remain execution checks. */
int xcy_policy_pageout_package_results(const xcy_memory_snapshot_t *snapshot,
                                       const xcy_policy_result_t *results,
                                       const char *pkg, uint32_t uid);
/* Kill admission no longer requires every member to be cached. Verified
 * ordinary-background P1/P2/P3/P4 members may coexist, including all-service
 * packages. P0, unknown state/ownership and non-third-party members still hold.
 * Pressure, PSS/age ordering and external protections remain caller checks. */
int xcy_policy_kill_package_results(const xcy_memory_snapshot_t *snapshot,
                                    const xcy_policy_result_t *results,
                                    const char *pkg, uint32_t uid);
const char *xcy_policy_tier_name(xcy_policy_tier_t tier);
/* Returns required bytes excluding NUL; a nonempty output is always terminated. */
size_t xcy_policy_format_reasons(uint64_t reasons, char *buf, size_t size);

/* Pure sampled confirmation: t0/t30/t60/t90, no timer or implicit polling.
 * fingerprint must cover PID generations and the original native state fields.
 * A false input, changed fingerprint, backwards clock, or gap over 60 seconds
 * starts a fresh series.
 * Return 1 after four distinct samples spanning at least 90 seconds; next_due
 * is then zero. Missed samples are never invented or caught up in one call. */
int xcy_policy_confirm_update(xcy_policy_confirmation_t *state, uint64_t fingerprint,
                             uint64_t now_ms, int overlimit_eligible,
                             uint64_t *next_due_ms);

#endif
