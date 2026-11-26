/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_EVENT_FEEDBACK_H
#define FRAME_SCENE_EVENT_FEEDBACK_H

#include "scene_limiter_feedback.h"

struct fp_scene_event_config {
	bool gesture_enabled;
	bool perf_notify_enabled;
	bool kswapd_enabled;
	u32 gesture_duration_ms;
};

struct fp_scene_event_state {
	u64 epoch;
	u64 last_input_ns;
	u64 gesture_duration_ns;
	u32 contacts;
	bool gesture_contact;
	bool watch_kswapd;
	struct fp_scene_limiter_gesture gesture;
	struct fp_scene_limiter_notify kswapd;
	struct fp_scene_limiter_pulse gesture_pulse;
	struct fp_scene_limiter_pulse notify_pulse;
};

struct fp_scene_event_snapshot {
	u64 last_input_ns;
	u64 next_deadline_ns;
	u32 contacts;
	bool gesture_active;
	bool notify_active;
	bool force_core;
};

/* The caller serializes each state instance and supplies monotonic event
 * times under that lock. Epoch zero disables it. Pass NULL when exiting an
 * eligible CPU feedback scene, including game, display-off and static policy.
 * Input events must come from the selected physical panel and the scheduler
 * predicates must identify the actual kswapd0 kernel thread.
 */
static inline void fp_scene_event_reset(struct fp_scene_event_state *state,
				       u64 epoch, const struct fp_scene_event_config *config)
{
	*state = (struct fp_scene_event_state){};
	if (!epoch || !config)
		return;
	state->epoch = epoch;
	state->watch_kswapd = config->perf_notify_enabled && config->kswapd_enabled;
	if (config->gesture_enabled)
		state->gesture_duration_ns = fp_scene_limiter_gesture_duration(config->gesture_duration_ms);
}

static inline bool fp_scene_event_valid(const struct fp_scene_event_state *state, u64 epoch)
{
	return epoch && state->epoch == epoch;
}

/* Release clears gesture recognition. A previously published pulse expires
 * on its own deadline and may have one successor queued by earlier motion.
 */
static inline void fp_scene_event_touch_contact(struct fp_scene_event_state *state,
						u64 epoch, u64 now_ns, u32 contacts, bool down)
{
	if (!fp_scene_event_valid(state, epoch) || now_ns < state->last_input_ns)
		return;
	state->last_input_ns = now_ns;
	state->contacts = contacts;
	if (down) {
		state->gesture = (struct fp_scene_limiter_gesture){};
		state->gesture_contact = contacts == 1;
	} else {
		state->gesture.boosting = false;
		state->gesture_contact = false;
	}
}

/* True indicates that the caller should read and publish the new deadline.
 * Adding a successor interval under an unchanged deadline needs no wakeup.
 */
static inline bool fp_scene_event_touch_axis(struct fp_scene_event_state *state,
					     u64 epoch, u64 now_ns, u32 contacts,
					     u32 axis, u32 coordinate, u32 max_x, u32 max_y)
{
	u64 old_deadline;

	if (!fp_scene_event_valid(state, epoch) || !state->gesture_duration_ns ||
	    now_ns < state->last_input_ns || axis > 1 || !max_x || !max_y ||
	    coordinate > (axis ? max_y : max_x))
		return false;
	state->last_input_ns = now_ns;
	state->contacts = contacts;
	if (contacts != 1) {
		state->gesture_contact = false;
		state->gesture.boosting = false;
	}
	if (!state->gesture_contact)
		return false;
	if (!fp_scene_limiter_gesture_axis(&state->gesture, axis, coordinate,
					  contacts, max_x, max_y))
		return false;
	old_deadline = state->gesture_pulse.deadline_ns;
	fp_scene_limiter_pulse_trigger(&state->gesture_pulse, now_ns, state->gesture_duration_ns);
	return state->gesture_pulse.deadline_ns != old_deadline;
}

/* Native 535478 reads the panel's hardware peak capability. The switch
 * observer can report a running kswapd0 on another CPU's sched_switch.
 */
static inline bool fp_scene_event_sched_switch(struct fp_scene_event_state *state,
					       u64 epoch, u64 now_ns, u32 panel_peak_hz,
					       bool prev_kswapd, bool next_kswapd)
{
	u64 old_deadline;

	if (!fp_scene_event_valid(state, epoch) || !state->watch_kswapd || !panel_peak_hz ||
	    (prev_kswapd && next_kswapd))
		return false;
	if (!fp_scene_limiter_notify_sample(&state->kswapd, now_ns,
					   fp_scene_limiter_notify_threshold(panel_peak_hz),
					   next_kswapd, prev_kswapd))
		return false;
	old_deadline = state->notify_pulse.deadline_ns;
	fp_scene_limiter_pulse_trigger(&state->notify_pulse, now_ns, FP_SCENE_LIMITER_NOTIFY_NS);
	return state->notify_pulse.deadline_ns != old_deadline;
}

/* Advance both pulses once, at a real event or the one-shot deadline. A
 * successor requires a real event observed during the previous interval.
 * A stale epoch receives an empty snapshot without touching current state.
 */
static inline struct fp_scene_event_snapshot fp_scene_event_snapshot(
		struct fp_scene_event_state *state, u64 epoch, u64 now_ns)
{
	struct fp_scene_event_snapshot snapshot = {};
	u64 gesture, notify;

	if (!fp_scene_event_valid(state, epoch))
		return snapshot;
	snapshot.gesture_active = fp_scene_limiter_pulse_advance(&state->gesture_pulse, now_ns);
	snapshot.notify_active = fp_scene_limiter_pulse_advance(&state->notify_pulse, now_ns);
	snapshot.force_core = snapshot.gesture_active;
	snapshot.contacts = state->contacts;
	snapshot.last_input_ns = state->last_input_ns;
	gesture = state->gesture_pulse.deadline_ns;
	notify = state->notify_pulse.deadline_ns;
	snapshot.next_deadline_ns = !gesture || (notify && notify < gesture) ? notify : gesture;
	return snapshot;
}

#endif /* FRAME_SCENE_EVENT_FEEDBACK_H */
