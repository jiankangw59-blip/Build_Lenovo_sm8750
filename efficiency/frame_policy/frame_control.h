/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_CONTROL_H
#define FRAME_CONTROL_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdint.h>
typedef uint64_t u64;
typedef uint32_t u32;
#endif

/*
 * Independent integer controller. A slow frame raises the requested floor,
 * while sustained headroom lowers it. A single long pause resets history.
 */
struct fp_control {
	u64 average_ns;
	u32 level;
	u32 samples;
	u32 good_frames;
};

static inline void fp_control_reset(struct fp_control *state)
{
	*state = (struct fp_control){};
}

static inline u32 fp_control_frame(struct fp_control *state, u64 interval_ns,
				   u32 target_fps)
{
	u64 target_ns = 1000000000ULL / target_fps;

	if (!interval_ns || interval_ns > 250000000ULL) {
		fp_control_reset(state);
		return 0;
	}
	state->average_ns = state->samples ?
		(state->average_ns * 7 + interval_ns) / 8 : interval_ns;
	if (state->samples < 8) {
		state->samples++;
		return state->level;
	}
	if (state->average_ns > target_ns + target_ns / 16) {
		state->level = state->level > 950 ? 1000 : state->level + 50;
		state->good_frames = 0;
	} else if (state->average_ns < target_ns && ++state->good_frames >= 8) {
		state->level = state->level < 25 ? 0 : state->level - 25;
		state->good_frames = 0;
	}
	return state->level;
}

static inline u32 fp_control_floor(u32 level, u32 min_khz, u32 max_khz)
{
	return min_khz + (u64)(max_khz - min_khz) * level / 1000;
}

static inline u32 fp_control_gpu(struct fp_control *state, u64 pending_ns, u32 target_fps)
{
	u64 budget = 1000000000ULL / target_fps;

	state->samples++;
	state->average_ns = state->samples == 1 ? pending_ns :
		(state->average_ns * 7 + pending_ns) / 8;
	if (state->average_ns > budget * 3 / 4) {
		state->level = state->level > 950 ? 1000 : state->level + 50;
		state->good_frames = 0;
	} else if (state->average_ns < budget / 4 && ++state->good_frames >= 8) {
		state->level = state->level < 25 ? 0 : state->level - 25;
		state->good_frames = 0;
	}
	return state->level;
}

#endif
