/* SPDX-License-Identifier: GPL-2.0 */
/*
 * Host fault tests (glibc): gcc -std=c11 -O1 -g -Wall -Wextra -Werror
 * -pedantic -fsanitize=address,undefined -fno-omit-frame-pointer
 * -DSRCFG_TEST_WRAP scene_refresh_config.c test_scene_refresh_config.c
 * -Wl,--wrap=malloc,--wrap=calloc,--wrap=realloc,--wrap=read,--wrap=__read_chk
 * -o /tmp/test_scene_refresh_config
 */
#define _POSIX_C_SOURCE 200809L
#include "scene_refresh_config.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static const struct srp_mode modes[] = {
	{0, 120, 1272, 2800}, {1, 60, 1272, 2800}, {2, 90, 1272, 2800},
	{3, 144, 1272, 2800}, {4, 165, 1272, 2800}, {5, 170, 1272, 2800},
	{6, 175, 1272, 2800}, {7, 180, 1272, 2800}, {8, 185, 1272, 2800},
};
static const struct srp_catalog catalog = {modes, 9, 0, 2, 1, 8};
static const char apps[] = "fixture.game 120 165\nfixture.normal 60 90\nfixture.new 0 120\n";
static const char global[] = "enable=1\napps_default=high\napps_inactive=low\n";

#ifdef SRCFG_TEST_WRAP
static size_t fail_allocation, allocations;
static const char *rename_from, *rename_to;
void *__real_malloc(size_t size);
void *__real_calloc(size_t count, size_t size);
void *__real_realloc(void *pointer, size_t size);
ssize_t __real_read(int fd, void *buffer, size_t size);
ssize_t __real___read_chk(int fd, void *buffer, size_t size, size_t capacity);

static bool allocation_fails(void)
{
	if (fail_allocation && ++allocations == fail_allocation) {
		errno = ENOMEM;
		return true;
	}
	return false;
}
void *__wrap_malloc(size_t size) { return allocation_fails() ? NULL : __real_malloc(size); }
void *__wrap_calloc(size_t n, size_t size) { return allocation_fails() ? NULL : __real_calloc(n, size); }
void *__wrap_realloc(void *p, size_t size) { return allocation_fails() ? NULL : __real_realloc(p, size); }
ssize_t __wrap_read(int fd, void *buffer, size_t size)
{
	ssize_t result = __real_read(fd, buffer, size);

	if (result > 0 && rename_from) {
		const char *from = rename_from;
		rename_from = NULL;
		assert(rename(from, rename_to) == 0);
	}
	return result;
}
ssize_t __wrap___read_chk(int fd, void *buffer, size_t size, size_t capacity)
{
	/* Ubuntu may fortify read(); preserve its bounds check before injecting. */
	if (size > capacity) return __real___read_chk(fd, buffer, size, capacity);
	return __wrap_read(fd, buffer, size);
}
#endif

static struct srcfg_snapshot *parse(const char *app_text, const char *global_text,
		const struct srcfg_options *options)
{
	struct srcfg_snapshot *snapshot = NULL;
	struct srcfg_error error;

	assert(srcfg_parse_buffers(app_text, strlen(app_text), global_text, strlen(global_text),
		&catalog, options, &snapshot, &error) == SRCFG_OK);
	assert(snapshot && error.code == SRCFG_OK);
	return snapshot;
}

static const struct srcfg_entry *entry(const struct srcfg_snapshot *snapshot, const char *key)
{
	size_t i;

	for (i = 0; i < snapshot->entry_count; i++)
		if (!strcmp(snapshot->entries[i].key, key)) return &snapshot->entries[i];
	return NULL;
}

