/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_AUX_POLICY_H
#define FRAME_SCENE_AUX_POLICY_H

#include "scene_policy.h"
#include "scene_aux_uapi.h"

#define FP_AUX_RATE_MAX_US 1000000U
#define FP_AUX_HISPEED_MAX_KHZ 10000000U
#define FP_AUX_BOOST_MIN (-100)
#define FP_AUX_BOOST_MAX 100
#define FP_AUX_ADDITIVE_MAX_KHZ 10000000

static inline bool fp_scene_aux_options_valid(const struct fp_scene_aux_options *options)
{
	u32 values[] = {options->source, options->stat_source, options->gesture_boost,
		options->gesture_duration_ms, options->launcher_boost, options->perf_notify,
		options->perf_notify_in_apps, options->perf_use_presets, options->kswapd_boost,
		options->panel_peak_hz, options->gpu_lock};
	u32 i;

	if ((options->valid_mask & ~FP_AUX_OPTIONS_MASK) ||
	    options->source > FP_AUX_SOURCE_CUSTOM ||
	    options->stat_source > 1 || options->gesture_boost > 1 ||
	    options->gesture_duration_ms > 60000 || options->launcher_boost > 1 ||
	    options->perf_notify > 1 || options->perf_notify_in_apps > 1 ||
	    options->perf_use_presets > 1 || options->kswapd_boost > 1 ||
	    options->panel_peak_hz > 1000 || options->gpu_lock > 1 ||
	    ((options->valid_mask & FP_AUX_OPT_PANEL_PEAK) && !options->panel_peak_hz))
		return false;
	/* These event providers are explicitly unsupported in this version.
	 * Reject at client preflight too, before any legacy owner is stopped.
	 * Current audited configuration has both disabled.
	 */
	if (options->launcher_boost || (options->perf_notify && options->perf_notify_in_apps) ||
	    (options->perf_notify && options->kswapd_boost &&
	     !(options->valid_mask & FP_AUX_OPT_PANEL_PEAK)))
		return false;
	/* These fields deliberately match their validity bit order in the ABI. */
	for (i = 0; i < 11; i++)
		if (!(options->valid_mask & (1U << i)) && values[i])
			return false;
	return true;
}

static inline bool fp_aux_zero(const void *data, unsigned long size)
{
	const unsigned char *bytes = data;
	unsigned long i;

	for (i = 0; i < size; i++)
		if (bytes[i])
			return false;
	return true;
}

static inline bool fp_scene_aux_cpu_valid(const struct fp_scene_aux_cpu *cpu, u32 cpu_count)
{
	u32 i, used = 0;

	if ((cpu->valid_mask & ~FP_AUX_DIRECT_MASK) ||
	    (cpu->feedback_mask & ~FP_AUX_FEEDBACK_MASK) ||
	    cpu->up_us > FP_AUX_RATE_MAX_US || cpu->down_us > FP_AUX_RATE_MAX_US ||
	    cpu->hispeed_khz > FP_AUX_HISPEED_MAX_KHZ ||
	    cpu->walt_boost < FP_AUX_BOOST_MIN || cpu->walt_boost > FP_AUX_BOOST_MAX ||
	    cpu->core_enable > 1 || cpu->min_cpus > cpu->max_cpus || cpu->max_cpus > 64 ||
	    cpu->prefer > 3 || cpu->limiter_cluster_count > cpu_count)
		return false;
	if ((!(cpu->valid_mask & FP_AUX_UP_RATE) && cpu->up_us) ||
	    (!(cpu->valid_mask & FP_AUX_DOWN_RATE) && cpu->down_us) ||
	    (!(cpu->valid_mask & FP_AUX_HISPEED) && cpu->hispeed_khz) ||
	    (!(cpu->valid_mask & FP_AUX_BOOST) && cpu->walt_boost) ||
	    (!(cpu->valid_mask & FP_AUX_CORE_CTL) &&
	     (cpu->core_enable || cpu->min_cpus || cpu->max_cpus)) ||
	    (!(cpu->feedback_mask & FP_AUX_FEEDBACK_EXCLUDES) && cpu->excludes_mask) ||
	    (!(cpu->feedback_mask & FP_AUX_FEEDBACK_PREFER) && cpu->prefer) ||
	    (!(cpu->feedback_mask & FP_AUX_FEEDBACK_CORE_CTL) &&
	     (cpu->limiter_cluster_count || cpu->limiter_clusters_packed)))
		return false;
	for (i = 0; i < cpu->limiter_cluster_count; i++) {
		u32 cluster = (cpu->limiter_clusters_packed >> (8 * i)) & 255;

		if (cluster >= cpu_count || (used & (1U << cluster)))
			return false;
		used |= 1U << cluster;
	}
	if (cpu->limiter_cluster_count < 4 &&
	    (cpu->limiter_clusters_packed >> (8 * cpu->limiter_cluster_count)))
		return false;
	if (cpu->perfect_khz > FP_AUX_HISPEED_MAX_KHZ || cpu->smoothness > 32 ||
	    cpu->mt_milli_percent > 100000 || cpu->additive_count > FP_MAX_MARGINS ||
	    (!(cpu->feedback_mask & FP_AUX_FEEDBACK_PERFECT) && cpu->perfect_khz) ||
	    (!(cpu->feedback_mask & FP_AUX_FEEDBACK_SMOOTHNESS) && cpu->smoothness) ||
	    (!(cpu->feedback_mask & FP_AUX_FEEDBACK_MT) && cpu->mt_milli_percent) ||
	    (!(cpu->feedback_mask & FP_AUX_FEEDBACK_ADDITIVE) && cpu->additive_count))
		return false;
	for (i = 0; i < FP_MAX_MARGINS; i++) {
		const struct fp_scene_aux_additive *step = &cpu->additive[i];

		if (i >= cpu->additive_count) {
			if (step->threshold_khz || step->offset_khz)
				return false;
		} else if (step->threshold_khz > FP_AUX_HISPEED_MAX_KHZ ||
			   step->offset_khz < -FP_AUX_ADDITIVE_MAX_KHZ ||
			   step->offset_khz > FP_AUX_ADDITIVE_MAX_KHZ ||
			   (!i && step->threshold_khz) ||
			   (i && step->threshold_khz <= cpu->additive[i - 1].threshold_khz)) {
			return false;
		}
	}
	return true;
}

