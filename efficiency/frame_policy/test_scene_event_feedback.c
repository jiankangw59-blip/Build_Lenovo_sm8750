/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>

#include "scene_event_feedback.h"

static const struct fp_scene_event_config active_config = {
	.gesture_enabled = true,
	.perf_notify_enabled = true,
	.kswapd_enabled = true,
};

static void test_live_gesture_and_stale_epoch(void)
{
	struct fp_scene_event_state state;
	struct fp_scene_event_snapshot snapshot;
	const u64 epoch = 7, start = 1000000, duration = 500000000;

	fp_scene_event_reset(&state, epoch, &active_config);
	assert(!fp_scene_event_touch_axis(&state, epoch, start, 1, 0, 10, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, epoch, start, 1, 1, 400, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, epoch, start, 1, 0, 100, 1000, 1800));
	assert(!state.gesture.origin[0]);
	fp_scene_event_touch_contact(&state, epoch - 1, start, 1, true);
	assert(!state.gesture_contact);
	fp_scene_event_touch_contact(&state, epoch, start, 1, true);
	assert(!fp_scene_event_touch_axis(&state, epoch, start, 1, 0, 10, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, epoch, start, 1, 1, 400, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, epoch, start, 1, 0, 69, 1000, 1800));
	assert(fp_scene_event_touch_axis(&state, epoch, start + 1, 1, 0, 70, 1000, 1800));
	snapshot = fp_scene_event_snapshot(&state, epoch, start + 1);
	assert(snapshot.gesture_active && snapshot.force_core && !snapshot.notify_active);
	assert(snapshot.contacts == 1 && snapshot.last_input_ns == start + 1);
	assert(snapshot.next_deadline_ns == start + 1 + duration);
	assert(!fp_scene_event_touch_axis(&state, epoch, start + 2, 1, 1, 401, 1000, 1800));
	assert(state.gesture_pulse.pending_ns == duration);
	fp_scene_event_touch_contact(&state, epoch, start + 3, 0, false);
	assert(!state.gesture.boosting);
	snapshot = fp_scene_event_snapshot(&state, epoch - 1, start + 1 + duration);
	assert(!snapshot.gesture_active && !snapshot.next_deadline_ns);
	assert(state.gesture_pulse.pending_ns == duration);
	snapshot = fp_scene_event_snapshot(&state, epoch, start + 1 + duration);
	assert(snapshot.gesture_active && !snapshot.contacts);
	assert(snapshot.next_deadline_ns == start + 1 + 2 * duration);
	assert(!state.gesture_pulse.pending_ns);
	snapshot = fp_scene_event_snapshot(&state, epoch, start + 1 + 2 * duration);
	assert(!snapshot.gesture_active && !snapshot.next_deadline_ns && !snapshot.force_core);
}

static void test_kswapd_and_earliest_deadline(void)
{
	struct fp_scene_event_state state;
	struct fp_scene_event_snapshot snapshot;
	const u64 epoch = 8, start = 3000000, duration = 500000000, slice = 6600000;

	fp_scene_event_reset(&state, epoch, &active_config);
	assert(!fp_scene_event_sched_switch(&state, epoch, start, 0, false, true));
	assert(!state.kswapd.running_ns);
	assert(!fp_scene_event_sched_switch(&state, epoch - 1, start, 185, false, true));
	assert(!state.kswapd.running_ns);
	assert(!fp_scene_event_sched_switch(&state, epoch, start, 185, false, true));
	assert(!fp_scene_event_sched_switch(&state, epoch, start + slice - 1, 185, false, false));
	assert(fp_scene_event_sched_switch(&state, epoch, start + slice, 185, false, false));
	fp_scene_event_touch_contact(&state, epoch, start + slice, 1, true);
	assert(!fp_scene_event_touch_axis(&state, epoch, start + slice, 1, 0, 5, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, epoch, start + slice, 1, 1, 700, 1000, 1800));
	assert(fp_scene_event_touch_axis(&state, epoch, start + slice, 1, 0, 65, 1000, 1800));
	snapshot = fp_scene_event_snapshot(&state, epoch, start + slice);
	assert(snapshot.gesture_active && snapshot.notify_active && snapshot.force_core);
	assert(snapshot.next_deadline_ns == start + slice + FP_SCENE_LIMITER_NOTIFY_NS);
	snapshot = fp_scene_event_snapshot(&state, epoch, start + slice + FP_SCENE_LIMITER_NOTIFY_NS);
	assert(snapshot.gesture_active && !snapshot.notify_active && snapshot.force_core);
	assert(snapshot.next_deadline_ns == start + slice + duration);
	fp_scene_event_reset(&state, epoch + 1, NULL);
	snapshot = fp_scene_event_snapshot(&state, epoch + 1, start + slice + duration);
	assert(!snapshot.gesture_active && !snapshot.notify_active && !snapshot.next_deadline_ns);
	assert(!fp_scene_event_sched_switch(&state, epoch, start + slice + duration, 185, true, false));
	assert(!state.kswapd.running_ns && !state.watch_kswapd && !state.gesture_contact);
}