static void actual_defaults_and_stored_game_range(void)
{
	struct srcfg_snapshot *snapshot = parse(apps, global, NULL);
	const struct srp_app_rule *rule = srp_find_app_rule(snapshot->rules, snapshot->rule_count, "fixture.game");
	struct srp_context context = {.valid = true, .game = true};
	struct srp_range range;

	assert(snapshot->rule_count == 3 && snapshot->policy.enabled);
	assert(srp_mode_by_level(&snapshot->catalog, SRP_HIGH)->hz == 120);
	assert(srp_mode_by_level(&snapshot->catalog, SRP_MIDDLE)->hz == 90);
	assert(srp_mode_by_level(&snapshot->catalog, SRP_LOW)->hz == 60);
	assert(srp_mode_by_level(&snapshot->catalog, SRP_PEAK)->hz == 185);
	assert(snapshot->policy.normal_peak_hz == 185);
	assert(rule && rule->min_hz == 120 && rule->max_hz == 165);
	assert(srp_effective_range(&snapshot->policy, &snapshot->catalog, rule, &context, &range) == SRP_OK);
	assert(range.min_mode_id == 4 && range.max_mode_id == 4);
	assert(rule->min_hz == 120 && rule->max_hz == 165);
	srcfg_snapshot_free(snapshot);
	snapshot = parse("fixture.inverted 165 120\n", global, NULL);
	assert(snapshot->rules[0].min_hz == 165 && snapshot->rules[0].max_hz == 120);
	assert(!snapshot->normalized_value_count); /* Original UI does not clamp min to max. */
	srcfg_snapshot_free(snapshot);
}

static void parses_all_confirmed_global_settings(void)
{
	const char text[] = "enable=true\napps_default=middle\napps_inactive=high\ngames_default=peak\n"
		"gesture=low\nscreen_off=peak\nlow_battery_mode=middle\nlow_brightness=low\n"
		"low_battery_threshold=9\nbrightness_threshold=20\ntap_delay=1100\nscroll_delay=2600\n"
		"strict=1\ncompatible=true\ndisable_ltpo=1\nnormal_peak_rr=120\noc_step_interval=750\n";
	struct srcfg_snapshot *snapshot = parse(apps, text, NULL);
	const struct srp_config *c = &snapshot->policy;

	assert(c->enabled && c->strict && c->compatible && c->disable_ltpo);
	assert(c->apps_default == SRP_MIDDLE && c->apps_inactive == SRP_HIGH);
	assert(c->games_default == SRP_PEAK && c->gesture == SRP_LOW && c->screen_off == SRP_PEAK);
	assert(c->low_battery_mode == SRP_MIDDLE && c->low_brightness == SRP_LOW);
	assert(c->low_battery_threshold == 9 && c->brightness_threshold == 20);
	assert(c->tap_delay_ms == 1100 && c->scroll_delay_ms == 2600);
	assert(c->normal_peak_hz == 120 && c->oc_step_interval_ms == 750);
	assert(!snapshot->unknown_key_count);
	srcfg_snapshot_free(snapshot);
}

static void mode_overrides_are_ids_and_leave_peak(void)
{
	struct srcfg_snapshot *snapshot = parse(apps, "enable=1\ndisplay_modes=3 -1 1\n", NULL);
	struct srp_context context = {.valid = true};
	struct srp_range range;
	const struct srp_app_rule *rule = srp_find_app_rule(snapshot->rules, snapshot->rule_count, "fixture.new");

	assert(snapshot->catalog.high_mode_id == 3 && snapshot->catalog.middle_mode_id == 2);
	assert(snapshot->catalog.low_mode_id == 1 && snapshot->catalog.peak_mode_id == 8);
	assert(!strcmp(entry(snapshot, "display_modes")->value, "3 -1 1"));
	assert(rule->min_hz == 0 && rule->max_hz == 120);
	assert(srp_effective_range(&snapshot->policy, &snapshot->catalog, rule, &context, &range) == SRP_OK);
	assert(range.min_mode_id == 1 && range.max_mode_id == 3);
	srcfg_snapshot_free(snapshot);
}

static void invalid_mode_override_is_not_hz_or_other_group(void)
{
	struct srcfg_snapshot *snapshot = NULL;
	struct srcfg_error error;
	const char bad[] = "enable=1\ndisplay_modes=120 -1 -1\n";

	assert(srcfg_parse_buffers(apps, strlen(apps), bad, strlen(bad), &catalog, NULL,
		&snapshot, &error) == SRCFG_CATALOG);
	assert(!snapshot && error.source == SRCFG_SOURCE_GLOBAL && error.line == 2);
}

