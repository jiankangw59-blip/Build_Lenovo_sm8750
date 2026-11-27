/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include "scene_activity_policy.h"

struct fixture {
	struct sa_state state;
	struct sa_config config;
	struct sa_context context;
	struct sa_output out;
};

static void apply(struct fixture *f, struct sa_event event)
{
	assert(sa_apply(&f->state, &f->config, &f->context, &event, &f->out) == SA_OK);
}

static struct fixture start(void)
{
	struct fixture f = {0};

	f.config = sa_native_defaults();
	f.context = (struct sa_context){.epoch = 1, .enabled = true, .valid = true};
	apply(&f, (struct sa_event){.kind = SA_CONTEXT, .now_ms = 1000, .input_reliable = true});
	assert(f.out.owner == SA_OWNER_SCENE && !f.out.inactive && f.out.publish);
	return f;
}

static void initial_idle_and_immediate_touch_reactivation(void)
{
	struct fixture f = start();

	assert(f.out.deadline_valid && f.out.deadline_ms == 2000);
	apply(&f, (struct sa_event){.kind = SA_DEADLINE, .now_ms = 2000,
		.token = f.out.token, .load_known = true, .load_permille = 500});
	assert(f.out.inactive && f.out.publish && !f.out.deadline_valid);
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 2001, .contacts = 1});
	assert(!f.out.inactive && f.out.publish && !f.out.deadline_valid);
}

static void contacts_hold_and_release_uses_2s_not_refresh_900ms(void)
{
	struct fixture f = start();

	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1100, .contacts = 2});
	assert(!f.out.deadline_valid && !f.out.publish);
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1200, .contacts = 1});
	assert(!f.out.deadline_valid);
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1300, .contacts = 0});
	assert(f.out.deadline_valid && f.out.deadline_ms == 3300);
}

static void launcher_release_uses_3s(void)
{
	struct fixture f = start();

	f.context.epoch++;
	f.context.launcher = true;
	apply(&f, (struct sa_event){.kind = SA_CONTEXT, .now_ms = 1100, .input_reliable = true});
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1200});
	assert(f.out.deadline_ms == 4200);
}

static void event_deadlines_replace_absolute_time_and_reject_old_token(void)
{
	struct fixture f = start();
	sa_u64 old_token;
	struct sa_event event;

	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1100});
	old_token = f.out.token;
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1200});
	assert(f.out.deadline_ms == 3200 && f.out.token != old_token);
	event = (struct sa_event){.kind = SA_DEADLINE, .now_ms = 5000, .token = old_token,
		.load_known = true, .load_permille = 0};
	assert(sa_apply(&f.state, &f.config, &f.context, &event, &f.out) == SA_STALE);
	assert(!f.out.inactive && f.out.deadline_ms == 3200);
	event.token = f.out.token;
	event.now_ms = 3199;
	assert(sa_apply(&f.state, &f.config, &f.context, &event, &f.out) == SA_NOT_DUE);
	assert(f.out.deadline_valid);
}

static void load_hold_uses_verified_strict_thresholds(void)
{
	assert(sa_native_load_hold_ms(650) == 0);
	assert(sa_native_load_hold_ms(651) == 3000);
	assert(sa_native_load_hold_ms(750) == 3000);
	assert(sa_native_load_hold_ms(751) == 4000);
	assert(sa_native_load_hold_ms(850) == 4000);
	assert(sa_native_load_hold_ms(851) == 5000);
	assert(sa_native_load_hold_ms(950) == 5000);
	assert(sa_native_load_hold_ms(951) == 6000);
}

static void existing_load_event_can_finish_qualification_without_polling(void)
{
	struct fixture f = start();

	apply(&f, (struct sa_event){.kind = SA_DEADLINE, .now_ms = 2000, .token = f.out.token,
		.load_known = true, .load_permille = 950});
	assert(!f.out.inactive && !f.out.publish && f.out.deadline_ms == 7000);
	apply(&f, (struct sa_event){.kind = SA_LOAD, .now_ms = 2100,
		.token = f.out.token, .load_known = true, .load_permille = 500});
	assert(f.out.inactive && f.out.publish && !f.out.deadline_valid);
}

