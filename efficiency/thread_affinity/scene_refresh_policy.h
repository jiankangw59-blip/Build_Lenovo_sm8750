/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SCENE_REFRESH_POLICY_H
#define SCENE_REFRESH_POLICY_H

/*
 * Independently authored, side-effect-free Scene display policy.
 *
 * The caller owns immutable configuration/rules and one resolution-group mode
 * catalog with one unambiguous mode for each integer Hz. Validate a replacement
 * catalog once before publishing it. The input
 * adapter supplies Scene-equivalent gesture classification; this header does
 * not infer gestures from raw evdev codes, poll, read files, or issue Binder.
 * Bump context generation for app/config/display ownership changes. Cancel a
 * pending deadline on new activity; releases may merge absolute deadlines.
 *
 * Original package min/max values are never modified. LTPO/ramp/compatible
 * requests are execution requirements, not a claim those backends succeeded.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

enum srp_result {
	SRP_OK = 0,
	SRP_DISABLED,
	SRP_NO_CONTEXT,
	SRP_INVALID,
	SRP_UNSUPPORTED_VIDEO,
	SRP_STALE,
	SRP_NOT_DUE,
	SRP_NO_DEADLINE,
};

enum srp_level { SRP_HIGH, SRP_MIDDLE, SRP_LOW, SRP_PEAK };

enum srp_source {
	SRP_SOURCE_ACTIVE,
	SRP_SOURCE_TOUCH,
	SRP_SOURCE_INACTIVE,
	SRP_SOURCE_SCREEN_OFF,
};

/* Values deliberately match the original daemon reason values. */
enum srp_reason {
	SRP_REASON_NONE = 0,
	SRP_REASON_GESTURE = 2,
	SRP_REASON_LOW_BATTERY = 4,
	SRP_REASON_LOW_BRIGHTNESS = 6,
	SRP_REASON_SCREEN_OFF = 8,
	SRP_REASON_APP_MAX = 10,
	SRP_REASON_APP_MIN = 12,
};

enum srp_execution_flags {
	SRP_EXEC_DIRECT_1035 = 1U << 0,
	SRP_EXEC_COMPATIBLE = 1U << 1,
	SRP_EXEC_LTPO_REASSERT = 1U << 2,
	SRP_EXEC_RAMP = 1U << 3,
};

struct srp_mode {
	int32_t mode_id;
	uint32_t hz;
	uint32_t width;
	uint32_t height;
};

struct srp_catalog {
	const struct srp_mode *modes;
	size_t count;
	int32_t high_mode_id;
	int32_t middle_mode_id;
	int32_t low_mode_id;
	int32_t peak_mode_id;
};

struct srp_config {
	bool enabled;
	enum srp_level apps_default;
	enum srp_level apps_inactive;
	enum srp_level games_default;
	enum srp_level gesture;
	enum srp_level screen_off;
	enum srp_level low_battery_mode;
	enum srp_level low_brightness;
	int32_t low_battery_threshold;
	int32_t brightness_threshold;
	int32_t tap_delay_ms;
	int32_t scroll_delay_ms;
	bool strict;
	bool compatible;
	bool disable_ltpo;
	bool video_optimize;
	uint32_t normal_peak_hz;
	uint32_t oc_step_interval_ms;
};

struct srp_app_rule {
	const char *package;
	int32_t min_hz;
	int32_t max_hz;
};

struct srp_context {
	uint64_t generation;
	bool valid;
	bool game;
	bool launcher;
	bool screen_off;
	bool standby;
	bool video;
};

struct srp_environment {
	/* -1 or >100 denotes unknown, never zero brightness/battery. */
	int32_t brightness_percent;
	int32_t battery_percent;
	bool charging;
	/* 0 means no confirmed current mode; used only for ramp requirements. */
	uint32_t current_hz;
};

struct srp_range {
	int32_t min_mode_id;
	int32_t max_mode_id;
	bool dynamic;
	bool explicit_rule;
};

struct srp_state {
	uint64_t generation;
	uint64_t deadline_token;
	uint64_t deadline_ms;
	bool initialized;
	bool deadline_valid;
};

struct srp_decision {
	bool apply;
	int32_t mode_id;
	uint32_t hz;
	enum srp_reason reason;
	bool force;
	uint32_t execution_flags;
	uint32_t oc_step_interval_ms;
	uint64_t generation;
	bool deadline_valid;
	uint64_t deadline_ms;
	uint64_t deadline_token;
};

