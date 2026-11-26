/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_RATE_H
#define FRAME_RATE_H

#include "frame_control.h"

/* Cadence qualification is driven only by actual successful submissions. */
struct fp_rate {
	u64 average_ns, candidate_ns;
	u32 samples, target, candidate;
};

static inline void fp_rate_reset(struct fp_rate *state)
{
	*state = (struct fp_rate){};
}

static inline u32 fp_rate_sample(struct fp_rate *state, u64 interval_ns,
				 const u32 *rates, u32 count)
{
	u32 fps, candidate = 0, i;

	if (!interval_ns || interval_ns > 250000000ULL) {
		fp_rate_reset(state);
		return 0;
	}
	state->average_ns = state->samples ?
		(state->average_ns * 15 + interval_ns) / 16 : interval_ns;
	if (state->samples < 16)
		state->samples++;
	fps = (1000000000ULL + state->average_ns / 2) / state->average_ns;
	/* Keep an established grade through modest jitter before trying another. */
	for (i = 0; i < count; i++) {
		u32 rate = rates[i], low = rate / 6 > 10 ? rate / 6 : 10;
		u32 high = rate / 40 > 1 ? rate / 40 : 1;

		if (fps + low < rate || fps > rate + high)
			continue;
		if (!candidate || state->target == rate)
			candidate = rate;
		if (state->target == rate)
			break;
	}
	if (!candidate) {
		state->target = state->candidate = 0;
		state->candidate_ns = 0;
		return 0;
	}
	if (state->candidate != candidate) {
		state->candidate = candidate;
		state->candidate_ns = 0;
		state->target = 0;
	}
	if (state->candidate_ns < 500000000ULL)
		state->candidate_ns += interval_ns;
	if (state->samples >= 12 && state->candidate_ns >= 500000000ULL)
		state->target = candidate;
	return state->target;
}

#endif