static void caller_detection_is_not_hardcoded_to_phone_ids(void)
{
	const struct srp_mode alternate[] = {
		{91, 90, 1080, 2378}, {15, 30, 1080, 2378},
		{17, 60, 1080, 2378}, {55, 120, 1080, 2378},
	};
	const struct srp_catalog detected = {alternate, 4, 91, 17, 15, 55};
	struct srcfg_snapshot *snapshot = NULL;
	struct srcfg_error error;

	assert(srcfg_parse_buffers("fixture.zero 0 0", 16, global, strlen(global),
		&detected, NULL, &snapshot, &error) == SRCFG_OK);
	assert(srp_mode_by_level(&snapshot->catalog, SRP_HIGH)->hz == 90);
	assert(srp_mode_by_level(&snapshot->catalog, SRP_LOW)->hz == 30);
	assert(snapshot->catalog.peak_mode_id == 55);
	srcfg_snapshot_free(snapshot);
}

static void duplicates_last_win_and_remain_diagnosable(void)
{
	struct srcfg_snapshot *snapshot = parse(
		"fixture.a 60 90\nfixture.b 60 120\nfixture.a 0 165\n",
		"enable=1\ngesture=unknown_old\ngesture=peak\nfuture_key=old\nfuture_key=new\n", NULL);
	const struct srp_app_rule *rule = srp_find_app_rule(snapshot->rules, snapshot->rule_count, "fixture.a");

	assert(snapshot->rule_count == 2 && snapshot->duplicate_app_count == 1);
	assert(rule && !rule->min_hz && rule->max_hz == 165);
	assert(snapshot->duplicate_key_count == 2 && snapshot->unknown_key_count == 1);
	assert(snapshot->policy.gesture == SRP_PEAK);
	assert(entry(snapshot, "future_key")->occurrences == 2);
	assert(!entry(snapshot, "future_key")->known && !strcmp(entry(snapshot, "future_key")->value, "new"));
	srcfg_snapshot_free(snapshot);
}

static void unknown_keys_and_empty_unknown_values_preserved(void)
{
	struct srcfg_snapshot *snapshot = parse(apps, "enable=1\nnext_feature=\nfuture_threshold=003\n", NULL);

	assert(snapshot->unknown_key_count == 2 && snapshot->entry_count == 3);
	assert(!strcmp(entry(snapshot, "next_feature")->value, ""));
	assert(!strcmp(entry(snapshot, "future_threshold")->value, "003"));
	srcfg_snapshot_free(snapshot);
}

static void native_normalizations_keep_original_value_for_diagnostics(void)
{
	struct srcfg_snapshot *snapshot = parse(apps,
		"enable=1\ntap_delay=-1\nscroll_delay=0\nbrightness_threshold=101\n", NULL);

	assert(snapshot->normalized_value_count == 3);
	assert(snapshot->policy.tap_delay_ms == 900 && snapshot->policy.scroll_delay_ms == 2000);
	assert(!snapshot->policy.brightness_threshold);
	assert(!strcmp(entry(snapshot, "tap_delay")->value, "-1"));
	srcfg_snapshot_free(snapshot);
}

static void platform_defaults_and_missing_enable_are_explicit(void)
{
	struct srcfg_options options = {.default_strict = true, .default_compatible = true, .video_optimize = true};
	struct srcfg_snapshot *snapshot = parse(apps, "future_field=1\n", &options);

	assert(!snapshot->policy.enabled && snapshot->policy.strict && snapshot->policy.compatible);
	assert(snapshot->policy.video_optimize);
	srcfg_snapshot_free(snapshot);
	snapshot = parse(apps, "enable=1\nstrict=false\ncompatible=0\n", &options);
	assert(snapshot->policy.enabled && !snapshot->policy.strict && !snapshot->policy.compatible);
	srcfg_snapshot_free(snapshot);
}

