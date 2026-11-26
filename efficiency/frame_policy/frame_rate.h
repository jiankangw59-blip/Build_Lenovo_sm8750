/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_RATE_H
#define FRAME_RATE_H

#include "frame_control.h"

/* Cadence qualification is driven only by actual successful submissions. */
struct fp_rate {
	u64 average_ns, window_ns;
	u32 samples, target, candidate, window_frames, candidate_windows, bad_windows;
};

static inline void fp_rate_reset(struct fp_rate *state)
{
	*state = (struct fp_rate){};
}

static inline u32 fp_rate_sample(struct fp_rate *state, u64 interval_ns,
				 const u32 *rates, u32 count)
{
	u32 fps, candidate = 0, i;

	if (!interval_ns || interval_ns >= 1000000000ULL) {
		fp_rate_reset(state);
		return 0;
	}
	if (state->samples < 16)
		state->samples++;
	state->window_ns += interval_ns;
	state->window_frames++;
	/* Queue returns can bunch up while average cadence stays unchanged. */
	if (state->window_ns < 500000000ULL || state->window_frames < 12)
		return state->target;
	fps = ((u64)state->window_frames * 1000000000ULL + state->window_ns / 2) /
		state->window_ns;
	state->average_ns = state->window_ns / state->window_frames;
	state->window_ns = 0;
	state->window_frames = 0;
	/* Keep an established grade through modest jitter before trying another. */
	for (i = 0; i < count; i++) {
		u32 rate = rates[i], low = rate / 6 > 10 ? rate / 6 : 10;
		u32 high = rate == state->target ? (rate / 12 > 3 ? rate / 12 : 3) :
			(rate / 40 > 1 ? rate / 40 : 1);

		if (fps + low < rate || fps > rate + high)
			continue;
		if (!candidate || state->target == rate)
			candidate = rate;
		if (state->target == rate)
			break;
	}
	if (!candidate) {
		state->candidate = state->candidate_windows = 0;
		if (state->bad_windows < 2)
			state->bad_windows++;
		if (state->bad_windows >= 2)
			state->target = 0;
		return state->target;
	}
	state->bad_windows = 0;
	if (state->candidate != candidate) {
		state->candidate = candidate;
		state->candidate_windows = 0;
	}
	if (state->candidate_windows < 2)
		state->candidate_windows++;
	if (state->candidate_windows >= 2)
		state->target = candidate;
	return state->target;
}

#endif
