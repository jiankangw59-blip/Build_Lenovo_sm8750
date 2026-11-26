/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SCENE_ACTIVITY_POLICY_H
#define SCENE_ACTIVITY_POLICY_H

/*
 * Pure event/deadline adapter for Scene CPU/GPU active/inactive state.
 * Native evidence: 57b54c -> 52c270; input 55a3a0 -> 52c900;
 * idle 52ca50 -> 52cc60 -> 52c190. This is NOT refresh tap_delay/scroll_delay.
 *
 * No input fd, task, workqueue, timer, scheduler/thermal write, or poll is owned
 * here. The caller supplies reliable aggregate contacts, classified context,
 * and a KNOWN load qualification from an existing provider. Unknown load is
 * explicit; it never silently becomes permission to enter inactive.
 *
 * Context epoch MUST include package, complete visible/windowed sets, mode,
 * source validity, screen and game ownership. This adapter neither replaces
 * those sets with a UID nor attributes touches to only the foreground UID.
 * Caller bumps the epoch for a semantic config/context change, never a heartbeat
 * or timestamp-only file rewrite, and cancels its old one-shot timer.
 */
#ifdef __KERNEL__
#include <linux/types.h>
typedef u64 sa_u64;
typedef u32 sa_u32;
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint64_t sa_u64;
typedef uint32_t sa_u32;
#endif

enum sa_owner { SA_OWNER_NONE, SA_OWNER_SCENE, SA_OWNER_FRAME };
enum sa_phase { SA_NONE, SA_OFF, SA_FRAME, SA_ACTIVE, SA_QUALIFY, SA_INACTIVE };
enum sa_event_kind { SA_CONTEXT, SA_INPUT, SA_RESYNC, SA_SOURCE_LOST, SA_LOAD, SA_DEADLINE };
enum sa_result { SA_OK, SA_STALE, SA_NOT_DUE, SA_NO_DEADLINE, SA_INVALID, SA_TIME_REVERSED };

struct sa_config {
	sa_u32 initial_idle_ms;
	sa_u32 input_idle_ms;
	sa_u32 launcher_idle_ms;
};

struct sa_context {
	/* Monotonic source epoch, including visible/windowed-set changes. */
	sa_u64 epoch;
	bool enabled;
	bool valid;
	bool screen_off;
	bool game;
	bool launcher;
};

struct sa_event {
	enum sa_event_kind kind;
	sa_u64 now_ms;
	/* Deadline AND asynchronous load replies use their captured request token. */
	sa_u64 token;
	sa_u32 contacts;
	/* SA_CONTEXT/SA_RESYNC require an actual synchronized input snapshot. */
	bool input_reliable;
	bool load_known;
	sa_u32 load_permille;
};

struct sa_state {
	struct sa_context context;
	enum sa_phase phase;
	bool initialized;
	bool input_reliable;
	sa_u32 contacts;
	sa_u64 last_event_ms;
	sa_u64 token;
	sa_u64 deadline_ms;
	bool deadline_valid;
	sa_u64 qualify_start_ms;
};

struct sa_output {
	enum sa_owner owner;
	bool publish;
	bool off;
	bool inactive;
	bool needs_input_resync;
	bool needs_load;
	sa_u64 epoch;
	bool deadline_valid;
	sa_u64 deadline_ms;
	sa_u64 token;
};

static inline struct sa_config sa_native_defaults(void)
{
	/* Original initial check=1s; subsequent app=2s, Launcher=3s. Game is FAS. */
	return (struct sa_config){1000, 2000, 3000};
}

static inline sa_u64 sa_add_ms(sa_u64 a, sa_u64 b)
{
	sa_u64 maximum = ~(sa_u64)0;
	return maximum - a < b ? maximum : a + b;
}

/* Constant-load bounds from native 52c190; comparisons are strictly greater. */
static inline sa_u32 sa_native_load_hold_ms(sa_u32 load_permille)
{
	if (load_permille > 950) return 6000;
	if (load_permille > 850) return 5000;
	if (load_permille > 750) return 4000;
	if (load_permille > 650) return 3000;
	return 0;
}

static inline void sa_cancel(struct sa_state *state)
{
	state->deadline_valid = false;
	state->deadline_ms = 0;
	if (++state->token == 0) state->token = 1;
}

static inline void sa_arm(struct sa_state *state, sa_u64 at_ms)
{
	sa_cancel(state);
	state->deadline_ms = at_ms;
	state->deadline_valid = true;
}

static inline enum sa_owner sa_state_owner(const struct sa_state *state)
{
	if (!state->initialized || state->phase == SA_NONE) return SA_OWNER_NONE;
	return state->phase == SA_FRAME ? SA_OWNER_FRAME : SA_OWNER_SCENE;
}

