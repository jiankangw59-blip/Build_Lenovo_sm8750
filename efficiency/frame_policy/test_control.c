// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdio.h>
#include "frame_control.h"
#include "frame_policy_uapi.h"

int main(void)
{
	struct fp_control state = {};
	u32 previous = 0, result;
	unsigned int i;

	assert(sizeof(struct fp_config) == 248);
	assert(offsetof(struct fp_config, cpus) == 120);
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
	puts("Frame controller bounds, warmup, recovery, inactivity and ABI checks passed");
	return 0;
}
