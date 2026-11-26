// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdio.h>
#include "scene_activity_profiles.h"

int main(void)
{
	struct fp_scene_profile active = {0}, inactive = {0};

	assert(!fp_scene_activity_needed(NULL, NULL, FP_CPU | FP_GPU, 1));
	assert(!fp_scene_activity_needed(&active, &inactive, FP_CPU | FP_GPU, 1));
	inactive.gpu_percent = 105;
	assert(fp_scene_activity_needed(&active, &inactive, FP_GPU, 1));
	assert(!fp_scene_activity_needed(&active, &inactive, FP_CPU, 1));
	inactive = active; inactive.cpus[0].limits.max_khz = 2000000;
	assert(fp_scene_activity_needed(&active, &inactive, FP_CPU, 1));
	assert(!fp_scene_activity_needed(&active, &inactive, FP_GPU, 1));
	assert(fp_scene_activity_needed(NULL, &inactive, FP_GPU, 0));
	assert(fp_scene_activity_needed(&active, NULL, FP_CPU, 1));
	assert(!fp_scene_activity_needed(&active, NULL, FP_OBSERVE, 1));
	puts("scene activity gate: static, CPU-only, GPU-only, ignored profiles and disabled axes passed");
	return 0;
}