static void unknown_load_stays_explicit_then_has_one_bounded_hold(void)
{
	struct fixture f = start();
	struct sa_event event;

	apply(&f, (struct sa_event){.kind = SA_DEADLINE, .now_ms = 2000, .token = f.out.token});
	assert(!f.out.inactive && f.out.needs_load && !f.out.deadline_valid);
	apply(&f, (struct sa_event){.kind = SA_LOAD, .now_ms = 2100,
		.token = f.out.token, .load_known = true, .load_permille = 990});
	assert(f.out.deadline_ms == 8000 && !f.out.needs_load);
	apply(&f, (struct sa_event){.kind = SA_DEADLINE, .now_ms = 8000, .token = f.out.token});
	assert(f.out.inactive && !f.out.deadline_valid);
	event = (struct sa_event){.kind = SA_DEADLINE, .now_ms = 9000, .token = f.out.token};
	assert(sa_apply(&f.state, &f.config, &f.context, &event, &f.out) == SA_NO_DEADLINE);
	assert(!f.out.deadline_valid); /* Never creates a repeating timer. */
}

static void new_touch_cancels_load_qualification(void)
{
	struct fixture f = start();
	sa_u64 old_token;
	struct sa_event event;

	apply(&f, (struct sa_event){.kind = SA_DEADLINE, .now_ms = 2000, .token = f.out.token,
		.load_known = true, .load_permille = 990});
	old_token = f.out.token;
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 2100, .contacts = 1});
	assert(!f.out.inactive && !f.out.deadline_valid && !f.out.needs_load);
	event = (struct sa_event){.kind = SA_DEADLINE, .now_ms = 8000, .token = old_token};
	assert(sa_apply(&f.state, &f.config, &f.context, &event, &f.out) == SA_NO_DEADLINE);
	/* A late reply from round one must not authorize a later idle round. */
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 2200});
	apply(&f, (struct sa_event){.kind = SA_DEADLINE, .now_ms = 4200, .token = f.out.token});
	assert(f.out.needs_load);
	event = (struct sa_event){.kind = SA_LOAD, .now_ms = 4300, .token = old_token,
		.load_known = true, .load_permille = 0};
	assert(sa_apply(&f.state, &f.config, &f.context, &event, &f.out) == SA_STALE);
	assert(!f.out.inactive && f.out.needs_load);
}

static void visible_or_mode_epoch_changes_cancel_old_context_work(void)
{
	struct fixture f = start();
	struct sa_context previous = f.context;
	struct sa_event event;
	sa_u64 token = f.out.token;

	f.context.epoch++; /* Same foreground, different visible/windowed set or mode. */
	apply(&f, (struct sa_event){.kind = SA_CONTEXT, .now_ms = 1100, .input_reliable = true});
	assert(f.out.publish && f.out.epoch == 2 && f.out.deadline_ms == 2100);
	event = (struct sa_event){.kind = SA_DEADLINE, .now_ms = 2000, .token = token,
		.load_known = true, .load_permille = 0};
	assert(sa_apply(&f.state, &f.config, &previous, &event, &f.out) == SA_STALE);
	assert(!f.out.inactive && f.out.epoch == 2);
}

static void game_frame_owner_and_screen_off_never_run_scene_idle(void)
{
	struct fixture f = start();

	f.context.epoch++;
	f.context.game = true;
	apply(&f, (struct sa_event){.kind = SA_CONTEXT, .now_ms = 1100, .input_reliable = true});
	assert(f.out.owner == SA_OWNER_FRAME && !f.out.inactive && !f.out.deadline_valid);
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1200});
	assert(f.out.owner == SA_OWNER_FRAME && !f.out.publish && !f.out.deadline_valid);
	f.context.epoch++;
	f.context.screen_off = true;
	apply(&f, (struct sa_event){.kind = SA_CONTEXT, .now_ms = 1300, .input_reliable = true});
	assert(f.out.owner == SA_OWNER_SCENE && f.out.off && !f.out.inactive && !f.out.deadline_valid);
	f.context.epoch++;
	f.context.screen_off = f.context.game = false;
	apply(&f, (struct sa_event){.kind = SA_CONTEXT, .now_ms = 1400, .input_reliable = true});
	assert(f.out.owner == SA_OWNER_SCENE && !f.out.off && f.out.deadline_ms == 2400);
}