static void whitespace_comments_and_nonterminated_buffers(void)
{
	const char app_bytes[] = {'a', '\t', '0', ' ', '1', '2', '0'};
	const char feature_bytes[] = {'e', 'n', 'a', 'b', 'l', 'e', '=', '1'};
	struct srcfg_snapshot *snapshot = parse("#note\r\n fixture.a\t60   90 \r\n",
		" #comment\r\n enable = true \r\n", NULL);
	struct srcfg_error error;

	assert(snapshot->rule_count == 1 && snapshot->policy.enabled);
	srcfg_snapshot_free(snapshot);
	snapshot = NULL;
	assert(srcfg_parse_buffers(app_bytes, sizeof(app_bytes), feature_bytes, sizeof(feature_bytes),
		&catalog, NULL, &snapshot, &error) == SRCFG_OK);
	assert(snapshot->rule_count == 1 && !strcmp(snapshot->rules[0].package, "a"));
	srcfg_snapshot_free(snapshot);
}

static void empty_requires_explicit_intent(void)
{
	struct srcfg_snapshot *snapshot = NULL;
	struct srcfg_options options = {0};
	struct srcfg_error error;

	assert(srcfg_parse_buffers("", 0, global, strlen(global), &catalog, NULL, &snapshot, &error) == SRCFG_EMPTY);
	assert(error.source == SRCFG_SOURCE_APPS && !snapshot);
	assert(srcfg_parse_buffers(apps, strlen(apps), "", 0, &catalog, NULL, &snapshot, &error) == SRCFG_EMPTY);
	assert(error.source == SRCFG_SOURCE_GLOBAL && !snapshot);
	options.flags = SRCFG_ALLOW_EMPTY_APPS | SRCFG_ALLOW_EMPTY_GLOBAL;
	assert(srcfg_parse_buffers(NULL, 0, NULL, 0, &catalog, &options, &snapshot, &error) == SRCFG_OK);
	assert(!snapshot->rule_count && !snapshot->entry_count && !snapshot->policy.enabled);
	srcfg_snapshot_free(snapshot);
}

static void bad_app_rows_report_exact_reason_and_line(void)
{
	const struct { const char *text; enum srcfg_status status; } bad[] = {
		{"fixture.a 60", SRCFG_SYNTAX}, {"fixture.a 60 90 extra", SRCFG_SYNTAX},
		{"fixture.a -1 90", SRCFG_VALUE}, {"fixture.a nope 90", SRCFG_VALUE},
		{"fixture.a 60 2147483648", SRCFG_OVERFLOW}, {"fixture.a 77 90", SRCFG_UNSUPPORTED_RATE},
		{"fixture.a 60 77", SRCFG_UNSUPPORTED_RATE},
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		char buffer[160];
		struct srcfg_snapshot *snapshot = NULL;
		struct srcfg_error error;

		snprintf(buffer, sizeof(buffer), "fixture.good 60 90\n%s\n", bad[i].text);
		assert(srcfg_parse_buffers(buffer, strlen(buffer), global, strlen(global), &catalog,
			NULL, &snapshot, &error) == bad[i].status);
		assert(!snapshot && error.source == SRCFG_SOURCE_APPS && error.line == 2);
	}
}

static void bad_global_rows_do_not_silently_fallback(void)
{
	const struct { const char *text; enum srcfg_status status; } bad[] = {
		{"apps_default=auto", SRCFG_VALUE}, {"enable=yes", SRCFG_VALUE},
		{"enable=", SRCFG_VALUE}, {"tap_delay=12ms", SRCFG_VALUE},
		{"tap_delay=2147483648", SRCFG_OVERFLOW}, {"normal_peak_rr=-1", SRCFG_VALUE},
		{"oc_step_interval=-1", SRCFG_VALUE}, {"display_modes=0 -1", SRCFG_VALUE},
		{"future_key=a=b", SRCFG_SYNTAX}, {"=bad", SRCFG_SYNTAX}, {"unfinished", SRCFG_SYNTAX},
	};
	size_t i;

	for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
		char buffer[160];
		struct srcfg_snapshot *snapshot = NULL;
		struct srcfg_error error;

		snprintf(buffer, sizeof(buffer), "#header\n%s\n", bad[i].text);
		assert(srcfg_parse_buffers(apps, strlen(apps), buffer, strlen(buffer), &catalog,
			NULL, &snapshot, &error) == bad[i].status);
		assert(!snapshot && error.source == SRCFG_SOURCE_GLOBAL && error.line == 2);
	}
}

