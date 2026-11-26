/* SPDX-License-Identifier: GPL-2.0 */
/* Host arithmetic/state-machine tests; never accesses an Android device. */
#include <assert.h>
#include <stdio.h>
#include "scene_refresh_policy.h"

static const struct srp_mode modes[] = {
	{0, 120, 1272, 2800}, {1, 60, 1272, 2800},
	{2, 90, 1272, 2800}, {3, 144, 1272, 2800},
	{4, 165, 1272, 2800}, {5, 170, 1272, 2800},
	{6, 175, 1272, 2800}, {7, 180, 1272, 2800},
	{8, 185, 1272, 2800},
};
static const struct srp_catalog catalog = {modes, 9, 0, 2, 1, 8};
static const struct srp_app_rule game_rule = {"test.game", 120, 165};

struct fixture {
	struct srp_config config;
	struct srp_context context;
	struct srp_environment environment;
	struct srp_state state;
	struct srp_decision decision;
};

static struct fixture fixture(void)
{
	struct fixture f = {0};

	srp_config_default(&f.config);
	f.config.enabled = true;
	f.context.valid = true;
	f.context.generation = 1;
	f.environment.brightness_percent = 50;
	f.environment.battery_percent = 80;
	f.environment.current_hz = 120;
	srp_set_context(&f.state, 1);
	return f;
}

static void choose(struct fixture *f, const struct srp_app_rule *rule,
		enum srp_source source, bool gesture, int32_t mode_id,
		uint32_t hz, enum srp_reason reason)
{
	assert(srp_decide(&f->config, &catalog, rule, &f->context,
		&f->environment, source, gesture, &f->state, &f->decision) == SRP_OK);
	assert(f->decision.apply);
	assert(f->decision.mode_id == mode_id);
	assert(f->decision.hz == hz);
	assert(f->decision.reason == reason);
	assert(f->decision.generation == f->context.generation);
}

static void real_mode_selectors(void)
{
	assert(srp_validate_catalog(&catalog) == SRP_OK);
	assert(srp_mode_by_level(&catalog, SRP_HIGH)->hz == 120);
	assert(srp_mode_by_level(&catalog, SRP_MIDDLE)->hz == 90);
	assert(srp_mode_by_level(&catalog, SRP_LOW)->hz == 60);
	assert(srp_mode_by_level(&catalog, SRP_PEAK)->hz == 185);
	assert(srp_mode_by_level(&catalog, (enum srp_level)-1) == NULL);
}

static void ordinary_global_active_inactive(void)
{
	struct fixture f = fixture();

	choose(&f, NULL, SRP_SOURCE_ACTIVE, false, 0, 120, SRP_REASON_APP_MAX);
	choose(&f, NULL, SRP_SOURCE_INACTIVE, false, 1, 60, SRP_REASON_APP_MIN);
	f.config.apps_default = SRP_PEAK;
	f.config.apps_inactive = SRP_MIDDLE;
	choose(&f, NULL, SRP_SOURCE_ACTIVE, false, 8, 185, SRP_REASON_APP_MAX);
	choose(&f, NULL, SRP_SOURCE_INACTIVE, false, 2, 90, SRP_REASON_APP_MIN);
}

static void exact_package_lookup(void)
{
	const struct srp_app_rule rules[] = {
		{"test.game", 120, 165}, {"test.game.beta", 60, 90},
	};

	assert(srp_find_app_rule(rules, 2, "test.game") == &rules[0]);
	assert(srp_find_app_rule(rules, 2, "test.game.beta") == &rules[1]);
	assert(!srp_find_app_rule(rules, 2, "test.game:remote"));
	assert(!srp_find_app_rule(rules, 2, "test"));
	assert(!srp_find_app_rule(NULL, 2, "test.game"));
}

