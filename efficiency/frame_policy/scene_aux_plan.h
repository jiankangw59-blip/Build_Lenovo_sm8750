/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_AUX_PLAN_H
#define FRAME_SCENE_AUX_PLAN_H
#include "scene_aux_policy.h"

struct fp_aux_step { u32 knob; int value; };

/* Original all-on/auto action ordering, with every intermediate min<=max.
 * Exact OnePlus stores clamp each bound independently, without fixing peers.
 * Caller already validates hardware counts. The plan contains no equal writes.
 */
static inline u32 fp_aux_core_plan(const int old[3], const int next[3],
				   struct fp_aux_step steps[4])
{
	u32 n = 0, first, second, i;

	if (!memcmp(old, next, sizeof(int) * 3))
		return 0;
	if (!old[0])
		steps[n++] = (struct fp_aux_step){FP_AUX_KNOB_CORE_ENABLE, 1};
	first = next[2] < old[1] ? 1 : 2;
	second = 3 - first;
	for (i = 0; i < 2; i++) {
		u32 slot = i ? second : first;

		if (old[slot] != next[slot])
			steps[n++] = (struct fp_aux_step){FP_AUX_KNOB_CORE_ENABLE + slot, next[slot]};
	}
	if (!next[0])
		steps[n++] = (struct fp_aux_step){FP_AUX_KNOB_CORE_ENABLE, 0};
	return n;
}
#endif