static inline void srp_config_default(struct srp_config *out)
{
	/* Enable and platform-dependent strict/compatible are supplied by caller. */
	memset(out, 0, sizeof(*out));
	out->apps_default = SRP_HIGH;
	out->apps_inactive = SRP_LOW;
	out->games_default = SRP_HIGH;
	out->gesture = SRP_HIGH;
	out->screen_off = SRP_HIGH;
	out->low_battery_mode = SRP_LOW;
	out->low_brightness = SRP_HIGH;
	out->low_battery_threshold = 5;
	out->tap_delay_ms = 900;
	out->scroll_delay_ms = 2000;
	out->normal_peak_hz = 240;
	out->oc_step_interval_ms = 600;
}

static inline const struct srp_mode *srp_mode_by_id(
		const struct srp_catalog *catalog, int32_t id)
{
	size_t i;

	if (!catalog || !catalog->modes)
		return NULL;
	for (i = 0; i < catalog->count; i++)
		if (catalog->modes[i].mode_id == id)
			return &catalog->modes[i];
	return NULL;
}

static inline const struct srp_mode *srp_mode_by_level(
		const struct srp_catalog *catalog, enum srp_level level)
{
	if (!catalog)
		return NULL;
	switch (level) {
	case SRP_HIGH: return srp_mode_by_id(catalog, catalog->high_mode_id);
	case SRP_MIDDLE: return srp_mode_by_id(catalog, catalog->middle_mode_id);
	case SRP_LOW: return srp_mode_by_id(catalog, catalog->low_mode_id);
	case SRP_PEAK: return srp_mode_by_id(catalog, catalog->peak_mode_id);
	}
	return NULL;
}

static inline enum srp_result srp_validate_catalog(
		const struct srp_catalog *catalog)
{
	size_t i, j;

	if (!catalog || !catalog->modes || !catalog->count)
		return SRP_INVALID;
	for (i = 0; i < catalog->count; i++) {
		const struct srp_mode *mode = &catalog->modes[i];

		if (mode->mode_id < 0 || !mode->hz || !mode->width || !mode->height ||
		    mode->width != catalog->modes[0].width ||
		    mode->height != catalog->modes[0].height)
			return SRP_INVALID;
		for (j = 0; j < i; j++)
			if (mode->mode_id == catalog->modes[j].mode_id ||
			    mode->hz == catalog->modes[j].hz)
				return SRP_INVALID;
	}
	return srp_mode_by_level(catalog, SRP_HIGH) &&
	       srp_mode_by_level(catalog, SRP_MIDDLE) &&
	       srp_mode_by_level(catalog, SRP_LOW) &&
	       srp_mode_by_level(catalog, SRP_PEAK) ? SRP_OK : SRP_INVALID;
}

static inline enum srp_result srp_validate_config(const struct srp_config *config)
{
	if (!config || (unsigned int)config->apps_default > SRP_PEAK ||
	    (unsigned int)config->apps_inactive > SRP_PEAK ||
	    (unsigned int)config->games_default > SRP_PEAK ||
	    (unsigned int)config->gesture > SRP_PEAK ||
	    (unsigned int)config->screen_off > SRP_PEAK ||
	    (unsigned int)config->low_battery_mode > SRP_PEAK ||
	    (unsigned int)config->low_brightness > SRP_PEAK)
		return SRP_INVALID;
	return SRP_OK;
}

/* Scene aliases are inserted before native per-Hz modes (53dd50..53dea8). */
static inline const struct srp_mode *srp_mode_for_rule_hz(
		const struct srp_catalog *catalog, int32_t hz)
{
	size_t i;

	if (!catalog || !catalog->modes || hz <= 0)
		return NULL;
	if (hz == 120)
		return srp_mode_by_level(catalog, SRP_HIGH);
	if (hz == 90)
		return srp_mode_by_level(catalog, SRP_MIDDLE);
	if (hz == 60)
		return srp_mode_by_level(catalog, SRP_LOW);
	for (i = 0; i < catalog->count; i++)
		if (catalog->modes[i].hz == (uint32_t)hz)
			return &catalog->modes[i];
	return NULL;
}

static inline const struct srp_app_rule *srp_find_app_rule(
		const struct srp_app_rule *rules, size_t count, const char *package)
{
	size_t i;

	if (!rules || !package || !*package)
		return NULL;
	for (i = 0; i < count; i++)
		if (rules[i].package && !strcmp(rules[i].package, package))
			return &rules[i];
	return NULL;
}

static inline enum srp_result srp_effective_range(
		const struct srp_config *config, const struct srp_catalog *catalog,
		const struct srp_app_rule *rule, const struct srp_context *context,
		struct srp_range *out)
{
	const struct srp_mode *minimum, *maximum;
	enum srp_level active;

