/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SCENE_ACTIVITY_PROFILES_H
#define SCENE_ACTIVITY_PROFILES_H
#include "scene_policy.h"

/* Ignore differences in disabled axes. NULL is an explicit ignored state. */
static inline bool fp_scene_activity_needed(const struct fp_scene_profile *active,
		const struct fp_scene_profile *inactive, u32 flags, u32 cpu_count)
{
	u32 i;

	if (!(flags & (FP_CPU | FP_GPU)) || active == inactive)
		return false;
	if (!active || !inactive)
		return true;
	if ((flags & FP_GPU) && (active->gpu_percent != inactive->gpu_percent ||
	    memcmp(&active->gpu, &inactive->gpu, sizeof(active->gpu))))
		return true;
	if (flags & FP_CPU)
		for (i = 0; i < cpu_count; i++)
			if (memcmp(&active->cpus[i], &inactive->cpus[i], sizeof(active->cpus[i])))
				return true;
	return false;
}
#endif
