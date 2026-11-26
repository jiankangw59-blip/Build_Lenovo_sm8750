/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_GAME_CONSTRAINTS_H
#define FRAME_GAME_CONSTRAINTS_H

#ifndef __KERNEL__
#include <stdbool.h>
#endif
#include "frame_control.h"

/* A four-policy dependency graph is bounded; no allocation or recursion. */
static inline bool fp_relation_order(const struct fp_relation *relations, u32 count,
				     u32 order[FP_MAX_CPUS])
{
	u32 complete = 0, used = 0, i;

	if (!count || count > FP_MAX_CPUS)
		return false;
	for (i = 0; i < count; i++) {
		const struct fp_relation *relation = &relations[i];

		if (relation->kind > FP_REL_POLICY ||
		    relation->minimum_khz < -10000000 || relation->maximum_khz > 10000000 ||
		    relation->minimum_khz > relation->maximum_khz ||
		    (relation->kind == FP_REL_POLICY && relation->reference >= count) ||
		    (relation->kind == FP_REL_ABSOLUTE && relation->minimum_khz < 0) ||
		    (relation->kind != FP_REL_POLICY && relation->reference) ||
		    (relation->kind == FP_REL_NONE && (relation->minimum_khz || relation->maximum_khz)))
			return false;
	}
	while (complete < count) {
		u32 before = complete;

		for (i = 0; i < count; i++) {
			if ((used & (1U << i)) || (relations[i].kind == FP_REL_POLICY &&
			    !(used & (1U << relations[i].reference))))
				continue;
			order[complete++] = i;
			used |= 1U << i;
		}
		if (before == complete)
			return false;
	}
	return true;
}

static inline bool fp_apply_relations(u32 targets[FP_MAX_CPUS],
				      const struct fp_limits limits[FP_MAX_CPUS],
				      const struct fp_relation relations[FP_MAX_CPUS], u32 count)
{
	u32 order[FP_MAX_CPUS], j;

	if (!fp_relation_order(relations, count, order))
		return false;
	for (j = 0; j < count; j++) {
		u32 i = order[j];
		__s64 value = targets[i];
		__s64 base = relations[i].kind == FP_REL_POLICY ? targets[relations[i].reference] : 0;

		if (relations[i].kind) {
			__s64 minimum = base + relations[i].minimum_khz;
			__s64 maximum = base + relations[i].maximum_khz;

			value = value < minimum ? minimum : value > maximum ? maximum : value;
		}
		/* Hardware and configured safety limits always retain priority. */
		value = value < limits[i].min_khz ? limits[i].min_khz :
			value > limits[i].max_khz ? limits[i].max_khz : value;
		targets[i] = value;
	}
	return true;
}

#endif