static void embedded_nul_input_caps_and_invalid_catalog(void)
{
	const char bad[] = "fixture.a 60 90\0fixture.b 60 120";
	struct srcfg_options options = {.max_bytes = 8};
	struct srcfg_snapshot *snapshot = NULL;
	struct srcfg_error error;
	struct srp_catalog invalid = catalog;

	assert(srcfg_parse_buffers(bad, sizeof(bad) - 1, global, strlen(global), &catalog, NULL,
		&snapshot, &error) == SRCFG_SYNTAX);
	assert(srcfg_parse_buffers(apps, strlen(apps), global, strlen(global), &catalog, &options,
		&snapshot, &error) == SRCFG_TOO_LARGE);
	invalid.high_mode_id = 99;
	assert(srcfg_parse_buffers(apps, strlen(apps), global, strlen(global), &invalid, NULL,
		&snapshot, &error) == SRCFG_CATALOG);
	assert(!snapshot);
}

static void dynamic_growth_supports_thousands_not_fixed_28(void)
{
	char *buffer = malloc(80000), *features = malloc(32000);
	size_t i, used = 0, global_used = 0;
	struct srcfg_snapshot *snapshot = NULL;
	struct srcfg_error error;

	assert(buffer && features);
	for (i = 0; i < 2000; i++)
		used += (size_t)snprintf(buffer + used, 80000 - used, "fixture.app%04zu 0 120\n", i);
	global_used += (size_t)snprintf(features, 32000, "enable=1\n");
	for (i = 0; i < 500; i++)
		global_used += (size_t)snprintf(features + global_used, 32000 - global_used, "future_%04zu=value\n", i);
	assert(srcfg_parse_buffers(buffer, used, features, global_used, &catalog, NULL, &snapshot, &error) == SRCFG_OK);
	assert(snapshot->rule_count == 2000 && snapshot->unknown_key_count == 500);
	assert(srp_find_app_rule(snapshot->rules, snapshot->rule_count, "fixture.app1999"));
	srcfg_snapshot_free(snapshot);
	free(buffer); free(features);
}

static void snapshot_copies_catalog_and_strings(void)
{
	struct srp_mode copy_modes[9];
	struct srp_catalog detected = catalog;
	char mutable_apps[] = "fixture.a 0 120";
	struct srcfg_snapshot *snapshot = NULL;
	struct srcfg_error error;

	memcpy(copy_modes, modes, sizeof(copy_modes));
	detected.modes = copy_modes;
	assert(srcfg_parse_buffers(mutable_apps, strlen(mutable_apps), global, strlen(global),
		&detected, NULL, &snapshot, &error) == SRCFG_OK);
	mutable_apps[0] = 'x';
	copy_modes[0].hz = 999;
	assert(!strcmp(snapshot->rules[0].package, "fixture.a"));
	assert(srp_mode_by_id(&snapshot->catalog, 0)->hz == 120);
	srcfg_snapshot_free(snapshot);
}

static void atomic_publish_retires_only_after_complete_success(void)
{
	struct srcfg_snapshot *initial = parse(apps, global, NULL), *retired = NULL, *current;
	_Atomic(struct srcfg_snapshot *) slot = initial;
	struct srcfg_error error;
	const char replacement[] = "enable=1\napps_default=peak\n";
	const char broken[] = "enable=";

	assert(srcfg_replace_buffers(&slot, apps, strlen(apps), replacement, strlen(replacement),
		&catalog, NULL, &retired, &error) == SRCFG_OK);
	current = atomic_load_explicit(&slot, memory_order_acquire);
	assert(current != initial && retired == initial && current->policy.apps_default == SRP_PEAK);
	assert(initial->policy.apps_default == SRP_HIGH && initial->rule_count == 3); /* not freed under reader */
	srcfg_snapshot_free(retired);
	retired = current;
	assert(srcfg_replace_buffers(&slot, apps, strlen(apps), broken, strlen(broken), &catalog,
		NULL, &retired, &error) == SRCFG_VALUE);
	assert(!retired && atomic_load(&slot) == current && current->rule_count == 3);
	srcfg_snapshot_free(current);
}

