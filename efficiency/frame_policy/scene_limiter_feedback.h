/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_LIMITER_FEEDBACK_H
#define FRAME_SCENE_LIMITER_FEEDBACK_H

#ifndef __KERNEL__
#include <stdbool.h>
#endif
#include "frame_control.h"

#define FP_SCENE_LIMITER_MAX_CPUS 64U
#define FP_SCENE_LIMITER_MAX_HISTORY 32U
#define FP_SCENE_LIMITER_MT_SCALE 100000U /* 100 percent, in thousandths of a percent. */
#define FP_SCENE_LIMITER_SMOOTH_RECENT_NS 3000000000ULL
#define FP_SCENE_LIMITER_CORE_RECENT_NS 2000000000ULL
#define FP_SCENE_LIMITER_NOTIFY_NS 100000000ULL
#define FP_SCENE_LIMITER_GESTURE_MIN_NS 500000000ULL

struct fp_scene_limiter_pulse {
	u64 deadline_ns;
	u64 pending_ns;
};

static inline u64 fp_scene_limiter_deadline(u64 now_ns, u64 duration_ns)
{
	const u64 max_ns = (u64)~0ULL;

	return duration_ns > max_ns - now_ns ? max_ns : now_ns + duration_ns;
}

/* Native 536a30..536b94 and 55b960..55bac4 retain the largest requested
 * interval in their current sleep. Kernel callers use one deadline per
 * active interval; only an observed event can queue its successor.
 */
static inline bool fp_scene_limiter_pulse_advance(struct fp_scene_limiter_pulse *pulse, u64 now_ns)
{
	if (pulse->deadline_ns && now_ns >= pulse->deadline_ns) {
		u64 pending_deadline = fp_scene_limiter_deadline(pulse->deadline_ns, pulse->pending_ns);

		pulse->deadline_ns = now_ns < pending_deadline ? pending_deadline : 0;
		pulse->pending_ns = 0;
	}
	return pulse->deadline_ns != 0;
}

static inline void fp_scene_limiter_pulse_trigger(struct fp_scene_limiter_pulse *pulse, u64 now_ns,
						  u64 duration_ns)
{
	if (!duration_ns)
		return;
	if (!fp_scene_limiter_pulse_advance(pulse, now_ns))
		pulse->deadline_ns = fp_scene_limiter_deadline(now_ns, duration_ns);
	else if (duration_ns > pulse->pending_ns)
		pulse->pending_ns = duration_ns;
}

struct fp_scene_limiter_gesture {
	u32 origin[2];
	u32 current[2];
	bool edge;
	bool moved;
	bool boosting;
};

/* Native 55b690..55b928 / 55c230. The caller forwards physical axis
 * updates only for the selected panel and clears the state on contact start.
 */
static inline bool fp_scene_limiter_gesture_axis(struct fp_scene_limiter_gesture *gesture,
						u32 axis, u32 coordinate, u32 contacts,
						u32 max_x, u32 max_y)
{
	u32 short_axis = max_x < max_y ? max_x : max_y;
	u32 threshold = (u64)short_axis * 6 / 100;
	u32 origin, delta_x, delta_y;

	if (axis > 1 || !threshold)
		return false;
	gesture->current[axis] = coordinate;
	origin = gesture->origin[axis];
	if (!origin) {
		u32 x, y;

		gesture->origin[axis] = coordinate;
		x = gesture->origin[0];
		y = gesture->origin[1];
		gesture->edge = x && y && (x < threshold || x > max_x - threshold ||
					  y < threshold || y > max_y - threshold);
		return false;
	}
	if (contacts != 1) {
		gesture->boosting = false;
		gesture->edge = false;
		return false;
	}
	if (gesture->boosting)
		return true;
	delta_x = gesture->current[0] > gesture->origin[0] ?
		  gesture->current[0] - gesture->origin[0] : gesture->origin[0] - gesture->current[0];
	delta_y = gesture->current[1] > gesture->origin[1] ?
		  gesture->current[1] - gesture->origin[1] : gesture->origin[1] - gesture->current[1];
	if (delta_x < threshold && delta_y < threshold)
		return false;
	gesture->moved = true;
	gesture->boosting = gesture->edge;
	return gesture->boosting;
}

