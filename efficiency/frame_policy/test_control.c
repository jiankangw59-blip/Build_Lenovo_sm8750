// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include "frame_control.h"
#include "frame_rate.h"
#include "frame_policy_uapi.h"

int main(void)
{
	struct fp_control state = {};
	u32 previous = 0, result;
	unsigned int i;
	struct fp_rate rate = {};
	u32 rates[] = { 30, 60, 90, 120, 165 }, high_only[] = { 165 };

	assert(sizeof(struct fp_config) == 592);
	assert(offsetof(struct fp_config, cpus) == 144);
	for (i = 0; i < 600; i++)
		assert(fp_rate_sample(&rate, 16666667, high_only, 1) == 0);
	fp_rate_reset(&rate);
	for (i = 0; i < 29; i++)
		assert(fp_rate_sample(&rate, 16666667, rates, 5) == 0);
	assert(fp_rate_sample(&rate, 16666667, rates, 5) == 60);
	for (i = 0; i < 300; i++)
		fp_rate_sample(&rate, 6060606, rates, 5);
	assert(rate.target == 165);
	for (i = 0; i < 180; i++)
		fp_rate_sample(&rate, 33333333, rates, 5);
	assert(rate.target == 30);
	assert(fp_rate_sample(&rate, 1000000000, rates, 5) == 0);
	assert(rate.samples == 0);
	for (i = 0; i < 600; i++)
		assert(fp_rate_sample(&rate, 100000000, rates, 5) == 0);
	for (i = 0; i < 120; i++)
		fp_rate_sample(&rate, 11111111 + (i % 3) * 50000, rates, 5);
	assert(rate.target == 90);
	previous = 4000000;
	for (i = 0; i < 600; i++) {
		result = fp_game_ceiling(previous, 500000, 4000000, 100, 16666667, 60, 20000000);
		assert(result <= previous && result >= 500000);
		assert(previous - result <= 17500);
		previous = result;
	}
	assert(previous == 500000);
	for (i = 0; i < 60; i++) {
		result = fp_game_ceiling(previous, 500000, 4000000, 100, 24000000, 60, 20000000);
		assert(result >= previous && result <= 4000000);
		previous = result;
	}
	assert(previous == 4000000);
	assert(fp_game_ceiling(1000000, 500000, 4000000, 900, 16666667, 60, 20000000) == 1000000);
	assert(fp_game_ceiling(1000000, 500000, 4000000, 0, 16666667, 60, 0) == 1000000);
	assert(fp_game_ceiling(0, 500000, 4000000, 0, 16666667, 60, 0) == 500000);
	previous = 0;
	for (i = 0; i < 200; i++) {
		result = fp_control_frame(&state, 24000000, 60);
		assert(result >= previous && result <= 1000);
		previous = result;
	}
	assert(state.level == 1000);
	for (i = 0; i < 600; i++) {
		result = fp_control_frame(&state, 12000000, 60);
		assert(result <= 1000);
	}
	assert(state.level == 0);
	{
		struct fp_cpu_config cpu = {
			.scene = { 960000, 2400000 }, .margin_count = 3,
			.margins = { { 0, 200 }, { 1555200, 130 }, { 1996800, 90 } },
		};
		assert(fp_scene_ceiling(0, &cpu) == 960000);
		assert(fp_scene_ceiling(1000000, &cpu) == 2000000);
		assert(fp_scene_ceiling(1555200, &cpu) == 2021760);
		assert(fp_scene_ceiling(1996800, &cpu) == 1797120);
		assert(fp_scene_ceiling(4000000, &cpu) == 2400000);
	}
	assert(fp_control_floor(0, 384000, 3532800) == 384000);
	assert(fp_control_floor(1000, 384000, 3532800) == 3532800);
	assert(fp_control_frame(&state, 5000000000ULL, 165) == 0);
	assert(state.samples == 0 && state.average_ns == 0);
	for (i = 0; i < 100; i++)
		fp_control_gpu(&state, 16000000, 60);
	assert(state.level == 1000);
	for (i = 0; i < 600; i++)
		fp_control_gpu(&state, 0, 60);
	assert(state.level == 0);
	for (i = 0; i < 200000; i++) {
		u64 duration = (u64)i * 7919 % 300000000ULL;
		u32 level = fp_control_frame(&state, duration, 15 + i % 226);

		assert(level <= 1000);
		assert(fp_control_floor(level, 0, 10000000) <= 10000000);
	}
	puts("Cadence qualification, transitions, controller bounds, recovery and ABI checks passed");
	return 0;
}