static void stored_range_survives_game_merge(void)
{
	struct fixture f = fixture();
	struct srp_range range;
	struct srp_app_rule before = game_rule;

	assert(srp_effective_range(&f.config, &catalog, &game_rule,
		&f.context, &range) == SRP_OK);
	assert(range.min_mode_id == 0 && range.max_mode_id == 4);
	assert(range.dynamic && range.explicit_rule);
	f.context.game = true;
	assert(srp_effective_range(&f.config, &catalog, &game_rule,
		&f.context, &range) == SRP_OK);
	assert(range.min_mode_id == 4 && range.max_mode_id == 4 && !range.dynamic);
	choose(&f, &game_rule, SRP_SOURCE_INACTIVE, false, 4, 165, SRP_REASON_APP_MIN);
	assert(!memcmp(&before, &game_rule, sizeof(before)));
}

static void zero_min_uses_global_inactive(void)
{
	struct fixture f = fixture();
	const struct srp_app_rule rule = {"test.new", 0, 165};

	choose(&f, &rule, SRP_SOURCE_INACTIVE, false, 1, 60, SRP_REASON_APP_MIN);
	f.config.apps_inactive = SRP_MIDDLE;
	choose(&f, &rule, SRP_SOURCE_INACTIVE, false, 2, 90, SRP_REASON_APP_MIN);
	assert(rule.min_hz == 0 && rule.max_hz == 165);
}

static void game_launcher_standby_defaults(void)
{
	struct fixture f = fixture();
	struct srp_range range;

	f.config.games_default = SRP_PEAK;
	f.context.game = true;
	choose(&f, NULL, SRP_SOURCE_INACTIVE, false, 8, 185, SRP_REASON_APP_MIN);
	f.context.game = false;
	f.context.launcher = true;
	f.config.gesture = SRP_MIDDLE;
	choose(&f, NULL, SRP_SOURCE_ACTIVE, false, 2, 90, SRP_REASON_GESTURE);
	choose(&f, NULL, SRP_SOURCE_INACTIVE, false, 1, 60, SRP_REASON_APP_MIN);
	f.context.launcher = false;
	f.context.standby = true;
	f.config.screen_off = SRP_HIGH;
	assert(srp_effective_range(&f.config, &catalog, NULL,
		&f.context, &range) == SRP_OK);
	assert(range.min_mode_id == 0 && range.max_mode_id == 0 && !range.dynamic);
	choose(&f, NULL, SRP_SOURCE_INACTIVE, false, 0, 120, SRP_REASON_SCREEN_OFF);
}

static void brightness_strict_boundary_and_unknown(void)
{
	struct fixture f = fixture();

	f.config.brightness_threshold = 20;
	f.environment.brightness_percent = 20;
	choose(&f, &game_rule, SRP_SOURCE_TOUCH, false, 4, 165, SRP_REASON_APP_MAX);
	f.environment.brightness_percent = 19;
	choose(&f, &game_rule, SRP_SOURCE_TOUCH, false, 0, 120, SRP_REASON_LOW_BRIGHTNESS);
	f.environment.brightness_percent = -1;
	choose(&f, &game_rule, SRP_SOURCE_TOUCH, false, 4, 165, SRP_REASON_APP_MAX);
	f.environment.brightness_percent = 101;
	choose(&f, &game_rule, SRP_SOURCE_TOUCH, false, 4, 165, SRP_REASON_APP_MAX);
	f.environment.brightness_percent = 0;
	f.config.brightness_threshold = 0;
	choose(&f, &game_rule, SRP_SOURCE_TOUCH, false, 4, 165, SRP_REASON_APP_MAX);
	f.config.brightness_threshold = 101;
	choose(&f, &game_rule, SRP_SOURCE_TOUCH, false, 4, 165, SRP_REASON_APP_MAX);
}

static void low_battery_boundary_charging_and_unknown(void)
{
	struct fixture f = fixture();

	f.environment.battery_percent = 5;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 1, 60, SRP_REASON_LOW_BATTERY);
	f.environment.battery_percent = 6;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
	f.environment.battery_percent = 0;
	f.environment.charging = true;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
	f.environment.charging = false;
	f.environment.battery_percent = -1;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
	f.environment.battery_percent = 101;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
	f.environment.battery_percent = 1;
	f.config.low_battery_threshold = 0;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
}

