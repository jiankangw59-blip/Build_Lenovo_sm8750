/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_FAS_CONTROL_H
#define FRAME_FAS_CONTROL_H

/* Independently authored integer implementation of the configured feedback law. */
#ifndef __KERNEL__
#include <stdbool.h>
#endif
#include "frame_control.h"
#include "fas_tuning_uapi.h"

struct fp_fas_feedback {
	u64 util_sample_ns;
	__s32 util_offset_mfps;
};

static inline bool fp_fas_tuning_valid(const struct fp_fas_tuning *tuning)
{
	u32 i;

	if (tuning->abi != FP_FAS_ABI || tuning->mode >= FP_FAS_MODES ||
	    tuning->margin_base_mfps > 30000 || tuning->override_count > FP_MAX_RATES)
		return false;
	for (i = 0; i < FP_MAX_RATES; i++) {
		u32 fps = tuning->overrides[i].fps, margin = tuning->overrides[i].margin_mfps;

		if (i >= tuning->override_count) {
			if (fps || margin)
				return false;
			continue;
		}
		if (fps < 15 || fps > 240 || margin > fps * 1000 - 4000 ||
		    (i && fps <= tuning->overrides[i - 1].fps))
			return false;
	}
	return true;
}

static inline u32 fp_fas_margin(const struct fp_fas_tuning *tuning, u32 fps)
{
	u32 i;

	for (i = 0; i < tuning->override_count; i++)
		if (tuning->overrides[i].fps == fps)
			return tuning->overrides[i].margin_mfps;
	return (u64)tuning->margin_base_mfps * fps / 60;
}

/* Called by real frame work; elapsed idle time never produces catch-up ticks. */
static inline void fp_fas_util_event(struct fp_fas_feedback *state, u64 now,
				      u32 util_permille)
{
	if (!state->util_sample_ns || now < state->util_sample_ns) {
		state->util_sample_ns = now;
		return;
	}
	if (now - state->util_sample_ns < 1000000000ULL)
		return;
	state->util_sample_ns = now;
	if (util_permille <= 100)
		state->util_offset_mfps = 0;
	else if (util_permille <= 550)
		state->util_offset_mfps -= 100;
	else if (util_permille >= 650)
		state->util_offset_mfps += 100;
	if (state->util_offset_mfps < -3000)
		state->util_offset_mfps = -3000;
	if (state->util_offset_mfps > 0)
		state->util_offset_mfps = 0;
}

static inline u32 fp_fas_effective_rate_margin(const struct fp_fas_feedback *state,
					       u32 fps, u32 margin_mfps)
{
	__s64 rate = (__s64)fps * 1000 + state->util_offset_mfps - margin_mfps;

	return rate < 1000 ? 1000 : rate;
}

static inline u32 fp_fas_effective_rate(const struct fp_fas_tuning *tuning,
					const struct fp_fas_feedback *state, u32 fps)
{
	return fp_fas_effective_rate_margin(state, fps, fp_fas_margin(tuning, fps));
}

/* kp=0.0003 kHz per normalized nanosecond, represented exactly as 3/10000. */
static inline __s64 fp_fas_proportional(u64 interval_ns, u32 effective_mfps)
{
	__s64 normalized;

	/* Keep a genuine long frame: rejecting 400ms would erase its recovery
	 * feedback. A one-second submission gap is handled by the owner deadline. */
	if (!interval_ns || interval_ns > 1000000000ULL || !effective_mfps || effective_mfps > 240000)
		return 0;
	normalized = (__s64)(interval_ns * effective_mfps / 1000) - 1000000000LL;
	return normalized * 3 / 10000;
}

static inline u32 fp_fas_common_target(u32 previous_max_khz, u32 actual_max_khz,
				       u32 util_permille, bool util_valid, bool janked,
				       __s64 adjustment_khz, u32 hardware_max_khz)
{
	__s64 desired = (__s64)previous_max_khz + adjustment_khz;
	u64 tracking = (u64)actual_max_khz * util_permille / 500;

	if (!janked && util_valid && desired > (__s64)tracking)
		desired = tracking;
	if (desired < 0)
		return 0;
	return desired > hardware_max_khz ? hardware_max_khz : desired;
}

#endif
