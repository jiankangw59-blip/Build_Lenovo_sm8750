/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include "scene_limiter_feedback.h"
#include "scene_policy.h"

static void test_aggregate(void)
{
	struct fp_scene_limiter_cpu_sample samples[] = {
		{ 6, 1000000, true }, { 0, 8000000, true }, { 7, 400000, true },
		{ 5, 0, false }, { 3, 0, true },
	};
	struct fp_scene_limiter_load load;
	u32 i;

	assert(fp_scene_limiter_mt(42000, 0, false, false) == 0);
	assert(fp_scene_limiter_mt(42000, 0, true, false) == FP_SCENE_LIMITER_MT_SCALE);
	assert(fp_scene_limiter_mt(42000, 0, true, true) == 42000);
	assert(fp_scene_limiter_mt(42000, 1, true, false) == 42000);
	assert(fp_scene_limiter_aggregate(samples, 5, 1ULL, 0, &load));
	assert(load.samples == 3 && load.peak_khz == 1000000 && load.mean_khz == 466666);
	assert(load.mixed_khz == 1000000);
	assert(fp_scene_limiter_aggregate(samples, 5, 1ULL, 50000, &load));
	assert(load.mixed_khz == 733333);
	assert(fp_scene_limiter_aggregate(samples, 5, 1ULL, 100000, &load));
	assert(load.mixed_khz == 466666);
	assert(!fp_scene_limiter_aggregate(samples, 5, ~0ULL, 50000, &load));
	assert(!fp_scene_limiter_aggregate(samples, 5, 1ULL, 100001, &load));
	samples[4].cpu = 0;
	assert(!fp_scene_limiter_aggregate(samples, 5, 1ULL, 0, &load));
	samples[4].cpu = 64;
	assert(!fp_scene_limiter_aggregate(samples, 5, 1ULL, 0, &load));
	for (i = 0; i < 5; i++)
		samples[i] = (struct fp_scene_limiter_cpu_sample){ i, UINT32_MAX, true };
	assert(fp_scene_limiter_aggregate(samples, 5, 0, 99999, &load));
	assert(load.peak_khz == UINT32_MAX && load.mean_khz == UINT32_MAX && load.mixed_khz == UINT32_MAX);
}

static void test_smoothing(void)
{
	struct fp_scene_limiter_history history = {};

	assert(fp_scene_limiter_extended_window(1, false, false));
	assert(!fp_scene_limiter_extended_window(1, true, false));
	assert(!fp_scene_limiter_extended_window(3, false, false));
	assert(fp_scene_limiter_extended_window(3, true, true));
	assert(!fp_scene_limiter_input_recent(1, 0, 0, FP_SCENE_LIMITER_SMOOTH_RECENT_NS));
	assert(!fp_scene_limiter_input_recent(1, 2, 0, FP_SCENE_LIMITER_SMOOTH_RECENT_NS));
	assert(fp_scene_limiter_input_recent(1, 0, 1, FP_SCENE_LIMITER_SMOOTH_RECENT_NS));
	assert(fp_scene_limiter_input_recent(3000000041ULL, 42, 0, FP_SCENE_LIMITER_SMOOTH_RECENT_NS));
	assert(!fp_scene_limiter_input_recent(3000000042ULL, 42, 0, FP_SCENE_LIMITER_SMOOTH_RECENT_NS));
	assert(!fp_scene_limiter_input_recent(2000000042ULL, 42, 0, FP_SCENE_LIMITER_CORE_RECENT_NS));
	assert(fp_scene_limiter_sun_perfect(0, 3000000, 2500000) == 2400000);
	assert(fp_scene_limiter_sun_perfect(0, 3000000, 2000000) == 2000000);
	assert(fp_scene_limiter_sun_perfect(2200000, 3000000, 2500000) == 2200000);
	assert(fp_scene_limiter_smooth(&history, 99, 100, 0, true, false, false) == 99);
	assert(fp_scene_limiter_history_init(&history, 0, true) && history.capacity == 8);
	assert(fp_scene_limiter_history_init(&history, 0, false) && history.capacity == 5);
	assert(!fp_scene_limiter_history_init(&history, 33, false));
	assert(fp_scene_limiter_history_init(&history, 2, false));
	assert(fp_scene_limiter_smooth(&history, 100, 100, 1, true, true, true) == 100);
	assert(fp_scene_limiter_smooth(&history, 100, 100, 3, false, true, true) == 100);
	assert(!history.count && !history.sum);
	assert(fp_scene_limiter_smooth(&history, 170, 125, 0, true, false, false) == 170);
	assert(fp_scene_limiter_smooth(&history, 90, 125, 0, true, false, false) == 125);
	assert(history.count == 2 && history.sum == 260);
	assert(fp_scene_limiter_smooth(&history, 80, 110, 3, true, false, false) == 130);
	assert(history.count == 2 && history.sum == 170);
	assert(fp_scene_limiter_smooth(&history, 50, 60, 0, true, true, false) == 85);
	assert(history.count == 2 && history.sum == 130);
	assert(fp_scene_limiter_smooth(&history, 150, 140, 0, true, false, true) == 150);
	assert(history.count == 2 && history.sum == 200);
	assert(fp_scene_limiter_history_init(&history, 1, false));
	fp_scene_limiter_history_push(&history, UINT32_MAX);
	assert(fp_scene_limiter_smooth(&history, UINT32_MAX, 1, 3, true, false, false) == UINT32_MAX);
}