static inline u32 fp_scene_aux_required(const struct fp_scene_aux_table *table, u32 cpu)
{
	u32 i, mask = 0;

	for (i = 0; i < table->profile_count; i++) {
		mask |= table->profiles[i].cpus[cpu].valid_mask;
		/* The original force branch restores ALL physical clusters. A
		 * dynamic ordered list therefore needs a complete core_ctl lease
		 * group for every policy, even without a direct core alias there.
		 */
		if (table->profiles[i].cpus[0].limiter_cluster_count)
			mask |= FP_AUX_CORE_CTL;
	}
	return mask;
}

static inline u32 fp_scene_aux_knob_bit(u32 knob)
{
	return knob < FP_AUX_KNOB_CORE_ENABLE ? 1U << knob : FP_AUX_CORE_CTL;
}

/* Shared by the kernel and client. Runtime FDs/epoch are never disk policy. */
static inline bool fp_scene_aux_table_valid(const struct fp_scene_aux_table *table, bool runtime)
{
	u32 i, j;
	bool empty = !table->profile_count;

	if (table->abi != FP_SCENE_AUX_ABI || table->reserved ||
	    !fp_scene_aux_options_valid(&table->options) ||
	    table->count > FP_SCENE_MAX_RULES || table->cpu_count > FP_MAX_CPUS ||
	    table->profile_count > FP_SCENE_AUX_MAX_PROFILES ||
	    (empty && (table->count || table->cpu_count || table->expected_scene_epoch)) ||
	    (!empty && (!table->cpu_count || (runtime && !table->expected_scene_epoch))) ||
	    (!runtime && table->expected_scene_epoch))
		return false;
	if (empty && !fp_aux_zero(&table->options, sizeof(table->options)))
		return false;
	for (i = 0; i < FP_SCENE_STATES; i++)
		if (empty ? table->defaults[i] != 0 : table->defaults[i] >= table->profile_count)
			return false;
	for (i = 0; i < FP_MAX_CPUS; i++) {
		u32 required = i < table->cpu_count ? fp_scene_aux_required(table, i) : 0;

		if (i >= table->cpu_count && table->cpu_ids[i])
			return false;
		for (j = 0; j < i && i < table->cpu_count; j++)
			if (table->cpu_ids[i] == table->cpu_ids[j])
				return false;
		for (j = 0; j < FP_SCENE_AUX_KNOBS; j++) {
			bool needed = required & fp_scene_aux_knob_bit(j);

			if (runtime && needed ? table->fds[i][j] < 0 : table->fds[i][j] != -1)
				return false;
		}
	}
	for (i = 0; i < FP_SCENE_AUX_MAX_PROFILES; i++)
		for (j = 0; j < FP_MAX_CPUS; j++) {
			const struct fp_scene_aux_cpu *cpu = &table->profiles[i].cpus[j];

			if (i < table->profile_count && j < table->cpu_count) {
				u32 needed = FP_AUX_FEEDBACK_PERFECT | FP_AUX_FEEDBACK_SMOOTHNESS |
					FP_AUX_FEEDBACK_MT | FP_AUX_FEEDBACK_ADDITIVE;

				if (!fp_scene_aux_cpu_valid(cpu, table->cpu_count))
					return false;
				if (cpu->additive_count &&
				    ((cpu->feedback_mask & needed) != needed ||
				     !(table->options.valid_mask & FP_AUX_OPT_STAT_SOURCE)))
					return false;
				/* This original limiter list is profile-global, repeated in each
				 * CPU record for the fixed layout. Never accept divergent lists.
				 */
				if (j) {
					const struct fp_scene_aux_cpu *first = &table->profiles[i].cpus[0];

					if (((cpu->feedback_mask ^ first->feedback_mask) & FP_AUX_FEEDBACK_CORE_CTL) ||
					    cpu->limiter_cluster_count != first->limiter_cluster_count ||
					    cpu->limiter_clusters_packed != first->limiter_clusters_packed)
						return false;
				}
			} else if (!fp_aux_zero(cpu, sizeof(*cpu))) {
				return false;
			}
		}
	for (i = 0; i < FP_SCENE_MAX_RULES; i++) {
		const struct fp_scene_rule *rule = &table->rules[i];

		if (i >= table->count) {
			if (!fp_aux_zero(rule, sizeof(*rule)))
				return false;
			continue;
		}
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

static inline const struct fp_scene_aux_profile *fp_scene_aux_select(
	const struct fp_scene_aux_table *table, const char *package, bool game, bool off, bool inactive)
{
	const __u32 *indices;
	u32 i, state = off ? FP_SCENE_OFF : inactive ? FP_SCENE_INACTIVE : FP_SCENE_ACTIVE;

	if (!table || !table->profile_count || !package || !*package || (game && !off))
		return NULL;
	indices = table->defaults;
	for (i = 0; i < table->count; i++)
		if (!strcmp(table->rules[i].package, package)) {
			indices = table->rules[i].profiles;
			break;
		}
	return &table->profiles[indices[state]];
}

#endif