static void parse_failure_does_not_replace_output(void)
{
	struct srcfg_snapshot *initial = parse(apps, global, NULL), *out = initial;
	struct srcfg_error error;

	assert(srcfg_parse_buffers("broken", 6, global, strlen(global), &catalog, NULL, &out, &error) == SRCFG_SYNTAX);
	assert(out == initial && initial->rule_count == 3);
	srcfg_snapshot_free(initial);
}

static void write_file(const char *path, const char *text)
{
	int fd = open(path, O_WRONLY | O_TRUNC);
	size_t len = strlen(text);

	assert(fd >= 0 && write(fd, text, len) == (ssize_t)len && close(fd) == 0);
}

static void read_only_file_load_and_failure_preserves_current(void)
{
	char app_path[] = "/tmp/scene-refresh-apps-XXXXXX", global_path[] = "/tmp/scene-refresh-global-XXXXXX";
	int a = mkstemp(app_path), g = mkstemp(global_path);
	struct srcfg_snapshot *initial = NULL, *retired = NULL;
	_Atomic(struct srcfg_snapshot *) slot = NULL;
	struct srcfg_error error;
	struct stat before, after;

	assert(a >= 0 && g >= 0 && !close(a) && !close(g));
	write_file(app_path, apps); write_file(global_path, global);
	assert(!stat(app_path, &before));
	assert(srcfg_replace_paths(&slot, app_path, global_path, &catalog, NULL, &retired, &error) == SRCFG_OK);
	assert(!retired);
	initial = atomic_load(&slot);
	assert(initial && initial->rule_count == 3 && !stat(app_path, &after));
	assert(before.st_size == after.st_size && before.st_mtim.tv_sec == after.st_mtim.tv_sec &&
	       before.st_mtim.tv_nsec == after.st_mtim.tv_nsec);
	write_file(app_path, "fixture.incomplete 60");
	assert(srcfg_replace_paths(&slot, app_path, global_path, &catalog, NULL, &retired, &error) == SRCFG_SYNTAX);
	assert(!retired && atomic_load(&slot) == initial);
	assert(srcfg_replace_paths(&slot, "/does-not-exist/scene.conf", global_path,
		&catalog, NULL, &retired, &error) == SRCFG_IO);
	assert(error.sys_errno == ENOENT && atomic_load(&slot) == initial);
	assert(!unlink(app_path) && !unlink(global_path));
	srcfg_snapshot_free(initial);
}

static void every_truncated_prefix_is_memory_safe(void)
{
	size_t i, j;

	for (i = 0; i <= strlen(apps); i++) {
		for (j = 0; j <= strlen(global); j++) {
			struct srcfg_snapshot *snapshot = NULL;
			struct srcfg_error error;
			enum srcfg_status status = srcfg_parse_buffers(apps, i, global, j,
				&catalog, NULL, &snapshot, &error);

			assert((status == SRCFG_OK) == (snapshot != NULL));
			srcfg_snapshot_free(snapshot);
		}
	}
}

static void non_regular_config_is_rejected_without_waiting_for_a_writer(void)
{
	char fifo_path[] = "/tmp/scene-refresh-nonregular-XXXXXX";
	char global_path[] = "/tmp/scene-refresh-regular-XXXXXX";
	int a = mkstemp(fifo_path), g = mkstemp(global_path);
	struct srcfg_snapshot *snapshot = NULL;
	struct srcfg_error error;

	assert(a >= 0 && g >= 0 && !close(a) && !close(g));
	assert(!unlink(fifo_path) && !mkfifo(fifo_path, 0600));
	write_file(global_path, global);
	alarm(2); /* Regression bound: blocking O_RDONLY on this FIFO would hang. */
	assert(srcfg_load_paths(fifo_path, global_path, &catalog, NULL, &snapshot, &error) == SRCFG_IO);
	alarm(0);
	assert(!snapshot && error.source == SRCFG_SOURCE_APPS && error.sys_errno == EINVAL);
	assert(!unlink(fifo_path) && !unlink(global_path));
}