static void test_margin(void)
{
	const struct fp_scene_limiter_margin_step table[] = {
		{ 0, 100000 }, { 1000000, 180000 }, { 1500000, 250000 },
	};
	const struct fp_scene_limiter_margin_step signed_table[] = {
		{ 0, -200000 }, { 25000, 10000 },
	};

	assert(!fp_scene_limiter_margin(900000, NULL, 3, 0));
	assert(fp_scene_limiter_margin(899999, table, 3, 0) == 100000);
	assert(fp_scene_limiter_margin(900000, table, 3, 0) == 180000);
	assert(fp_scene_limiter_margin(1320000, table, 3, 0) == 250000);
	assert(fp_scene_limiter_margin(900000, table, 3, -999999) == 900000 / 33);
	assert(fp_scene_limiter_margin(900000, table, 3, 999999) == 360000);
	assert(!fp_scene_limiter_margin(0, table, 3, -1));
	assert(fp_scene_limiter_margin(224999, signed_table, 2, 0) == -200000);
	assert(fp_scene_limiter_margin(225000, signed_table, 2, 0) == 10000);
}

static void test_core(void)
{
	struct fp_scene_limiter_core state;
	struct fp_scene_limiter_core_result r;

	assert(!fp_scene_limiter_sun_core_init(&state, 3));
	assert(fp_scene_limiter_sun_core_init(&state, 0));
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_OTHER_SOURCE, 2000000, 2, 0x100, false, false);
	assert(!r.applicable && state.demand.count == 1 && state.retained == 0);
	assert(!fp_scene_limiter_core_blend(&state, 0, true));
	assert(fp_scene_limiter_core_blend(&state, 0, false));
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_ONLINE, 1363200, 2, 0x100, false, false);
	assert(r.applicable && !r.retained && !r.active_clusters);
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_CUSTOM, 1363201, 2, 0x001, false, false);
	assert(r.applicable && r.retained == 1 && r.active_clusters == 2);
	assert(!fp_scene_limiter_core_blend(&state, 0, true));
	assert(fp_scene_limiter_core_blend(&state, 1, true));
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_CUSTOM, 960001, 2, 0x100, false, false);
	assert(r.retained == 1 && r.active_clusters == 1);
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_CUSTOM, 960000, 2, 0x100, false, false);
	assert(!r.retained && !r.active_clusters);
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_CUSTOM, 50, 1, 0x001, true, false);
	assert(r.retained == 1 && r.restore_all && r.active_clusters == 3);
	assert(!fp_scene_limiter_core_blend(&state, 0, true));
	assert(!fp_scene_limiter_core_blend(&state, 1, true));
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_CUSTOM, 1785601, 2, 0x100, false, false);
	assert(r.retained == 2 && r.active_clusters == 3);
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_CUSTOM, 1363200, 2, 0x100, false, false);
	assert(r.retained == 1 && r.active_clusters == 1);
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_CUSTOM, 950000, 2, 0x100, false, true);
	assert(r.retained == 1 && r.active_clusters == 1); /* Mean keeps the old first slot. */
	r = fp_scene_limiter_sun_core_step(&state, FP_SCENE_LIMITER_SCENE_CUSTOM, 2000000, 2, 0x101, false, false);
	assert(!r.applicable && state.retained == 1);
}