/* Native 55b690 uses a profile duration (milliseconds), with a 500 ms minimum. */
static inline u64 fp_scene_limiter_gesture_duration(u32 duration_ms)
{
	return duration_ms > 500 ? (u64)duration_ms * 1000000 : FP_SCENE_LIMITER_GESTURE_MIN_NS;
}

struct fp_scene_limiter_notify {
	u64 running_ns;
	u64 mean_ns;
	u64 sent_stage;
	u32 samples;
};

/* Native CPU limiter PerfNotify, 535478..5354ec. The action subscriber
 * separately rejects duration <= 6 ms, matching 57c090.
 */
static inline u64 fp_scene_limiter_notify_threshold(u32 display_hz)
{
	if (display_hz > 90)
		return 6600000;
	return display_hz > 60 ? 8500000 : 10000000;
}

/* Called only for one of the explicitly monitored threads. The caller must
 * discard old identity/epoch state when it rebinds the monitor.
 */
static inline bool fp_scene_limiter_notify_sample(struct fp_scene_limiter_notify *task,
						   u64 now_ns, u64 threshold_ns,
						   bool switched_in, bool switched_out)
{
	u64 elapsed, stage;
	bool over;

	if (switched_in) {
		task->running_ns = now_ns;
		task->sent_stage = 0;
		return false;
	}
	if (!task->running_ns || now_ns <= task->running_ns)
		return false;
	elapsed = now_ns - task->running_ns;
	if (switched_out) {
		task->running_ns = 0;
		task->sent_stage = 0;
		if (elapsed < 1000000)
			return false;
		if (task->samples < 33)
			task->samples++;
		if (!task->mean_ns)
			task->mean_ns = elapsed;
		else
			task->mean_ns = 7 * (task->mean_ns / 8) + elapsed / 8 +
					(7 * (task->mean_ns % 8) + elapsed % 8) / 8;
		/* Use 64-bit arithmetic without multiplying an unbounded runtime. */
		over = task->samples == 33 && task->mean_ns <= ((u64)~0ULL - task->mean_ns / 2) / 2 &&
		       elapsed > 2 * task->mean_ns + task->mean_ns / 2;
		return elapsed > 6000000 && (over || (threshold_ns && elapsed > threshold_ns));
	}
	if (!threshold_ns)
		return false;
	stage = elapsed / threshold_ns;
	if (!stage || stage / 2 < task->sent_stage)
		return false;
	task->sent_stage = stage;
	return elapsed > 6000000;
}

static inline bool fp_scene_limiter_input_recent(u64 now_ns, u64 last_input_ns,
						 u32 contacts, u64 window_ns)
{
	return contacts || (last_input_ns && now_ns >= last_input_ns &&
			    now_ns - last_input_ns < window_ns);
}

/* Native 5347f8..53487c uses stat_source=1 for the ordinary Scene entry. */
static inline bool fp_scene_limiter_extended_window(u32 stat_source, bool standby,
						   bool override_limiter)
{
	return override_limiter || (stat_source == 1 && !standby);
}

struct fp_scene_limiter_cpu_sample {
	u32 cpu;
	u32 effective_khz;
	bool valid;
};

struct fp_scene_limiter_load {
	u32 peak_khz;
	u32 mean_khz;
	u32 mixed_khz;
	u32 samples;
};

/* Native 535dc0..535e44. Cluster zero uses the mean in the non-boost path. */
static inline u32 fp_scene_limiter_mt(u32 configured_mt, u32 cluster_slot,
				     bool blend_enabled, bool boost)
{
	if (!blend_enabled)
		return 0;
	if (!boost && !cluster_slot)
		return FP_SCENE_LIMITER_MT_SCALE;
	return configured_mt;
}