static void battery_caps_hz_not_mode_id_and_never_raises(void)
{
	const struct srp_mode alternate[] = {
		{50, 30, 1272, 2800}, {30, 60, 1272, 2800},
		{20, 90, 1272, 2800}, {10, 120, 1272, 2800},
	};
	const struct srp_catalog cat = {alternate, 4, 10, 20, 30, 10};
	const struct srp_app_rule rule = {"test.low", 30, 30};
	struct fixture f = fixture();

	f.environment.battery_percent = 1;
	assert(srp_validate_catalog(&cat) == SRP_OK);
	assert(srp_decide(&f.config, &cat, NULL, &f.context, &f.environment,
		SRP_SOURCE_ACTIVE, false, &f.state, &f.decision) == SRP_OK);
	assert(f.decision.mode_id == 30 && f.decision.hz == 60);
	assert(srp_decide(&f.config, &cat, &rule, &f.context, &f.environment,
		SRP_SOURCE_ACTIVE, false, &f.state, &f.decision) == SRP_OK);
	assert(f.decision.hz == 30 && f.decision.reason == SRP_REASON_APP_MAX);
}

static void touch_gesture_priority_and_final_cap(void)
{
	struct fixture f = fixture();

	f.config.gesture = SRP_PEAK;
	f.config.brightness_threshold = 20;
	f.environment.brightness_percent = 10;
	f.context.screen_off = true;
	choose(&f, &game_rule, SRP_SOURCE_TOUCH, true, 8, 185, SRP_REASON_GESTURE);
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, true, 0, 120, SRP_REASON_LOW_BRIGHTNESS);
	f.environment.battery_percent = 5;
	f.config.strict = true;
	choose(&f, &game_rule, SRP_SOURCE_TOUCH, true, 1, 60, SRP_REASON_LOW_BATTERY);
	assert(f.decision.force); /* force uses the final capped reason. */
}

static void off_source_differs_from_inactive_priority(void)
{
	struct fixture f = fixture();

	f.config.low_brightness = SRP_MIDDLE;
	f.config.brightness_threshold = 20;
	f.environment.brightness_percent = 10;
	f.context.screen_off = true;
	choose(&f, NULL, SRP_SOURCE_INACTIVE, false, 2, 90, SRP_REASON_LOW_BRIGHTNESS);
	choose(&f, NULL, SRP_SOURCE_SCREEN_OFF, false, 0, 120, SRP_REASON_SCREEN_OFF);
	f.environment.battery_percent = 5;
	choose(&f, NULL, SRP_SOURCE_SCREEN_OFF, false, 1, 60, SRP_REASON_LOW_BATTERY);
}

static void strict_force_excludes_gesture_and_game(void)
{
	struct fixture f = fixture();

	f.config.strict = true;
	choose(&f, NULL, SRP_SOURCE_ACTIVE, false, 0, 120, SRP_REASON_APP_MAX);
	assert(f.decision.force);
	choose(&f, NULL, SRP_SOURCE_TOUCH, true, 0, 120, SRP_REASON_GESTURE);
	assert(!f.decision.force);
	f.context.game = true;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
	assert(!f.decision.force);
}

static void backend_requirements_are_explicit(void)
{
	struct fixture f = fixture();

	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
	assert(f.decision.execution_flags == SRP_EXEC_DIRECT_1035);
	f.config.normal_peak_hz = 120;
	f.config.disable_ltpo = true;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
	assert(f.decision.execution_flags ==
		(SRP_EXEC_DIRECT_1035 | SRP_EXEC_LTPO_REASSERT | SRP_EXEC_RAMP));
	assert(f.decision.oc_step_interval_ms == 600);
	f.config.compatible = true;
	choose(&f, &game_rule, SRP_SOURCE_ACTIVE, false, 4, 165, SRP_REASON_APP_MAX);
	assert(f.decision.execution_flags == (SRP_EXEC_COMPATIBLE | SRP_EXEC_LTPO_REASSERT));
	f.config.compatible = false;
	f.config.disable_ltpo = false;
	f.environment.current_hz = 165;
	choose(&f, NULL, SRP_SOURCE_ACTIVE, false, 0, 120, SRP_REASON_APP_MAX);
	assert(f.decision.execution_flags & SRP_EXEC_RAMP); /* lowering from OC */
}