	if (!out || !context || srp_validate_config(config) != SRP_OK)
		return SRP_INVALID;
	memset(out, 0, sizeof(*out));
	if (!context->valid)
		return SRP_NO_CONTEXT;
	minimum = srp_mode_by_level(catalog, config->apps_inactive);
	if (rule) {
		maximum = srp_mode_for_rule_hz(catalog, rule->max_hz);
		if (!maximum)
			maximum = srp_mode_by_level(catalog, config->apps_default);
		if (rule->min_hz) {
			minimum = srp_mode_for_rule_hz(catalog, rule->min_hz);
			/* Reject unsupported input instead of inventing mode id zero. */
			if (!minimum)
				return SRP_INVALID;
		}
	} else {
		if (context->launcher)
			active = config->gesture;
		else if (context->game)
			active = config->games_default;
		else if (context->standby) {
			active = config->screen_off;
			minimum = srp_mode_by_level(catalog, active);
		} else
			active = config->apps_default;
		maximum = srp_mode_by_level(catalog, active);
	}
	if (!minimum || !maximum)
		return SRP_INVALID;
	out->max_mode_id = maximum->mode_id;
	out->min_mode_id = context->game ? maximum->mode_id : minimum->mode_id;
	out->dynamic = out->max_mode_id != out->min_mode_id;
	out->explicit_rule = rule != NULL;
	return SRP_OK;
}

static inline void srp_cancel_deadline(struct srp_state *state)
{
	state->deadline_valid = false;
	state->deadline_ms = 0;
	/* Token zero is reserved for an event which did not capture a deadline. */
	if (++state->deadline_token == 0)
		state->deadline_token = 1;
}

static inline void srp_set_context(struct srp_state *state, uint64_t generation)
{
	if (!state->initialized || state->generation != generation) {
		srp_cancel_deadline(state);
		state->generation = generation;
		state->initialized = true;
	}
}

static inline void srp_decision_snapshot(const struct srp_state *state,
		struct srp_decision *out)
{
	memset(out, 0, sizeof(*out));
	if (state && state->initialized) {
		out->generation = state->generation;
		out->deadline_valid = state->deadline_valid;
		out->deadline_ms = state->deadline_ms;
		out->deadline_token = state->deadline_token;
	}
}

static inline bool srp_low_brightness(const struct srp_config *config,
		const struct srp_environment *environment)
{
	return config->brightness_threshold > 0 &&
	       config->brightness_threshold <= 100 &&
	       environment->brightness_percent >= 0 &&
	       environment->brightness_percent <= 100 &&
	       environment->brightness_percent < config->brightness_threshold;
}

/* gesture_fast_path is supplied by the existing gesture/input adapter. */
static inline enum srp_result srp_decide(const struct srp_config *config,
		const struct srp_catalog *catalog, const struct srp_app_rule *rule,
		const struct srp_context *context,
		const struct srp_environment *environment, enum srp_source source,
		bool gesture_fast_path, const struct srp_state *state,
		struct srp_decision *out)
{
	const struct srp_mode *chosen, *cap;
	struct srp_range range;
	enum srp_reason reason;
	enum srp_result result;

	if (!out)
		return SRP_INVALID;
	srp_decision_snapshot(state, out);
	if (!config || !context || !environment ||
	    srp_validate_config(config) != SRP_OK ||
	    (unsigned int)source > SRP_SOURCE_SCREEN_OFF)
		return SRP_INVALID;
	if (!config->enabled)
		return SRP_DISABLED;
	if (!context->valid)
		return SRP_NO_CONTEXT;
	if (!state || !state->initialized || state->generation != context->generation)
		return SRP_STALE;
	if (source == SRP_SOURCE_INACTIVE && context->video && config->video_optimize)
		return SRP_UNSUPPORTED_VIDEO;
	result = srp_effective_range(config, catalog, rule, context, &range);
	if (result != SRP_OK)
		return result;
	if (source == SRP_SOURCE_SCREEN_OFF) {
		chosen = srp_mode_by_level(catalog, config->screen_off);
		reason = SRP_REASON_SCREEN_OFF;
	} else if (source == SRP_SOURCE_TOUCH && gesture_fast_path) {
		chosen = srp_mode_by_level(catalog, config->gesture);
		reason = SRP_REASON_GESTURE;
	} else if (srp_low_brightness(config, environment)) {
		chosen = srp_mode_by_level(catalog, config->low_brightness);
		reason = SRP_REASON_LOW_BRIGHTNESS;
	} else if (context->screen_off || context->standby) {
		chosen = srp_mode_by_level(catalog, config->screen_off);
		reason = SRP_REASON_SCREEN_OFF;
	} else if (source == SRP_SOURCE_INACTIVE) {
		chosen = srp_mode_by_id(catalog, range.min_mode_id);
		reason = SRP_REASON_APP_MIN;
	} else if (context->launcher) {
		chosen = srp_mode_by_level(catalog, config->gesture);
		reason = SRP_REASON_GESTURE;
	} else {
		chosen = srp_mode_by_id(catalog, range.max_mode_id);
		reason = SRP_REASON_APP_MAX;
	}
	if (!chosen)
		return SRP_INVALID;
	cap = srp_mode_by_level(catalog, config->low_battery_mode);
	if (!cap)
		return SRP_INVALID;
	if (config->low_battery_threshold > 0 && !environment->charging &&
	    environment->battery_percent >= 0 && environment->battery_percent <= 100 &&
	    environment->battery_percent <= config->low_battery_threshold &&
	    cap->hz < chosen->hz) {
		chosen = cap;
		reason = SRP_REASON_LOW_BATTERY;
	}
	out->apply = true;
	out->mode_id = chosen->mode_id;
	out->hz = chosen->hz;
	out->reason = reason;
	out->force = config->strict && reason != SRP_REASON_GESTURE && !context->game;
	out->execution_flags = config->compatible ? SRP_EXEC_COMPATIBLE : SRP_EXEC_DIRECT_1035;
	if (config->disable_ltpo)
		out->execution_flags |= SRP_EXEC_LTPO_REASSERT;
	if (!config->compatible &&
	    (chosen->hz > config->normal_peak_hz ||
	     environment->current_hz > config->normal_peak_hz))
		out->execution_flags |= SRP_EXEC_RAMP;
	out->oc_step_interval_ms = config->oc_step_interval_ms;
	return SRP_OK;
}

