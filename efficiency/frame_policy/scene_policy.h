/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_POLICY_H
#define FRAME_SCENE_POLICY_H

#ifdef __KERNEL__
#include <linux/string.h>
#else
#include <stdbool.h>
#include <string.h>
#endif
#include "frame_control.h"
#include "scene_policy_uapi.h"

static inline bool fp_scene_limits_valid(const struct fp_limits *limits)
{
	return limits->min_khz <= limits->max_khz && limits->max_khz &&
		limits->max_khz <= 10000000U;
}

static inline bool fp_scene_name_valid(const char package[128])
{
	unsigned int i;

	if (!package[0])
		return false;
	for (i = 0; i < 128; i++) {
		unsigned char c = package[i];

		if (!c)
			return true;
		if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		      (c >= '0' && c <= '9') || c == '_' || c == '.'))
			return false;
	}
	return false;
}

/* Called on a private copy before publishing the table under config_lock. */
static inline bool fp_scene_table_valid(const struct fp_scene_table *table)
{
	u32 i, j, k;

	if (table->abi != FP_SCENE_ABI || table->reserved ||
	    !table->cpu_count || table->cpu_count > FP_MAX_CPUS ||
	    !table->profile_count || table->profile_count > FP_SCENE_MAX_PROFILES ||
	    table->count > FP_SCENE_MAX_RULES)
		return false;
	for (i = 0; i < FP_SCENE_STATES; i++)
		if (table->defaults[i] >= table->profile_count)
			return false;
	for (i = 0; i < table->cpu_count; i++)
		for (j = 0; j < i; j++)
			if (table->cpu_ids[i] == table->cpu_ids[j])
				return false;
	for (i = 0; i < table->profile_count; i++) {
		const struct fp_scene_profile *profile = &table->profiles[i];

		if (profile->flags & ~FP_SCENE_ENABLED || profile->gpu_percent > 200)
			return false;
		if (!(profile->flags & FP_SCENE_ENABLED)) {
			struct fp_scene_profile empty = {};

			if (memcmp(profile, &empty, sizeof(empty)))
				return false;
			continue;
		}
		if (!fp_scene_limits_valid(&profile->gpu))
			return false;
		for (j = 0; j < table->cpu_count; j++) {
			const struct fp_scene_cpu *cpu = &profile->cpus[j];

			if (!fp_scene_limits_valid(&cpu->limits) || cpu->reserved ||
			    cpu->margin_count > FP_MAX_MARGINS)
				return false;
			for (k = 0; k < cpu->margin_count; k++)
				if (!cpu->margins[k].percent || cpu->margins[k].percent > 1000 ||
				    (!k && cpu->margins[k].threshold_khz) ||
				    (k && cpu->margins[k].threshold_khz <=
					  cpu->margins[k - 1].threshold_khz))
					return false;
		}
	}
	for (i = 0; i < table->count; i++) {
		const struct fp_scene_rule *rule = &table->rules[i];

		if (!fp_scene_name_valid(rule->package))
			return false;
		for (j = 0; j < FP_SCENE_STATES; j++)
			if (rule->profiles[j] >= table->profile_count)
				return false;
		for (j = 0; j < i; j++)
			if (!strcmp(rule->package, table->rules[j].package))
				return false;
	}
	return true;
}

static inline bool fp_scene_topology_matches(const struct fp_scene_table *table,
					    const struct fp_config *config)
{
	u32 i;

	if (table->cpu_count != config->cpu_count)
		return false;
	for (i = 0; i < table->cpu_count; i++)
		if (table->cpu_ids[i] != config->cpus[i].cpu)
			return false;
	return true;
}

/* This runs only at a foreground/state event, never from sched_switch. */
static inline const struct fp_scene_profile *fp_scene_select(
	const struct fp_scene_table *table, const char *package,
	bool game, bool off, bool inactive)
{
	u32 i, state = off ? FP_SCENE_OFF :
		inactive ? FP_SCENE_INACTIVE : FP_SCENE_ACTIVE;
	const __u32 *indices;
	const struct fp_scene_profile *profile;

	if (!table || !package || !package[0] || (game && !off))
		return NULL;
	indices = table->defaults;
	for (i = 0; i < table->count; i++)
		if (!strcmp(package, table->rules[i].package)) {
			indices = table->rules[i].profiles;
			break;
		}
	profile = &table->profiles[indices[state]];
	return profile->flags & FP_SCENE_ENABLED ? profile : NULL;
}

static inline bool fp_scene_uses_feedback(const struct fp_scene_profile *profile,
					 u32 cpu_count)
{
	u32 i;

	if (!profile)
		return false;
	for (i = 0; i < cpu_count; i++)
		if (profile->cpus[i].margin_count)
			return true;
	return false;
}

static inline u32 fp_scene_profile_ceiling(u32 effective_khz,
					 const struct fp_scene_cpu *cpu)
{
	u32 i, margin;
	u64 ceiling;

	if (!cpu->margin_count)
		return cpu->limits.max_khz;
	margin = cpu->margins[0].percent;
	for (i = 1; i < cpu->margin_count; i++) {
		if (effective_khz < cpu->margins[i].threshold_khz)
			break;
		margin = cpu->margins[i].percent;
	}
	ceiling = (u64)effective_khz * margin / 100;
	return ceiling < cpu->limits.min_khz ? cpu->limits.min_khz :
		ceiling > cpu->limits.max_khz ? cpu->limits.max_khz : ceiling;
}

#endif
