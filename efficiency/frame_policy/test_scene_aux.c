// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "scene_aux_plan.h"

static void plan(const int initial[3], const int wanted[3])
{
	struct fp_aux_step steps[4];
	int actual[3];
	u32 i, n = fp_aux_core_plan(initial, wanted, steps);

	memcpy(actual, initial, sizeof(actual));
	assert(n <= 4);
	for (i = 0; i < n; i++) {
		u32 slot = steps[i].knob - FP_AUX_KNOB_CORE_ENABLE;

		assert(slot < 3 && actual[slot] != steps[i].value);
		actual[slot] = steps[i].value;
		assert(actual[1] <= actual[2]);
	}
	assert(!memcmp(actual, wanted, sizeof(actual)));
}

int main(void)
{
	struct fp_scene_aux_table *table = calloc(1, sizeof(*table));
	struct fp_scene_aux_cpu *cpu;
	u32 i, j;
	int enable, minimum, maximum, next_enable, next_minimum, next_maximum;

	assert(table);
	table->abi = FP_SCENE_AUX_ABI;
	for (i = 0; i < FP_MAX_CPUS; i++)
		for (j = 0; j < FP_SCENE_AUX_KNOBS; j++) table->fds[i][j] = -1;
	assert(fp_scene_aux_table_valid(table, false));
	assert(fp_scene_aux_table_valid(table, true));
	table->cpu_count = 2; table->cpu_ids[1] = 6;
	table->profile_count = 3; table->defaults[1] = 1; table->defaults[2] = 2;
	cpu = &table->profiles[0].cpus[0];
	cpu->valid_mask = FP_AUX_UP_RATE | FP_AUX_HISPEED | FP_AUX_BOOST;
	cpu->up_us = 1000; cpu->hispeed_khz = 0; cpu->walt_boost = -2;
	cpu->feedback_mask = FP_AUX_FEEDBACK_EXCLUDES | FP_AUX_FEEDBACK_PREFER;
	cpu->excludes_mask = 1; cpu->prefer = 1;
	table->profiles[1] = table->profiles[0];
	table->profiles[1].cpus[0].walt_boost = -5;
	assert(fp_scene_aux_table_valid(table, false));
	assert(!fp_scene_aux_table_valid(table, true));
	table->expected_scene_epoch = 1;
	table->fds[0][0] = 10; table->fds[0][2] = 11; table->fds[0][3] = 12;
	assert(fp_scene_aux_table_valid(table, true));
	assert(fp_scene_aux_select(table, "test.app", false, false, false) == &table->profiles[0]);
	assert(fp_scene_aux_select(table, "test.app", false, false, true) == &table->profiles[1]);
	assert(!fp_scene_aux_select(table, "test.app", true, false, false));
	assert(fp_scene_aux_select(table, "test.app", true, true, false) == &table->profiles[2]);
	table->profile_count = 33; assert(!fp_scene_aux_table_valid(table, true)); table->profile_count = 3;
	cpu->walt_boost = -101; assert(!fp_scene_aux_table_valid(table, true)); cpu->walt_boost = -2;
	cpu->feedback_mask |= FP_AUX_FEEDBACK_CORE_CTL; cpu->limiter_cluster_count = 2;
	cpu->limiter_clusters_packed = 0x100;
	assert(!fp_scene_aux_table_valid(table, true)); /* global list must match every CPU */
	table->profiles[0].cpus[1].feedback_mask = FP_AUX_FEEDBACK_CORE_CTL;
	table->profiles[0].cpus[1].limiter_cluster_count = 2;
	table->profiles[0].cpus[1].limiter_clusters_packed = 0x100;
	table->options.source = FP_AUX_SOURCE_ONLINE;
	table->options.valid_mask = FP_AUX_OPT_SOURCE;
	assert(!fp_scene_aux_table_valid(table, true)); /* feedback also needs complete core leases */
	for (i = 0; i < 2; i++)
		for (j = FP_AUX_KNOB_CORE_ENABLE; j < FP_SCENE_AUX_KNOBS; j++)
			table->fds[i][j] = 20 + i * FP_SCENE_AUX_KNOBS + j;
	assert(fp_scene_aux_table_valid(table, true));
	cpu->limiter_clusters_packed = 0; assert(!fp_scene_aux_table_valid(table, true));
	cpu->limiter_clusters_packed = 0x100;
	table->options.source = FP_AUX_SOURCE_ONLINE;
	table->options.valid_mask = 0;
	assert(!fp_scene_aux_table_valid(table, true)); /* unknown is not a supplied default */
	table->options.valid_mask = FP_AUX_OPT_SOURCE;
	assert(fp_scene_aux_table_valid(table, true));
	table->options.valid_mask |= FP_AUX_OPT_PANEL_PEAK;
	assert(!fp_scene_aux_table_valid(table, true));
	table->options.panel_peak_hz = 185;
	assert(fp_scene_aux_table_valid(table, true));
	cpu->feedback_mask |= FP_AUX_FEEDBACK_PERFECT | FP_AUX_FEEDBACK_SMOOTHNESS |
		FP_AUX_FEEDBACK_MT | FP_AUX_FEEDBACK_ADDITIVE;
	table->options.valid_mask |= FP_AUX_OPT_STAT_SOURCE;
	table->options.stat_source = 1;
	cpu->additive_count = 2;
	cpu->additive[0] = (struct fp_scene_aux_additive){0, 150000};
	cpu->additive[1] = (struct fp_scene_aux_additive){1152000, 100000};
	assert(fp_scene_aux_table_valid(table, true));
	cpu->mt_milli_percent = 100001; assert(!fp_scene_aux_table_valid(table, true));
	cpu->mt_milli_percent = 0;
	cpu->additive[1].threshold_khz = 0; assert(!fp_scene_aux_table_valid(table, true));
	plan((int[]){0, 2, 2}, (int[]){1, 0, 2});
	plan((int[]){1, 0, 2}, (int[]){0, 2, 2});
	plan((int[]){1, 2, 2}, (int[]){1, 0, 0});
	plan((int[]){0, 0, 0}, (int[]){0, 2, 2});
	plan((int[]){0, 2, 2}, (int[]){0, 2, 2});
	/* Independently check every legal six-core bounds transition; this checks
	 * the observable safety/final state, not a copy of the planner algorithm.
	 */
	for (enable = 0; enable < 2; enable++)
		for (minimum = 0; minimum <= 6; minimum++)
			for (maximum = minimum; maximum <= 6; maximum++)
				for (next_enable = 0; next_enable < 2; next_enable++)
					for (next_minimum = 0; next_minimum <= 6; next_minimum++)
						for (next_maximum = next_minimum; next_maximum <= 6; next_maximum++)
							plan((int[]){enable, minimum, maximum},
							     (int[]){next_enable, next_minimum, next_maximum});
	free(table);
	puts("Scene aux: fixed ABI, optional clear, range/metadata, FAS/off selection and safe core transitions passed");
	return 0;
}
