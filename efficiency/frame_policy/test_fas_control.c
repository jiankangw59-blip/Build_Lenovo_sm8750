// SPDX-License-Identifier: GPL-2.0
#include <stdbool.h>
#include <assert.h>
#include <stdio.h>
#include "fas_control.h"

int main(void)
{
	struct fp_fas_tuning tuning = { .abi = FP_FAS_ABI, .mode = 1, .margin_base_mfps = 1000 };
	struct fp_fas_feedback state = {};
	u32 i;

	assert(sizeof(tuning) == 80);
	assert(fp_fas_tuning_valid(&tuning));
	assert(fp_fas_margin(&tuning, 60) == 1000);
	assert(fp_fas_margin(&tuning, 165) == 2750);
	assert(fp_fas_effective_rate(&tuning, &state, 120) == 118000);
	assert(fp_fas_proportional(16666667, 59000) == -4999);
	assert(fp_fas_proportional(20000000, 60000) == 60000);
	assert(fp_fas_proportional(0, 60000) == 0);
	assert(fp_fas_proportional(400000000, 60000) == 6900000);
	assert(fp_fas_proportional(1000000001, 60000) == 0);
	tuning.override_count = 1;
	tuning.overrides[0].fps = 165;
	tuning.overrides[0].margin_mfps = 300;
	assert(fp_fas_tuning_valid(&tuning));
	assert(fp_fas_effective_rate(&tuning, &state, 165) == 164700);
	assert(fp_fas_margin(&tuning, 120) == 2000);
	for (i = 1; i < 50; i++)
		fp_fas_util_event(&state, (u64)i * 1000000000ULL, 500, FP_FAS_UTIL_LIMIT_DEFAULT);
	assert(state.util_offset_mfps == -3000);
	fp_fas_util_event(&state, 49500000000ULL, 1000, FP_FAS_UTIL_LIMIT_DEFAULT);
	assert(state.util_offset_mfps == -3000);
	fp_fas_util_event(&state, 100000000000ULL, 1000, FP_FAS_UTIL_LIMIT_DEFAULT);
	assert(state.util_offset_mfps == -2900); /* One real event, no idle catch-up loop. */
	fp_fas_util_event(&state, 101000000000ULL, 100, FP_FAS_UTIL_LIMIT_DEFAULT);
	assert(state.util_offset_mfps == 0);
	for (u32 limit = 0; limit <= FP_FAS_UTIL_LIMIT_MAX; limit++) {
		struct fp_fas_tuning_ext extended = {.tuning = tuning, .util_limit_mfps = limit};

		assert(sizeof(extended) == 88);
		assert(fp_fas_tuning_ext_valid(&extended));
		state = (struct fp_fas_feedback){.util_offset_mfps = -3000};
		fp_fas_util_event(&state, 1000000000ULL, 500, limit);
		assert(state.util_offset_mfps == -(int)limit);
		for (i = 2; i < 45; i++)
			fp_fas_util_event(&state, (u64)i * 1000000000ULL, 500, limit);
		assert(state.util_offset_mfps == -(int)limit);
		extended.reserved = 1;
		assert(!fp_fas_tuning_ext_valid(&extended));
	}
	struct fp_fas_tuning_ext invalid_limit = {.tuning = tuning, .util_limit_mfps = 3001};
	assert(!fp_fas_tuning_ext_valid(&invalid_limit));
	assert(fp_fas_common_target(2000000, 1800000, 250, true, false, -5000, 4320000) == 900000);
	assert(fp_fas_common_target(2000000, 1800000, 250, true, true, 60000, 4320000) == 2060000);
	assert(fp_fas_common_target(2000000, 1800000, 250, false, false, -5000, 4320000) == 1995000);
	assert(fp_fas_common_target(4200000, 4200000, 1000, true, true, 20000000, 4320000) == 4320000);
	assert(fp_fas_common_target(1000000, 1000000, 500, false, true, -2000000, 4320000) == 0);
	tuning.mode = 4;
	assert(!fp_fas_tuning_valid(&tuning));
	tuning.mode = 1;
	tuning.overrides[0].margin_mfps = 165000;
	assert(!fp_fas_tuning_valid(&tuning));
	puts("FAS feedback: mode margins, fixed-point P control, event-driven utilization and shared targets passed");
	return 0;
}