static void video_unsupported_does_not_block_normal_paths(void)
{
	struct fixture f = fixture();

	f.config.video_optimize = true;
	choose(&f, NULL, SRP_SOURCE_INACTIVE, false, 1, 60, SRP_REASON_APP_MIN);
	f.context.video = true;
	choose(&f, NULL, SRP_SOURCE_ACTIVE, false, 0, 120, SRP_REASON_APP_MAX);
	assert(srp_decide(&f.config, &catalog, NULL, &f.context, &f.environment,
		SRP_SOURCE_INACTIVE, false, &f.state, &f.decision) == SRP_UNSUPPORTED_VIDEO);
	assert(!f.decision.apply && f.decision.reason == SRP_REASON_NONE);
	f.config.video_optimize = false;
	choose(&f, NULL, SRP_SOURCE_INACTIVE, false, 1, 60, SRP_REASON_APP_MIN);
}

static void disabled_missing_context_and_stale_are_not_applied(void)
{
	struct fixture f = fixture();

	f.config.enabled = false;
	assert(srp_decide(&f.config, &catalog, NULL, &f.context, &f.environment,
		SRP_SOURCE_ACTIVE, false, &f.state, &f.decision) == SRP_DISABLED);
	assert(!f.decision.apply);
	f.config.enabled = true;
	f.context.valid = false;
	assert(srp_decide(&f.config, &catalog, NULL, &f.context, &f.environment,
		SRP_SOURCE_ACTIVE, false, &f.state, &f.decision) == SRP_NO_CONTEXT);
	f.context.valid = true;
	f.context.generation++;
	assert(srp_decide(&f.config, &catalog, NULL, &f.context, &f.environment,
		SRP_SOURCE_ACTIVE, false, &f.state, &f.decision) == SRP_STALE);
	assert(!f.decision.apply);
}

static void validate_group_duplicates_and_missing_selectors(void)
{
	struct srp_mode bad[9];
	struct srp_catalog cat = catalog;
	struct fixture f = fixture();

	memcpy(bad, modes, sizeof(bad));
	cat.modes = bad;
	bad[8].width = 1080;
	assert(srp_validate_catalog(&cat) == SRP_INVALID);
	bad[8] = modes[8];
	bad[8].mode_id = 0;
	assert(srp_validate_catalog(&cat) == SRP_INVALID);
	bad[8] = modes[8];
	bad[8].hz = 120;
	assert(srp_validate_catalog(&cat) == SRP_INVALID);
	cat = catalog;
	cat.high_mode_id = 99;
	assert(srp_validate_catalog(&cat) == SRP_INVALID);
	f.config.gesture = (enum srp_level)-1;
	assert(srp_validate_config(&f.config) == SRP_INVALID);
}

static void aliases_respect_caller_high_override(void)
{
	struct fixture f = fixture();
	struct srp_catalog cat = catalog;
	const struct srp_app_rule rule = {"test.alias", 60, 120};
	struct srp_range range;

	cat.high_mode_id = 3; /* display_modes overrides high to mode144. */
	assert(srp_effective_range(&f.config, &cat, &rule,
		&f.context, &range) == SRP_OK);
	assert(range.max_mode_id == 3 && range.min_mode_id == 1);
}