static void test_vote(void)
{
	struct fp_scene_limiter_vote_state state = {};
	u32 i;

	assert(fp_scene_limiter_estimate(&state, 500000, 100000, 1000000, 0, true) == 600000);
	assert(fp_scene_limiter_vote(&state, 600000) == 600000);
	assert(state.rising_writes == 1);
	assert(fp_scene_limiter_estimate(&state, 810001, 100000, 1000000, 0, true) == 1000000);
	assert(state.last_estimate_khz == 910001);
	assert(fp_scene_limiter_estimate(&state, 810001, 100000, 1000000, 1300000, true) == 1200000);
	assert(fp_scene_limiter_estimate(&state, 0, -100000, 1000000, 0, false) == 0);
	for (i = 0; i < 16; i++)
		assert(fp_scene_limiter_vote(&state, 620000) == 600000);
	assert(state.rising_writes == 0);
	assert(fp_scene_limiter_vote(&state, 620000) == 620000);
	assert(state.small_changes == 0 && state.rising_writes == 1);
	assert(fp_scene_limiter_vote(&state, 599999) == 599999);
	assert(state.rising_writes == 0);
	assert(fp_scene_limiter_estimate(&state, UINT32_MAX, 600000, 1000000, 0, true) == UINT32_MAX);
}

static void test_boost_pulses(void)
{
	struct fp_scene_limiter_pulse pulse = {};
	struct fp_scene_limiter_gesture gesture = {};
	u64 gesture_ns = fp_scene_limiter_gesture_duration(0);

	assert(gesture_ns == 500000000 && fp_scene_limiter_gesture_duration(720) == 720000000);
	assert(!fp_scene_limiter_gesture_axis(&gesture, 0, 20, 1, 1000, 1800));
	assert(!fp_scene_limiter_gesture_axis(&gesture, 1, 500, 1, 1000, 1800));
	assert(gesture.edge);
	assert(!fp_scene_limiter_gesture_axis(&gesture, 0, 79, 1, 1000, 1800));
	assert(fp_scene_limiter_gesture_axis(&gesture, 0, 80, 1, 1000, 1800));
	assert(gesture.moved && gesture.boosting);
	fp_scene_limiter_pulse_trigger(&pulse, 100, gesture_ns);
	assert(pulse.deadline_ns == gesture_ns + 100);
	assert(fp_scene_limiter_gesture_axis(&gesture, 1, 500, 1, 1000, 1800));
	fp_scene_limiter_pulse_trigger(&pulse, 200, gesture_ns);
	assert(pulse.deadline_ns == gesture_ns + 100 && pulse.pending_ns == gesture_ns);
	gesture.boosting = false; /* Release clears the latch independently of the pulse. */
	assert(fp_scene_limiter_pulse_advance(&pulse, gesture_ns + 100));
	assert(!pulse.pending_ns && pulse.deadline_ns == 2 * gesture_ns + 100);
	assert(!fp_scene_limiter_pulse_advance(&pulse, 2 * gesture_ns + 100));

	gesture = (struct fp_scene_limiter_gesture){};
	assert(!fp_scene_limiter_gesture_axis(&gesture, 0, 200, 1, 1000, 1800));
	assert(!fp_scene_limiter_gesture_axis(&gesture, 1, 500, 1, 1000, 1800));
	assert(!fp_scene_limiter_gesture_axis(&gesture, 0, 140, 1, 1000, 1800));
	assert(gesture.moved && !gesture.boosting);
	gesture = (struct fp_scene_limiter_gesture){};
	assert(!fp_scene_limiter_gesture_axis(&gesture, 0, 10, 1, 1000, 1800));
	assert(!fp_scene_limiter_gesture_axis(&gesture, 1, 300, 1, 1000, 1800));
	assert(!fp_scene_limiter_gesture_axis(&gesture, 1, 380, 2, 1000, 1800));
	assert(!fp_scene_limiter_gesture_axis(&gesture, 1, 450, 1, 1000, 1800));
	assert(!gesture.edge && !gesture.boosting);
	assert(fp_scene_limiter_deadline(UINT64_MAX - 10, 11) == UINT64_MAX);
}

static void test_notify(void)
{
	struct fp_scene_limiter_notify state = {};
	u64 now = 100, threshold = fp_scene_limiter_notify_threshold(60);
	u32 i;

	assert(threshold == 10000000 && fp_scene_limiter_notify_threshold(0) == threshold);
	assert(fp_scene_limiter_notify_threshold(90) == 8500000);
	assert(fp_scene_limiter_notify_threshold(120) == 6600000);
	assert(!fp_scene_limiter_notify_sample(&state, now, threshold, true, false));
	assert(!fp_scene_limiter_notify_sample(&state, now + threshold - 1, threshold, false, false));
	assert(fp_scene_limiter_notify_sample(&state, now + threshold, threshold, false, false));
	assert(!fp_scene_limiter_notify_sample(&state, now + threshold + 10, threshold, false, false));
	assert(fp_scene_limiter_notify_sample(&state, now + 2 * threshold, threshold, false, false));
	assert(!fp_scene_limiter_notify_sample(&state, now + 3 * threshold, threshold, false, false));
	assert(fp_scene_limiter_notify_sample(&state, now + 4 * threshold, threshold, false, true));
	assert(!state.running_ns && state.samples == 1);

	state = (struct fp_scene_limiter_notify){};
	for (i = 0; i < 32; i++) {
		assert(!fp_scene_limiter_notify_sample(&state, now, threshold, true, false));
		now += 1000000;
		assert(!fp_scene_limiter_notify_sample(&state, now++, threshold, false, true));
	}
	assert(state.samples == 32 && state.mean_ns == 1000000);
	assert(!fp_scene_limiter_notify_sample(&state, now, threshold, true, false));
	assert(fp_scene_limiter_notify_sample(&state, now + 6000001, threshold, false, true));
	assert(state.samples == 33 && state.mean_ns == 1625000);
	assert(!fp_scene_limiter_notify_sample(&state, now, threshold, false, false));
}