static void dropped_input_requires_resync_and_does_not_guess_finger_release(void)
{
	struct fixture f = start();

	apply(&f, (struct sa_event){.kind = SA_SOURCE_LOST, .now_ms = 1100});
	assert(f.out.needs_input_resync && !f.out.deadline_valid && !f.out.inactive);
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1200, .contacts = 0});
	assert(f.out.needs_input_resync && !f.out.deadline_valid);
	apply(&f, (struct sa_event){.kind = SA_RESYNC, .now_ms = 1300,
		.input_reliable = true, .contacts = 2});
	assert(!f.out.needs_input_resync && !f.out.deadline_valid);
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = 1400});
	assert(f.out.deadline_ms == 3400);
}

static void disabled_or_untrusted_foreground_releases_scene_owner(void)
{
	struct fixture f = start();

	f.context.epoch++;
	f.context.enabled = false;
	apply(&f, (struct sa_event){.kind = SA_CONTEXT, .now_ms = 1100});
	assert(f.out.owner == SA_OWNER_NONE && f.out.publish && !f.out.deadline_valid);
	f.context.epoch++;
	f.context.enabled = true;
	f.context.valid = false;
	apply(&f, (struct sa_event){.kind = SA_CONTEXT, .now_ms = 1200});
	assert(f.out.owner == SA_OWNER_NONE && !f.out.deadline_valid);
}

static void stale_context_clock_and_unsigned_time_overflow_are_explicit(void)
{
	struct fixture f = start();
	struct sa_event event = {.kind = SA_CONTEXT, .now_ms = 1100, .input_reliable = true};

	f.context.game = true; /* Caller forgot to advance full-context epoch. */
	assert(sa_apply(&f.state, &f.config, &f.context, &event, &f.out) == SA_INVALID);
	f.context = f.state.context;
	event = (struct sa_event){.kind = SA_INPUT, .now_ms = 999};
	assert(sa_apply(&f.state, &f.config, &f.context, &event, &f.out) == SA_TIME_REVERSED);
	assert(!f.out.publish && f.out.deadline_ms == 2000);
	f.state.token = ~(sa_u64)0;
	apply(&f, (struct sa_event){.kind = SA_INPUT, .now_ms = ~(sa_u64)0 - 100});
	assert(f.out.deadline_valid && f.out.deadline_ms == ~(sa_u64)0 && f.out.token != 0);
}

#define RUN(test) do { test(); count++; } while (0)
int main(void)
{
	unsigned int count = 0;

	RUN(initial_idle_and_immediate_touch_reactivation);
	RUN(contacts_hold_and_release_uses_2s_not_refresh_900ms);
	RUN(launcher_release_uses_3s);
	RUN(event_deadlines_replace_absolute_time_and_reject_old_token);
	RUN(load_hold_uses_verified_strict_thresholds);
	RUN(existing_load_event_can_finish_qualification_without_polling);
	RUN(unknown_load_stays_explicit_then_has_one_bounded_hold);
	RUN(new_touch_cancels_load_qualification);
	RUN(visible_or_mode_epoch_changes_cancel_old_context_work);
	RUN(game_frame_owner_and_screen_off_never_run_scene_idle);
	RUN(dropped_input_requires_resync_and_does_not_guess_finger_release);
	RUN(disabled_or_untrusted_foreground_releases_scene_owner);
	RUN(stale_context_clock_and_unsigned_time_overflow_are_explicit);
	printf("scene_activity_policy: %u tests passed\n", count);
	return 0;
}
