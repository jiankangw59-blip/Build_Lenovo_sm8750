#ifndef XCY_RECLAIM_IDENTITY_H
#define XCY_RECLAIM_IDENTITY_H

#include "xcy_reclaim_snapshot.h"

struct pid;
struct xcy_reclaim_member;

/* The returned PID is pinned until the caller releases it with put_pid(). */
struct pid *xcy_reclaim_pin_identity(const xcy_memory_process_t *record);
int xcy_reclaim_validate_identity(const xcy_memory_process_t *record, void *unused);
struct pid *xcy_reclaim_pin_member(const struct xcy_reclaim_member *member);
bool xcy_reclaim_member_current(struct pid *pid, const struct xcy_reclaim_member *member);

#endif