static void invalid_rule_is_visible_and_max_fallback_is_global(void)
{
	struct fixture f = fixture();
	struct srp_range range;
	struct srp_app_rule rule = {"test.bad", 77, 165};

	assert(srp_effective_range(&f.config, &catalog, &rule,
		&f.context, &range) == SRP_INVALID);
	rule.min_hz = 60;
	rule.max_hz = 77;
	f.config.apps_default = SRP_MIDDLE;
	assert(srp_effective_range(&f.config, &catalog, &rule,
		&f.context, &range) == SRP_OK);
	assert(range.max_mode_id == 2 && range.min_mode_id == 1);
	rule.min_hz = 165;
	rule.max_hz = 120;
	assert(srp_effective_range(&f.config, &catalog, &rule,
		&f.context, &range) == SRP_OK);
	assert(range.min_mode_id == 4 && range.max_mode_id == 0);
	assert(rule.min_hz == 165 && rule.max_hz == 120); /* no silent rewrite */
}

static void native_deadline_formula_and_default_delays(void)
{
	struct fixture f = fixture();

	assert(srp_inactive_at(&f.config, false, 1000, 1000, 0, false) == 1900);
	assert(srp_inactive_at(&f.config, false, 1500, 1000, 1400, true) == 3400);
	assert(srp_inactive_at(&f.config, true, 1500, 1000, 0, false) == 2400);
	assert(srp_inactive_at(&f.config, true, 1500, 1000, 0, true) == 3500);
	assert(srp_inactive_at(&f.config, false, 10000, 0, 0, false) == 10000);
	f.config.tap_delay_ms = -1;
	f.config.scroll_delay_ms = 0;
	assert(srp_inactive_at(&f.config, false, 1000, 1000, 0, false) == 1900);
}

static void absolute_deadlines_merge_without_adding_delays(void)
{
	struct fixture f = fixture();
	uint64_t old_token;

	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1000, 1000, 0, false, &f.decision) == SRP_OK);
	assert(f.decision.deadline_valid && f.decision.deadline_ms == 1900);
	old_token = f.decision.deadline_token;
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1100, 1100, 0, false, &f.decision) == SRP_OK);
	assert(f.decision.deadline_ms == 2000); /* not 1900+900 */
	assert(srp_take_inactive(&f.state, 1, old_token, 5000) == SRP_STALE);
	old_token = f.decision.deadline_token;
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1150, 0, 0, false, &f.decision) == SRP_OK);
	assert(f.decision.deadline_ms == 2000); /* never bring a pending hold forward */
	assert(f.decision.deadline_token != old_token);
}

static void deadline_is_one_shot_and_generation_protected(void)
{
	struct fixture f = fixture();
	uint64_t token;

	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1000, 1000, 0, false, &f.decision) == SRP_OK);
	token = f.decision.deadline_token;
	assert(srp_take_inactive(&f.state, 1, token, 1899) == SRP_NOT_DUE);
	assert(f.state.deadline_valid);
	assert(srp_take_inactive(&f.state, 1, token, 1900) == SRP_OK);
	assert(!f.state.deadline_valid);
	assert(srp_take_inactive(&f.state, 1, token, 1900) == SRP_NO_DEADLINE);
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		2000, 2000, 0, false, &f.decision) == SRP_OK);
	token = f.decision.deadline_token;
	srp_set_context(&f.state, 2);
	assert(!f.state.deadline_valid);
	assert(srp_take_inactive(&f.state, 1, token, 9999) == SRP_STALE);
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		3000, 3000, 0, false, &f.decision) == SRP_STALE);
}

static void activity_cancel_and_screen_off_disarm_deadline(void)
{
	struct fixture f = fixture();

	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1000, 1000, 0, false, &f.decision) == SRP_OK);
	srp_cancel_deadline(&f.state); /* new touch event */
	assert(!f.state.deadline_valid);
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1100, 1100, 0, false, &f.decision) == SRP_OK);
	f.context.screen_off = true;
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1200, 1100, 0, false, &f.decision) == SRP_NO_DEADLINE);
	assert(!f.decision.deadline_valid && !f.state.deadline_valid);
}

