#ifndef XCY_RECLAIM_POLICY_H
#define XCY_RECLAIM_POLICY_H

#include "xcy_reclaim_candidates.h"

struct xcy_reclaim_policy;
struct xcy_reclaim_protection_frame;

int xcy_reclaim_policy_register(void (*scene_changed)(const struct xcy_reclaim_protection_frame *frame));
void xcy_reclaim_policy_unregister(void);
int xcy_reclaim_policy_get(struct xcy_reclaim_protection *protection, u64 *grace_ms,
			  struct xcy_reclaim_policy **result);
bool xcy_reclaim_policy_current(const struct xcy_reclaim_policy *policy);
void xcy_reclaim_policy_put(struct xcy_reclaim_policy *policy);

#endif
