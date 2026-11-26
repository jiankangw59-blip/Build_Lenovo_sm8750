/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SCENE_ACTIVITY_LOAD_H
#define SCENE_ACTIVITY_LOAD_H
#include "scene_activity_policy.h"

struct sa_cpu_counter {
	sa_u64 total, idle;
	bool online;
};

/* A counter reset/hotplug is unknown, never an apparent 0% busy sample. */
static inline bool sa_counter_delta(const struct sa_cpu_counter *old,
		const struct sa_cpu_counter *now, sa_u64 *total, sa_u64 *idle)
{
	if (!old->online || !now->online || now->total <= old->total ||
	    now->idle < old->idle)
		return false;
	*total = now->total - old->total;
	*idle = now->idle - old->idle;
	return *idle <= *total;
}

static inline sa_u32 sa_busy_permille(sa_u64 total, sa_u64 idle)
{
	sa_u64 busy = total - idle;
	/* Normal windows are seconds. The alternate path also bounds huge tests. */
	sa_u64 value = total <= ~(sa_u64)0 / 1000 ? busy * 1000 / total :
		busy / (total / 1000);
	return value > 1000 ? 1000 : (sa_u32)value;
}
#endif