static void disable_missing_context_and_zero_generation_cancel_safely(void)
{
	struct fixture f = fixture();
	uint64_t token;

	f.context.generation = 0;
	srp_set_context(&f.state, 0);
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1000, 1000, 0, false, &f.decision) == SRP_OK);
	token = f.decision.deadline_token;
	srp_set_context(&f.state, 0); /* unchanged identity preserves the hold. */
	assert(f.state.deadline_valid && f.state.deadline_token == token);
	f.config.enabled = false;
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1100, 1000, 0, false, &f.decision) == SRP_DISABLED);
	assert(!f.state.deadline_valid && !f.decision.deadline_valid);
	f.config.enabled = true;
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1200, 1100, 0, false, &f.decision) == SRP_OK);
	f.context.valid = false;
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1300, 1100, 0, false, &f.decision) == SRP_NO_CONTEXT);
	assert(!f.state.deadline_valid);
}

static void global_off_and_battery_selectors_are_consumed(void)
{
	struct fixture f = fixture();

	f.config.screen_off = SRP_PEAK;
	choose(&f, NULL, SRP_SOURCE_SCREEN_OFF, false, 8, 185, SRP_REASON_SCREEN_OFF);
	f.config.low_battery_mode = SRP_MIDDLE;
	f.environment.battery_percent = 5;
	choose(&f, NULL, SRP_SOURCE_SCREEN_OFF, false, 2, 90, SRP_REASON_LOW_BATTERY);
	f.context.launcher = true;
	f.config.gesture = SRP_PEAK;
	f.config.brightness_threshold = 20;
	f.config.low_brightness = SRP_LOW;
	f.environment.brightness_percent = 10;
	choose(&f, NULL, SRP_SOURCE_ACTIVE, false, 1, 60, SRP_REASON_LOW_BRIGHTNESS);
	assert(srp_decide(&f.config, &catalog, NULL, &f.context, &f.environment,
		(enum srp_source)-1, false, &f.state, &f.decision) == SRP_INVALID);
	assert(!f.decision.apply);
}

static void time_overflow_and_future_timestamp_rejected(void)
{
	struct fixture f = fixture();

	assert(srp_inactive_at(&f.config, true, UINT64_MAX - 100,
		UINT64_MAX - 100, UINT64_MAX - 100, true) == UINT64_MAX);
	assert(srp_schedule_inactive(&f.state, &f.config, &f.context,
		1000, 1001, 0, false, &f.decision) == SRP_INVALID);
	assert(!f.state.deadline_valid);
	f.state.deadline_token = UINT64_MAX;
	srp_cancel_deadline(&f.state);
	assert(f.state.deadline_token == 1 && !f.state.deadline_valid);
}

#define RUN(test) do { test(); count++; } while (0)
int main(void)
{
	unsigned int count = 0;

	RUN(real_mode_selectors);
	RUN(ordinary_global_active_inactive);
	RUN(exact_package_lookup);
	RUN(stored_range_survives_game_merge);
	RUN(zero_min_uses_global_inactive);
	RUN(game_launcher_standby_defaults);
	RUN(brightness_strict_boundary_and_unknown);
	RUN(low_battery_boundary_charging_and_unknown);
	RUN(battery_caps_hz_not_mode_id_and_never_raises);
	RUN(touch_gesture_priority_and_final_cap);
	RUN(off_source_differs_from_inactive_priority);
	RUN(strict_force_excludes_gesture_and_game);
	RUN(backend_requirements_are_explicit);
	RUN(video_unsupported_does_not_block_normal_paths);
	RUN(disabled_missing_context_and_stale_are_not_applied);
	RUN(validate_group_duplicates_and_missing_selectors);
	RUN(aliases_respect_caller_high_override);
	RUN(invalid_rule_is_visible_and_max_fallback_is_global);
	RUN(native_deadline_formula_and_default_delays);
	RUN(absolute_deadlines_merge_without_adding_delays);
	RUN(deadline_is_one_shot_and_generation_protected);
	RUN(activity_cancel_and_screen_off_disarm_deadline);
	RUN(disable_missing_context_and_zero_generation_cancel_safely);
	RUN(global_off_and_battery_selectors_are_consumed);
	RUN(time_overflow_and_future_timestamp_rejected);
	printf("scene_refresh_policy: %u tests passed\n", count);
	return 0;
}