static inline struct sa_output sa_snapshot(const struct sa_state *state)
{
	struct sa_output out = {0};

	if (!state) return out;
	out.owner = sa_state_owner(state);
	out.off = state->phase == SA_OFF;
	out.inactive = state->phase == SA_INACTIVE;
	out.epoch = state->context.epoch;
	out.needs_input_resync = out.owner == SA_OWNER_SCENE && !out.off && !state->input_reliable;
	out.needs_load = state->phase == SA_QUALIFY && !state->deadline_valid;
	out.deadline_valid = state->deadline_valid;
	out.deadline_ms = state->deadline_ms;
	out.token = state->token;
	return out;
}

static inline bool sa_context_equal(const struct sa_context *a, const struct sa_context *b)
{
	return a->epoch == b->epoch && a->enabled == b->enabled && a->valid == b->valid &&
	       a->screen_off == b->screen_off && a->game == b->game && a->launcher == b->launcher;
}

static inline void sa_qualify(struct sa_state *state, const struct sa_event *event)
{
	sa_u64 until;

	if (!event->load_known) return;
	until = sa_add_ms(state->qualify_start_ms, sa_native_load_hold_ms(event->load_permille));
	if (event->now_ms >= until) {
		state->phase = SA_INACTIVE;
		sa_cancel(state);
	} else {
		sa_arm(state, until);
	}
}

static inline enum sa_result sa_apply(struct sa_state *state, const struct sa_config *config,
		const struct sa_context *context, const struct sa_event *event,
		struct sa_output *out)
{
	struct sa_output previous;
	enum sa_result result = SA_OK;

	if (!state || !config || !context || !event || !out ||
	    (unsigned int)event->kind > SA_DEADLINE)
		return SA_INVALID;
	previous = sa_snapshot(state);
	*out = previous;
	if (event->kind != SA_CONTEXT && (!state->initialized ||
	    !sa_context_equal(&state->context, context))) return SA_STALE;
	if (event->kind == SA_CONTEXT && state->initialized && context->epoch < state->context.epoch)
		return SA_STALE;
	if (event->kind == SA_CONTEXT && state->initialized &&
	    context->epoch == state->context.epoch) {
		return sa_context_equal(&state->context, context) ? SA_OK : SA_INVALID;
	}
	if (state->initialized && event->now_ms < state->last_event_ms)
		return SA_TIME_REVERSED;
	if (event->kind == SA_CONTEXT) {
		sa_cancel(state);
		state->context = *context;
		state->initialized = true;
		state->contacts = event->contacts;
		state->input_reliable = event->input_reliable;
		state->qualify_start_ms = 0;
		if (!context->enabled || !context->valid) state->phase = SA_NONE;
		else if (context->screen_off) state->phase = SA_OFF;
		else if (context->game) state->phase = SA_FRAME;
		else {
			state->phase = SA_ACTIVE;
			if (state->input_reliable && !state->contacts)
				sa_arm(state, sa_add_ms(event->now_ms, config->initial_idle_ms));
		}
		goto done;
	}
	if (state->phase == SA_NONE || state->phase == SA_OFF || state->phase == SA_FRAME)
		goto done;
	switch (event->kind) {
	case SA_SOURCE_LOST:
		state->input_reliable = false;
		state->phase = SA_ACTIVE;
		sa_cancel(state);
		break;
	case SA_RESYNC:
		state->input_reliable = event->input_reliable;
		/* Only synchronized state can authorize idle again. */
		goto activity;
	case SA_INPUT:
	activity:
		state->phase = SA_ACTIVE;
		sa_cancel(state);
		state->qualify_start_ms = 0;
		if (state->input_reliable) {
			state->contacts = event->contacts;
			if (!state->contacts)
				sa_arm(state, sa_add_ms(event->now_ms, context->launcher ?
					config->launcher_idle_ms : config->input_idle_ms));
		}
		break;
	case SA_LOAD:
		if (state->phase == SA_QUALIFY) {
			if (state->token != event->token) { result = SA_STALE; break; }
			sa_qualify(state, event);
		}
		break;
	case SA_DEADLINE:
		if (!state->deadline_valid) { result = SA_NO_DEADLINE; break; }
		if (state->token != event->token) { result = SA_STALE; break; }
		if (event->now_ms < state->deadline_ms) { result = SA_NOT_DUE; break; }
		sa_cancel(state);
		if (state->phase == SA_ACTIVE) {
			state->phase = SA_QUALIFY;
			state->qualify_start_ms = event->now_ms;
			sa_qualify(state, event);
		} else if (state->phase == SA_QUALIFY) {
			/* A known qualification has an absolute <=6s bound; no polling. */
			if (event->load_known) sa_qualify(state, event);
			else state->phase = SA_INACTIVE;
		}
		break;
	case SA_CONTEXT:
		break;
	}
done:
	if (result == SA_OK) state->last_event_ms = event->now_ms;
	*out = sa_snapshot(state);
	out->publish = previous.owner != out->owner || previous.epoch != out->epoch ||
		previous.off != out->off || previous.inactive != out->inactive;
	return result;
}

#endif /* SCENE_ACTIVITY_POLICY_H */
