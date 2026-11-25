#ifndef XCY_RECLAIM_IDENTITY_H
#define XCY_RECLAIM_IDENTITY_H

#include "xcy_reclaim_snapshot.h"

struct pid;

/* The returned PID is pinned until the caller releases it with put_pid(). */
struct pid *xcy_reclaim_pin_identity(const xcy_memory_process_t *record);
int xcy_reclaim_validate_identity(const xcy_memory_process_t *record, void *unused);
bool xcy_reclaim_identity_current(struct pid *pid, const xcy_memory_process_t *record);

#endif
