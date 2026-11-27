/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_FAS_FRAMES_H
#define FRAME_FAS_FRAMES_H
#include "fas_control.h"

/* Independent submission history and loss-detecting handoff. No timers or IO. */
#define FP_FAS_HISTORY 1200U /* Five seconds at the largest supported 240 FPS. */
#define FP_FAS_EVENTS 128U
#define FP_FAS_MIN_INTERVALS 60U

struct fp_fas_history {
	u64 intervals[FP_FAS_HISTORY];
	u64 sum_ns;
	u32 head, count;
};
struct fp_fas_event {
	u64 ended_ns, interval_ns;
	__u16 fps;
	bool janked, late;
	u32 margin_mfps;
};
struct fp_fas_events {
	struct fp_fas_event events[FP_FAS_EVENTS];
	u32 head, count;
};

static inline void fp_fas_history_reset(struct fp_fas_history *history)
{
	/* Old values outside count need not be touched on a frame hot path. */
	history->head = history->count = 0;
	history->sum_ns = 0;
}

static inline bool fp_fas_history_push(struct fp_fas_history *history, u64 interval, u32 fps)
{
	u32 limit = fps && fps <= 240 ? fps * 5 : 720;

	if (!interval || interval >= 1000000000ULL) {
		fp_fas_history_reset(history);
		return false;
	}
	while (history->count >= limit) {
		history->sum_ns -= history->intervals[history->head];
		history->head = (history->head + 1) % FP_FAS_HISTORY;
		history->count--;
	}
	history->intervals[(history->head + history->count) % FP_FAS_HISTORY] = interval;
	history->sum_ns += interval;
	history->count++;
	return true;
}

static inline bool fp_fas_history_janked_elapsed(const struct fp_fas_history *history,
						 u32 fps, u64 additional_ns)
{
	/* Compare N/elapsed with T-2 without rounding away the two-FPS boundary. */
	return history->count && history->sum_ns && fps > 2 &&
	       (u64)history->count * 1000000000ULL < (history->sum_ns + additional_ns) * (fps - 2);
}

static inline bool fp_fas_history_janked(const struct fp_fas_history *history, u32 fps)
{
	return fp_fas_history_janked_elapsed(history, fps, 0);
}

static inline bool fp_fas_event_push(struct fp_fas_events *queue, struct fp_fas_event event)
{
	if (queue->count == FP_FAS_EVENTS)
		return false;
	queue->events[(queue->head + queue->count) % FP_FAS_EVENTS] = event;
	queue->count++;
	return true;
}

static inline bool fp_fas_event_pop(struct fp_fas_events *queue, struct fp_fas_event *event)
{
	if (!queue->count)
		return false;
	*event = queue->events[queue->head];
	queue->head = (queue->head + 1) % FP_FAS_EVENTS;
	queue->count--;
	return true;
}
#endif