static void test_captured_profile_waveforms(void)
{
	/*
	 * Original scene-profile.json SHA256 701f71f5c1ce12519d24fe4c45cab2b33159859656671d451dc24eabd42e030a.
	 * p1, p2, p3, inactive, idle, in policy0/policy6 order. These are
	 * pre-OPP and pre-profile-clamp outputs for the native margin branch.
	 */
	static const u32 demand[] = { 400000, 850000, 1002000, 1545600, 1790000, 2400000, 650000 };
	static const struct {
		u32 count;
		struct fp_scene_limiter_margin_step steps[3];
		u32 want[sizeof(demand) / sizeof(demand[0])];
	} rows[] = {
		{ 3, { { 0, 150000 }, { 1152000, 100000 }, { 1785600, 70000 } },
		  { 550000, 1000000, 1102000, 1645600, 1860000, 2470000, 800000 } },
		{ 2, { { 0, 150000 }, { 1209600, 100000 } },
		  { 550000, 1000000, 1152000, 1645600, 1890000, 2500000, 800000 } },
		{ 3, { { 0, 200000 }, { 1555200, 130000 }, { 1996800, 90000 } },
		  { 560000, 1050000, 1202000, 1675600, 1920000, 2490000, 850000 } },
		{ 2, { { 0, 200000 }, { 1689600, 130000 } },
		  { 560000, 1050000, 1202000, 1675600, 1920000, 2530000, 850000 } },
		{ 3, { { 0, 200000 }, { 1785600, 130000 }, { 2400000, 70000 } },
		  { 560000, 1050000, 1202000, 1745600, 1920000, 2470000, 850000 } },
		{ 2, { { 0, 200000 }, { 1958400, 130000 } },
		  { 560000, 1050000, 1202000, 1745600, 1920000, 2530000, 850000 } },
		{ 2, { { 0, 120000 }, { 960000, 90000 } },
		  { 520000, 940000, 1092000, 1635600, 1880000, 2490000, 770000 } },
		{ 2, { { 0, 120000 }, { 1689600, 90000 } },
		  { 520000, 970000, 1122000, 1665600, 1880000, 2490000, 770000 } },
		{ 2, { { 0, 100000 }, { 960000, 80000 } },
		  { 500000, 950000, 1082000, 1625600, 1870000, 2480000, 750000 } },
		{ 2, { { 0, 100000 }, { 1689600, 80000 } },
		  { 500000, 950000, 1102000, 1645600, 1870000, 2480000, 750000 } },
	};
	const struct fp_scene_cpu legacy_p1 = {
		.limits = { 748800, 1996800 },
		.margin_count = 3,
		.margins = { { 0, 150 }, { 1152000, 100 }, { 1785600, 70 } },
	};
	u32 cpu, i;

	for (cpu = 0; cpu < sizeof(rows) / sizeof(rows[0]); cpu++)
		for (i = 0; i < sizeof(demand) / sizeof(demand[0]); i++)
			assert(demand[i] + fp_scene_limiter_margin(demand[i], rows[cpu].steps,
								 rows[cpu].count, 0) == rows[cpu].want[i]);
	assert(fp_scene_profile_ceiling(demand[1], &legacy_p1) == 1275000);
	assert(rows[0].want[1] == 1000000);
	assert(fp_scene_profile_ceiling(demand[2], &legacy_p1) == 1503000);
	assert(rows[0].want[2] == 1102000);
	assert(fp_scene_profile_ceiling(demand[4], &legacy_p1) == 1253000);
	assert(rows[0].want[4] == 1860000);
}

int main(void)
{
	test_aggregate();
	test_smoothing();
	test_margin();
	test_core();
	test_vote();
	test_boost_pulses();
	test_notify();
	test_captured_profile_waveforms();
	puts("Scene limiter: CPU exclusions and blend, smoothing, additive margins and source-gated sun cluster hysteresis passed");
	return 0;
}