/* Native 535e94..535f20. PMU counter scaling and time belong to the caller.
 * Exclusions remove entire CPU IDs from both the maximum and the denominator.
 * A zero demand on a sampled CPU is valid. A missing sample is not a zero.
 */
static inline bool fp_scene_limiter_aggregate(const struct fp_scene_limiter_cpu_sample *cpus,
					       u32 count, u64 excludes, u32 mt,
					       struct fp_scene_limiter_load *load)
{
	u64 sum = 0, seen = 0;
	u32 i, peak = 0, samples = 0;

	if (!cpus || !load || count > FP_SCENE_LIMITER_MAX_CPUS || mt > FP_SCENE_LIMITER_MT_SCALE)
		return false;
	for (i = 0; i < count; i++) {
		u32 cpu = cpus[i].cpu;
		u64 bit;

		if (cpu >= FP_SCENE_LIMITER_MAX_CPUS)
			return false;
		bit = 1ULL << cpu;
		if (seen & bit)
			return false;
		seen |= bit;
		if (!cpus[i].valid || (excludes & bit))
			continue;
		if (cpus[i].effective_khz > peak)
			peak = cpus[i].effective_khz;
		sum += cpus[i].effective_khz;
		samples++;
	}
	if (!samples)
		return false;
	load->samples = samples;
	load->peak_khz = peak;
	load->mean_khz = sum / samples;
	/* Compute the blend before truncating the mean to integer kHz. */
	load->mixed_khz = ((u64)peak * samples * (FP_SCENE_LIMITER_MT_SCALE - mt) + sum * mt) /
			  (samples * FP_SCENE_LIMITER_MT_SCALE);
	return true;
}

struct fp_scene_limiter_history {
	u64 sum;
	u32 values[FP_SCENE_LIMITER_MAX_HISTORY];
	u32 capacity, next, count;
};

/* Native 536d40..536e4c and 5370b8..5370dc for sun. */
static inline u32 fp_scene_limiter_sun_perfect(u32 configured_khz, u32 hardware_max_khz,
					      u32 profile_max_khz)
{
	u32 perfect_khz = configured_khz ? configured_khz : (u64)hardware_max_khz * 4 / 5;

	return perfect_khz > profile_max_khz ? profile_max_khz : perfect_khz;
}

/* Native 537d34..537dd4. The default 8/5 selector comes from the caller.
 * Reinitialize on the same events that recreate the native policy closure.
 */
static inline bool fp_scene_limiter_history_init(struct fp_scene_limiter_history *history,
					          u32 smoothness, bool extended_window)
{
	if (smoothness > FP_SCENE_LIMITER_MAX_HISTORY)
		return false;
	*history = (struct fp_scene_limiter_history){ .capacity = smoothness ? smoothness :
								   extended_window ? 8 : 5 };
	return true;
}

static inline void fp_scene_limiter_history_push(struct fp_scene_limiter_history *history, u32 value)
{
	if (history->count == history->capacity)
		history->sum -= history->values[history->next];
	else
		history->count++;
	history->values[history->next] = value;
	history->sum += value;
	history->next = (history->next + 1) % history->capacity;
}

/* Native 537f98..538458. An explicit Launcher subcategory or prefer=3
 * uses the peak path. The other caller flag selects the same path. Neither
 * path stores the smoothed estimate back into the history.
 */