static void test_late_expiry_and_disabled_options(void)
{
	struct fp_scene_event_state state;
	struct fp_scene_event_snapshot snapshot;
	struct fp_scene_event_config config = active_config;

	config.perf_notify_enabled = false;
	fp_scene_event_reset(&state, 9, &config);
	assert(!fp_scene_event_sched_switch(&state, 9, 100, 185, false, true));
	assert(!state.watch_kswapd && !state.kswapd.running_ns);
	fp_scene_event_touch_contact(&state, 9, 104, 1, true);
	fp_scene_event_touch_contact(&state, 9, 103, 0, false);
	assert(state.contacts == 1 && state.gesture_contact);
	assert(!fp_scene_event_touch_axis(&state, 9, 104, 1, 2, 1, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, 9, 104, 1, 0, 1001, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, 9, 104, 1, 0, 10, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, 9, 104, 1, 1, 400, 1000, 1800));
	assert(fp_scene_event_touch_axis(&state, 9, 105, 1, 0, 90, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, 9, 106, 1, 1, 401, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, 9, 107, 2, 1, 402, 1000, 1800));
	assert(!fp_scene_event_touch_axis(&state, 9, 108, 1, 1, 403, 1000, 1800));
	snapshot = fp_scene_event_snapshot(&state, 9, 1100000000);
	assert(!snapshot.gesture_active && !snapshot.next_deadline_ns);
}

static void test_kswapd_switch_out_boundary(void)
{
	struct fp_scene_event_state state;
	struct fp_scene_event_snapshot snapshot;
	const u64 epoch = 10, start = 3000000, slice = 6600000;
	const u64 next_start = start + slice + 1;

	fp_scene_event_reset(&state, epoch, &active_config);
	assert(!fp_scene_event_sched_switch(&state, epoch, start, 185, false, true));
	assert(!fp_scene_event_sched_switch(&state, epoch, start + slice, 185, true, false));
	assert(!state.kswapd.running_ns && !state.notify_pulse.deadline_ns);
	assert(!fp_scene_event_sched_switch(&state, epoch, next_start, 185, false, true));
	assert(fp_scene_event_sched_switch(&state, epoch, next_start + slice + 1, 185, true, false));
	snapshot = fp_scene_event_snapshot(&state, epoch, next_start + slice + 1);
	assert(snapshot.notify_active && !snapshot.force_core);
	assert(snapshot.next_deadline_ns == next_start + slice + 1 + FP_SCENE_LIMITER_NOTIFY_NS);
}

int main(void)
{
	test_live_gesture_and_stale_epoch();
	test_kswapd_and_earliest_deadline();
	test_late_expiry_and_disabled_options();
	test_kswapd_switch_out_boundary();
	puts("Scene event feedback: trusted inputs, epoch invalidation and one-shot deadlines passed");
	return 0;
}
