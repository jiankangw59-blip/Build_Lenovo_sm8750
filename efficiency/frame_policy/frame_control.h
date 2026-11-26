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

#include "frame_policy_uapi.h"

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

/* Independently authored bounded CPU ceiling feedback, with 40% load headroom. */
static inline u32 fp_game_ceiling(u32 previous_khz, u32 min_khz, u32 max_khz,
				  u32 busy_permille, u64 average_ns, u32 fps, u64 dt)
{
	u64 budget, desired, step;
	u32 ceiling = previous_khz < min_khz ? min_khz :
		previous_khz > max_khz ? max_khz : previous_khz;

	if (!fps || !average_ns || !dt)
		return ceiling;
	if (dt > 100000000ULL)
		dt = 100000000ULL;
	budget = 1000000000ULL / fps;
	if (average_ns > budget + budget / 32) {
		/* Slow cadence: recover at up to one full frequency range per second. */
		step = (u64)(max_khz - min_khz) * dt / 1000000000ULL;
		if (!step)
			step = 1;
		ceiling = step > max_khz - ceiling ? max_khz : ceiling + step;
	} else if (busy_permille < 600) {
		/* Only lower ceilings when the frame budget is met, never on a jank. */
		desired = (u64)ceiling * busy_permille / 600;
		if (desired < min_khz)
			desired = min_khz;
		step = (u64)(max_khz - min_khz) * dt / 4000000000ULL;
		if (!step)
			step = 1;
		if (ceiling > desired)
			ceiling -= ceiling - desired < step ? ceiling - desired : step;
	}
	return ceiling < min_khz ? min_khz : ceiling > max_khz ? max_khz : ceiling;
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

static inline u32 fp_scene_ceiling(u32 effective_khz, const struct fp_cpu_config *cpu)
{
	u32 margin = cpu->margins[0].percent, i;
	u64 ceiling;

	for (i = 1; i < cpu->margin_count; i++) {
		if (effective_khz < cpu->margins[i].threshold_khz)
			break;
		margin = cpu->margins[i].percent;
	}
	ceiling = (u64)effective_khz * margin / 100;
	if (ceiling < cpu->scene.min_khz)
		return cpu->scene.min_khz;
	if (ceiling > cpu->scene.max_khz)
		return cpu->scene.max_khz;
	return ceiling;
}

#endif
