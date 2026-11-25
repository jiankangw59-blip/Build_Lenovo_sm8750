#ifndef XCY_RECLAIM_POLICY_H
#define XCY_RECLAIM_POLICY_H

#include "xcy_reclaim_candidates.h"

struct xcy_reclaim_policy;

int xcy_reclaim_policy_register(void);
void xcy_reclaim_policy_unregister(void);
int xcy_reclaim_policy_get(struct xcy_reclaim_protection *protection, u64 *grace_ms,
			  u64 *scene_valid_ms, struct xcy_reclaim_policy **result);
bool xcy_reclaim_policy_current(const struct xcy_reclaim_policy *policy);
void xcy_reclaim_policy_put(struct xcy_reclaim_policy *policy);

#endif