static inline u32 fp_scene_limiter_smooth(struct fp_scene_limiter_history *history, u32 demand_khz,
					  u32 perfect_khz, u32 prefer, bool enabled,
					  bool launcher, bool peak_path)
{
	u32 i, mean = 0, estimate = demand_khz;

	if (!enabled || prefer == 1 || !history->capacity)
		return demand_khz;
	if (history->count)
		mean = history->sum / history->count;
	if (launcher || prefer == 3 || peak_path) {
		u32 peak = 0;
		u32 midpoint;

		for (i = 0; i < history->count; i++)
			if (history->values[i] > peak)
				peak = history->values[i];
		midpoint = ((u64)peak + demand_khz) / 2;
		if (midpoint > estimate)
			estimate = midpoint;
		if (mean > estimate)
			estimate = mean;
	} else {
		if (mean > perfect_khz)
			mean = perfect_khz;
		if (mean > estimate)
			estimate = mean;
	}
	fp_scene_limiter_history_push(history, demand_khz);
	return estimate;
}

struct fp_scene_limiter_margin_step {
	__s32 threshold_khz;
	__s32 offset_khz;
};

/* Native 537590 uses an additive offset in kHz. Table thresholds are tested
 * against demand plus the preceding offset. 537110 converts the configured
 * offsets from MHz when building this table.
 */
static inline __s32 fp_scene_limiter_margin(u32 demand_khz,
					    const struct fp_scene_limiter_margin_step *table,
					    u32 count, __s32 extra_khz)
{
	u64 limit = (u64)demand_khz * 2 / 5;
	__s64 adjusted;
	__s32 offset;
	u32 i;

	if (!table || !count || count > FP_MAX_MARGINS)
		return 0;
	offset = table[0].offset_khz;
	for (i = 1; i < count; i++) {
		if ((__s64)demand_khz + offset < table[i].threshold_khz)
			break;
		offset = table[i].offset_khz;
	}
	adjusted = (__s64)offset + extra_khz;
	if (extra_khz < 0 && adjusted < demand_khz / 33)
		adjusted = demand_khz / 33;
	return adjusted > (__s64)limit ? (__s32)limit : (__s32)adjusted;
}

struct fp_scene_limiter_vote_state {
	__s64 last_estimate_khz;
	u32 last_vote_khz;
	u32 small_changes;
	u32 rising_writes;
};

/* Native 5380d0..538164. Call with resolved dynamic or fixed peak margin.
 * The peer argument is zero unless the original inter-cluster gate is on.
 */
static inline u32 fp_scene_limiter_estimate(struct fp_scene_limiter_vote_state *state,
					  u32 smooth_khz, __s32 margin_khz, u32 perfect_khz,
					  u32 peer_khz, bool enabled)
{
	__s64 estimate = (__s64)smooth_khz + margin_khz;
	__s64 previous = state->last_estimate_khz;
	__s64 peer_floor = (__s64)peer_khz - 100000;

	state->last_estimate_khz = estimate;
	/* The ordinary, non-Game callback adds 100 MHz per prior rising write. */
	if (enabled && estimate > previous + 250000 && state->rising_writes &&
	    estimate < perfect_khz && margin_khz < 200000) {
		estimate += (__s64)state->rising_writes * 100000;
		if (estimate > perfect_khz)
			estimate = perfect_khz;
	}
	if (estimate < peer_floor)
		estimate = peer_floor;
	if (estimate <= 0)
		return 0;
	if (estimate > (u32)~0U)
		return (u32)~0U;
	return estimate;
}

/* Native 538190..5381e4 and 5382d0..5382dc. The caller selects and clamps
 * the OPP before this gate and writes the returned vote only when changed.
 */
static inline u32 fp_scene_limiter_vote(struct fp_scene_limiter_vote_state *state, u32 candidate)
{
	u32 previous = state->last_vote_khz;
	u32 delta = candidate > previous ? candidate - previous : previous - candidate;

	if (delta <= 20000 && state->small_changes <= 15) {
		state->small_changes++;
		state->rising_writes = 0;
		return previous;
	}
	state->small_changes = 0;
	if (candidate <= previous)
		state->rising_writes = 0;
	else if (state->rising_writes != (u32)~0U)
		state->rising_writes++;
	state->last_vote_khz = candidate;
	return candidate;
}