#ifdef SRCFG_TEST_WRAP
static void each_allocation_failure_preserves_old_and_reclaims_candidate(void)
{
	struct srcfg_snapshot *initial = parse(apps, global, NULL), *retired;
	_Atomic(struct srcfg_snapshot *) slot = initial;
	size_t n, failures = 0;

	for (n = 1; n < 100; n++) {
		struct srcfg_error error;
		enum srcfg_status status;

		allocations = 0;
		fail_allocation = n;
		status = srcfg_replace_buffers(&slot, apps, strlen(apps), global, strlen(global),
			&catalog, NULL, &retired, &error);
		fail_allocation = 0;
		if (status == SRCFG_OK) {
			assert(retired == initial);
			srcfg_snapshot_free(atomic_load(&slot));
			break;
		}
		assert(status == SRCFG_NOMEM && !retired && atomic_load(&slot) == initial);
		assert(initial->rule_count == 3);
		failures++;
	}
	assert(n < 100 && failures > 15);
	srcfg_snapshot_free(initial);
}

static void rename_between_read_and_stat_is_retryable_without_publish(void)
{
	char app_path[] = "/tmp/scene-refresh-race-apps-XXXXXX";
	char global_path[] = "/tmp/scene-refresh-race-global-XXXXXX";
	char new_path[] = "/tmp/scene-refresh-race-next-XXXXXX";
	int a = mkstemp(app_path), g = mkstemp(global_path), n = mkstemp(new_path);
	struct srcfg_snapshot *initial = parse(apps, global, NULL), *retired = NULL;
	_Atomic(struct srcfg_snapshot *) slot = initial;
	struct srcfg_error error;
	enum srcfg_status status;

	assert(a >= 0 && g >= 0 && n >= 0 && !close(a) && !close(g) && !close(n));
	write_file(app_path, apps); write_file(global_path, global); write_file(new_path, "fixture.new 60 165\n");
	rename_from = new_path; rename_to = app_path;
	status = srcfg_replace_paths(&slot, app_path, global_path, &catalog, NULL, &retired, &error);
	if (status != SRCFG_CHANGED)
		fprintf(stderr, "rename injection: status=%d source=%d errno=%d hook_pending=%d\n",
			status, error.source, error.sys_errno, rename_from != NULL);
	assert(status == SRCFG_CHANGED);
	assert(!rename_from && !retired && atomic_load(&slot) == initial);
	assert(initial->rule_count == 3 && error.source == SRCFG_SOURCE_APPS);
	assert(!unlink(app_path) && !unlink(global_path));
	srcfg_snapshot_free(initial);
}
#endif

#define RUN(test) do { test(); count++; } while (0)
int main(void)
{
	unsigned int count = 0;

	RUN(actual_defaults_and_stored_game_range);
	RUN(parses_all_confirmed_global_settings);
	RUN(mode_overrides_are_ids_and_leave_peak);
	RUN(invalid_mode_override_is_not_hz_or_other_group);
	RUN(caller_detection_is_not_hardcoded_to_phone_ids);
	RUN(duplicates_last_win_and_remain_diagnosable);
	RUN(unknown_keys_and_empty_unknown_values_preserved);
	RUN(native_normalizations_keep_original_value_for_diagnostics);
	RUN(platform_defaults_and_missing_enable_are_explicit);
	RUN(whitespace_comments_and_nonterminated_buffers);
	RUN(empty_requires_explicit_intent);
	RUN(bad_app_rows_report_exact_reason_and_line);
	RUN(bad_global_rows_do_not_silently_fallback);
	RUN(embedded_nul_input_caps_and_invalid_catalog);
	RUN(dynamic_growth_supports_thousands_not_fixed_28);
	RUN(snapshot_copies_catalog_and_strings);
	RUN(atomic_publish_retires_only_after_complete_success);
	RUN(parse_failure_does_not_replace_output);
	RUN(read_only_file_load_and_failure_preserves_current);
	RUN(every_truncated_prefix_is_memory_safe);
	RUN(non_regular_config_is_rejected_without_waiting_for_a_writer);
#ifdef SRCFG_TEST_WRAP
	RUN(each_allocation_failure_preserves_old_and_reclaims_candidate);
	RUN(rename_between_read_and_stat_is_retryable_without_publish);
#endif
	printf("scene_refresh_config: %u tests passed\n", count);
	return 0;
}
