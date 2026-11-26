// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include "scene_policy.h"

int main(int argc, char **argv)
{
	struct fp_scene_table *table = calloc(1, sizeof(*table));
	struct fp_scene_profile *profile;
	u32 i;

	assert(table);
	assert(sizeof(struct fp_scene_cpu) == 80);
	assert(sizeof(struct fp_scene_profile) == 336);
	assert(sizeof(*table) == 14384);
	assert(offsetof(struct fp_scene_table, profiles) == 48);
	assert(offsetof(struct fp_scene_table, rules) == 5424);
	if (argc == 2) {
		FILE *file = fopen(argv[1], "rb");

		assert(file);
		assert(fread(table, 1, sizeof(*table), file) == sizeof(*table));
		assert(fgetc(file) == EOF);
		fclose(file);
		assert(fp_scene_table_valid(table));
		assert(!fp_scene_select(table, "com.test", true, false, false));
		puts("Scene imported table: valid ABI, topology and game exclusion");
		free(table);
		return 0;
	}
	table->abi = FP_SCENE_ABI;
	table->cpu_count = 2;
	table->cpu_ids[0] = 0;
	table->cpu_ids[1] = 6;
	table->profile_count = 3;
	table->defaults[FP_SCENE_ACTIVE] = 0;
	table->defaults[FP_SCENE_INACTIVE] = 1;
	table->defaults[FP_SCENE_OFF] = 2;
	for (i = 0; i < 2; i++) {
		profile = &table->profiles[i];
		profile->flags = FP_SCENE_ENABLED;
		profile->gpu = (struct fp_limits){330000, 1083000};
		profile->gpu_percent = 105;
		profile->cpus[0].limits = (struct fp_limits){500000, 2400000};
		profile->cpus[1].limits = (struct fp_limits){1000000, 4000000};
	}
	profile = &table->profiles[0];
	profile->cpus[0].margin_count = 2;
	profile->cpus[0].margins[0].percent = 200;
	profile->cpus[0].margins[1].threshold_khz = 1500000;
	profile->cpus[0].margins[1].percent = 90;
	assert(fp_scene_table_valid(table));
	assert(fp_scene_select(table, "com.test", false, false, false) == profile);
	assert(fp_scene_select(table, "com.test", false, false, true) == &table->profiles[1]);
	assert(!fp_scene_select(table, "com.test", false, true, false));
	assert(!fp_scene_select(table, "com.test", true, false, false));
	assert(fp_scene_uses_feedback(profile, 2));
	assert(!fp_scene_uses_feedback(&table->profiles[1], 2));
	assert(fp_scene_profile_ceiling(0, &profile->cpus[0]) == 500000);
	assert(fp_scene_profile_ceiling(1000000, &profile->cpus[0]) == 2000000);
	assert(fp_scene_profile_ceiling(1600000, &profile->cpus[0]) == 1440000);
	assert(fp_scene_profile_ceiling(10000000, &profile->cpus[0]) == 2400000);
	assert(fp_scene_profile_ceiling(0, &table->profiles[1].cpus[0]) == 2400000);
	table->count = 1;
	strcpy(table->rules[0].package, "com.test.camera");
	table->rules[0].profiles[0] = 1;
	assert(fp_scene_select(table, "com.test.camera", false, false, false) == &table->profiles[1]);
	assert(fp_scene_table_valid(table));
	table->rules[0].profiles[0] = 3;
	assert(!fp_scene_table_valid(table));
	table->rules[0].profiles[0] = 1;
	table->cpu_ids[1] = 0;
	assert(!fp_scene_table_valid(table));
	table->cpu_ids[1] = 6;
	profile->cpus[0].margin_count = 9;
	assert(!fp_scene_table_valid(table));
	profile->cpus[0].margin_count = 2;
	profile->cpus[0].margins[1].threshold_khz = 0;
	assert(!fp_scene_table_valid(table));
	profile->cpus[0].margins[1].threshold_khz = 1500000;
	table->profiles[2].gpu_percent = 105;
	assert(!fp_scene_table_valid(table));
	table->profiles[2].gpu_percent = 0;
	assert(fp_scene_table_valid(table));
	free(table);
	puts("Scene table: selection, game exclusion, static/PMU feedback and invalid input tests passed");
	return 0;
}