enum fp_scene_limiter_source {
	FP_SCENE_LIMITER_OTHER_SOURCE,
	FP_SCENE_LIMITER_SCENE_ONLINE,
	FP_SCENE_LIMITER_SCENE_CUSTOM,
};

struct fp_scene_limiter_core {
	struct fp_scene_limiter_history demand;
	u32 retained;
	u32 cluster_count;
	u32 cluster_ids_packed;
	u32 enabled_clusters;
	bool topology_changed;
};

struct fp_scene_limiter_core_result {
	bool applicable;
	bool restore_all;
	u32 retained;
	u32 active_clusters;
};

static inline bool fp_scene_limiter_sun_core_init(struct fp_scene_limiter_core *state, u32 retained)
{
	if (retained > 2)
		return false;
	*state = (struct fp_scene_limiter_core){ .retained = retained };
	return fp_scene_limiter_history_init(&state->demand, 3, false);
}

/* Native 535dc0..535e00 uses the core limiter's last selected clusters to
 * enable blending. Without a configured core limiter all clusters can blend.
 */
static inline bool fp_scene_limiter_core_blend(const struct fp_scene_limiter_core *state,
					     u32 cluster_id, bool limiter_present)
{
	return !limiter_present || (cluster_id < 2 && (state->enabled_clusters & (1U << cluster_id)));
}

/* Native 532ee0 records each demand before Apply checks the source. The
 * caller supplies the verified predicates from 532d18..532e1c. 'restore'
 * is the early force-all predicate. 'smooth' is the game/boost/recent-event
 * branch and uses the three-sample mean. Thresholds are the exact 'sun'
 * constants from 533334..5333b4. They are indexed by prefix position.
 */
static inline struct fp_scene_limiter_core_result fp_scene_limiter_sun_core_step(
		struct fp_scene_limiter_core *state, enum fp_scene_limiter_source source,
		u32 demand_khz, u32 cluster_count, u32 cluster_ids_packed, bool restore, bool smooth)
{
	const u32 retain_threshold[] = { 960000, 1363200 };
	const u32 add_threshold[] = { 1363200, 1785600 };
	struct fp_scene_limiter_core_result result = {};
	u32 i, mask = 0;

	if (state->demand.capacity != 3 || !cluster_count || cluster_count > 2 ||
	    cluster_ids_packed >> (cluster_count * 8))
		return result;
	for (i = 0; i < cluster_count; i++) {
		u32 id = (cluster_ids_packed >> (i * 8)) & 255;

		if (id >= 2 || (mask & (1U << id)))
			return result;
		mask |= 1U << id;
	}
	if (cluster_count != state->cluster_count || cluster_ids_packed != state->cluster_ids_packed) {
		state->cluster_count = cluster_count;
		state->cluster_ids_packed = cluster_ids_packed;
		state->topology_changed = true;
	}
	fp_scene_limiter_history_push(&state->demand, demand_khz);
	if (source != FP_SCENE_LIMITER_SCENE_ONLINE && source != FP_SCENE_LIMITER_SCENE_CUSTOM)
		return result;
	result.applicable = true;
	result.restore_all = restore;
	if (restore) {
		result.retained = cluster_count;
		result.active_clusters = 3; /* 532bb0 restores both physical clusters. */
	} else {
		if (smooth)
			demand_khz = state->demand.sum / state->demand.count;
		for (i = 0; i < 2; i++) {
			u32 threshold = i < state->retained ? retain_threshold[i] : add_threshold[i];

			if (threshold < demand_khz)
				result.retained = i + 1;
		}
		for (i = 0; i < cluster_count && i < result.retained; i++)
			result.active_clusters |= 1U << ((cluster_ids_packed >> (i * 8)) & 255);
		if (state->topology_changed || state->retained != result.retained)
			state->enabled_clusters = (state->enabled_clusters & ~mask) | result.active_clusters;
		state->topology_changed = false;
	}
	state->retained = result.retained;
	return result;
}

#endif