static inline uint64_t srp_add_ms(uint64_t a, uint64_t b)
{
	return UINT64_MAX - a < b ? UINT64_MAX : a + b;
}

static inline uint64_t srp_inactive_at(const struct srp_config *config,
		bool launcher, uint64_t now_ms, uint64_t last_touch_ms,
		uint64_t last_scroll_ms, bool scroll_release)
{
	uint64_t tap = config->tap_delay_ms > 0 ? (uint64_t)config->tap_delay_ms : 900;
	uint64_t scroll = config->scroll_delay_ms > 0 ?
		(uint64_t)config->scroll_delay_ms : 2000;
	uint64_t base = srp_add_ms(last_touch_ms, 1000), candidate, cap;

	if (launcher) {
		candidate = srp_add_ms(now_ms, scroll > 2000 ? scroll : 2000);
		if (candidate > base)
			base = candidate;
	}
	candidate = srp_add_ms(last_scroll_ms, scroll);
	if (candidate > base)
		base = candidate;
	cap = srp_add_ms(now_ms, scroll_release ? scroll : tap);
	if (base > cap)
		base = cap;
	return base > now_ms ? base : now_ms;
}

/* Caller calls this on release with zero remaining touches, not periodically. */
static inline enum srp_result srp_schedule_inactive(struct srp_state *state,
		const struct srp_config *config, const struct srp_context *context,
		uint64_t now_ms, uint64_t last_touch_ms, uint64_t last_scroll_ms,
		bool scroll_release, struct srp_decision *out)
{
	uint64_t deadline;

	if (!out)
		return SRP_INVALID;
	srp_decision_snapshot(state, out);
	if (!state || !context || srp_validate_config(config) != SRP_OK)
		return SRP_INVALID;
	if (!state->initialized || state->generation != context->generation)
		return SRP_STALE;
	if (!config->enabled || !context->valid) {
		srp_cancel_deadline(state);
		srp_decision_snapshot(state, out);
		return config->enabled ? SRP_NO_CONTEXT : SRP_DISABLED;
	}
	if (context->screen_off || context->standby) {
		srp_cancel_deadline(state);
		srp_decision_snapshot(state, out);
		return SRP_NO_DEADLINE;
	}
	/* Future input timestamps mean the producer changed its clock epoch. */
	if (last_touch_ms > now_ms || last_scroll_ms > now_ms)
		return SRP_INVALID;
	deadline = srp_inactive_at(config, context->launcher, now_ms,
		last_touch_ms, last_scroll_ms, scroll_release);
	if (state->deadline_valid && state->deadline_ms > deadline)
		deadline = state->deadline_ms;
	srp_cancel_deadline(state);
	state->deadline_ms = deadline;
	state->deadline_valid = true;
	srp_decision_snapshot(state, out);
	return SRP_OK;
}

/* Consumes a matching one-shot token once. No rearming or catch-up loop. */
static inline enum srp_result srp_take_inactive(struct srp_state *state,
		uint64_t generation, uint64_t deadline_token, uint64_t now_ms)
{
	if (!state || !state->initialized || state->generation != generation)
		return SRP_STALE;
	if (!state->deadline_valid)
		return SRP_NO_DEADLINE;
	if (state->deadline_token != deadline_token)
		return SRP_STALE;
	if (now_ms < state->deadline_ms)
		return SRP_NOT_DUE;
	srp_cancel_deadline(state);
	return SRP_OK;
}

#endif /* SCENE_REFRESH_POLICY_H */
