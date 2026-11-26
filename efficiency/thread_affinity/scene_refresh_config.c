/* SPDX-License-Identifier: GPL-2.0 */
#define _POSIX_C_SOURCE 200809L
#include "scene_refresh_config.h"

#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

/*
 * Pinned Scene: ed1.f and native 4ea070 use map assignment (last duplicate wins).
 * We preserve valid values/defaults, but reject a broken candidate rather than
 * copying the native parser's silent bad-line/default fallback. Empty input is
 * opt-in: original ed1's empty map can reload APK assets, which we cannot guess.
 */

struct rule_line { struct srp_app_rule rule; size_t line; };

static enum srcfg_status fail(struct srcfg_error *error, enum srcfg_status code,
		enum srcfg_source source, size_t line)
{
	if (error) {
		error->code = code;
		error->source = source;
		error->line = line;
		error->sys_errno = 0;
	}
	return code;
}

static enum srcfg_status io_fail(struct srcfg_error *error,
		enum srcfg_source source)
{
	int saved = errno;

	fail(error, SRCFG_IO, source, 0);
	if (error)
		error->sys_errno = saved;
	return SRCFG_IO;
}

static size_t limit(const struct srcfg_options *options)
{
	return options && options->max_bytes ? options->max_bytes : 16U * 1024U * 1024U;
}

static char *duplicate(const char *value, size_t len)
{
	char *copy;

	if (len == SIZE_MAX)
		return NULL;
	copy = malloc(len + 1);
	if (copy) {
		memcpy(copy, value, len);
		copy[len] = '\0';
	}
	return copy;
}

static char *trim(char *value)
{
	char *end;

	while (*value && isspace((unsigned char)*value))
		value++;
	end = value + strlen(value);
	while (end > value && isspace((unsigned char)end[-1]))
		*--end = '\0';
	return value;
}

static enum srcfg_status number(const char *value, int32_t *out)
{
	const char *p = value;
	char *end;
	long long parsed;

	if (*p == '+' || *p == '-')
		p++;
	if (!*p)
		return SRCFG_VALUE;
	for (; *p; p++)
		if (!isdigit((unsigned char)*p))
			return SRCFG_VALUE;
	errno = 0;
	parsed = strtoll(value, &end, 10);
	if (errno == ERANGE || parsed < INT32_MIN || parsed > INT32_MAX)
		return SRCFG_OVERFLOW;
	if (*end)
		return SRCFG_VALUE;
	*out = (int32_t)parsed;
	return SRCFG_OK;
}

static enum srcfg_status boolean_value(const char *value, bool *out)
{
	if (!strcmp(value, "1") || !strcmp(value, "true"))
		*out = true;
	else if (!strcmp(value, "0") || !strcmp(value, "false"))
		*out = false;
	else
		return SRCFG_VALUE;
	return SRCFG_OK;
}

static enum srcfg_status level_value(const char *value, enum srp_level *out)
{
	if (!strcmp(value, "high")) *out = SRP_HIGH;
	else if (!strcmp(value, "middle")) *out = SRP_MIDDLE;
	else if (!strcmp(value, "low")) *out = SRP_LOW;
	else if (!strcmp(value, "peak")) *out = SRP_PEAK;
	else return SRCFG_VALUE;
	return SRCFG_OK;
}

/* Returns count, or max+1 if there are extra fields. */
static size_t fields(char *line, char **out, size_t max)
{
	size_t count = 0;
	char *p = line;

	while (*p) {
		while (*p && isspace((unsigned char)*p)) p++;
		if (!*p) break;
		if (count == max) return max + 1;
		out[count++] = p;
		while (*p && !isspace((unsigned char)*p)) p++;
		if (*p) *p++ = '\0';
	}
	return count;
}

static enum srcfg_status grow(void **array, size_t *capacity, size_t count,
		size_t element_size)
{
	size_t next;
	void *replacement;

	if (count < *capacity)
		return SRCFG_OK;
	next = *capacity ? *capacity : 16;
	if (*capacity) {
		if (next > SIZE_MAX / 2)
			return SRCFG_OVERFLOW;
		next *= 2;
	}
	if (next > SIZE_MAX / element_size)
		return SRCFG_OVERFLOW;
	replacement = realloc(*array, next * element_size);
	if (!replacement)
		return SRCFG_NOMEM;
	*array = replacement;
	*capacity = next;
	return SRCFG_OK;
}

static int compare_entries(const void *a, const void *b)
{
	const struct srcfg_entry *left = a, *right = b;
	int order = strcmp(left->key, right->key);

	return order ? order : (left->line > right->line) - (left->line < right->line);
}

static int compare_rules(const void *a, const void *b)
{
	const struct rule_line *left = a, *right = b;
	int order = strcmp(left->rule.package, right->rule.package);

	return order ? order : (left->line > right->line) - (left->line < right->line);
}

static enum srcfg_status parse_global(char *data, struct srcfg_snapshot *snapshot,
		const struct srcfg_options *options, struct srcfg_error *error)
{
	char *line = data;
	size_t line_number = 0, capacity = 0, i, j, used;
	enum srcfg_status status;

	while (line) {
		char *next = strchr(line, '\n'), *key, *value, *equals;
		struct srcfg_entry entry = {0};
		void *storage;

		line_number++;
		if (next) *next++ = '\0';
		key = trim(line);
		line = next;
		if (!*key || *key == '#') continue;
		equals = strchr(key, '=');
		if (!equals || strchr(equals + 1, '='))
			return fail(error, SRCFG_SYNTAX, SRCFG_SOURCE_GLOBAL, line_number);
		*equals = '\0';
		key = trim(key);
		value = trim(equals + 1);
		if (!*key)
			return fail(error, SRCFG_SYNTAX, SRCFG_SOURCE_GLOBAL, line_number);
		storage = snapshot->entries;
		status = grow(&storage, &capacity,
			snapshot->entry_count, sizeof(*snapshot->entries));
		if (status != SRCFG_OK)
			return fail(error, status, SRCFG_SOURCE_GLOBAL, line_number);
		snapshot->entries = storage;
		entry.key = duplicate(key, strlen(key));
		entry.value = duplicate(value, strlen(value));
		if (!entry.key || !entry.value) {
			free(entry.key); free(entry.value);
			return fail(error, SRCFG_NOMEM, SRCFG_SOURCE_GLOBAL, line_number);
		}
		entry.line = line_number;
		entry.occurrences = 1;
		snapshot->entries[snapshot->entry_count++] = entry;
	}
	if (!snapshot->entry_count && !(options && (options->flags & SRCFG_ALLOW_EMPTY_GLOBAL)))
		return fail(error, SRCFG_EMPTY, SRCFG_SOURCE_GLOBAL, 0);
	if (snapshot->entry_count > 1)
		qsort(snapshot->entries, snapshot->entry_count, sizeof(*snapshot->entries), compare_entries);
	used = 0;
	for (i = 0; i < snapshot->entry_count; i = j) {
		struct srcfg_entry last;

		for (j = i + 1; j < snapshot->entry_count &&
		     !strcmp(snapshot->entries[i].key, snapshot->entries[j].key); j++) { }
		last = snapshot->entries[j - 1];
		last.occurrences = j - i;
		snapshot->duplicate_key_count += j - i - 1;
		for (; i + 1 < j; i++) {
			free(snapshot->entries[i].key);
			free(snapshot->entries[i].value);
		}
		snapshot->entries[used++] = last;
	}
	snapshot->entry_count = used;
	return SRCFG_OK;
}

static enum srcfg_status display_overrides(char *value, struct srcfg_snapshot *snapshot)
{
	char *tokens[3], *copy = duplicate(value, strlen(value));
	int32_t ids[3];
	size_t i;
	enum srcfg_status status = SRCFG_OK;

	if (!copy) return SRCFG_NOMEM;
	if (fields(copy, tokens, 3) != 3) { status = SRCFG_VALUE; goto done; }
	for (i = 0; i < 3; i++) {
		status = number(tokens[i], &ids[i]);
		if (status != SRCFG_OK) goto done;
		if (ids[i] >= 0 && !srp_mode_by_id(&snapshot->catalog, ids[i])) {
			status = SRCFG_CATALOG;
			goto done;
		}
	}
	if (ids[0] >= 0) snapshot->catalog.high_mode_id = ids[0];
	if (ids[1] >= 0) snapshot->catalog.middle_mode_id = ids[1];
	if (ids[2] >= 0) snapshot->catalog.low_mode_id = ids[2];
done:
	free(copy);
	return status;
}

static enum srcfg_status apply_globals(struct srcfg_snapshot *snapshot,
		struct srcfg_error *error)
{
	size_t i;
	uint32_t supported_peak = 0;

	for (i = 0; i < snapshot->catalog.count; i++)
		if (snapshot->catalog.modes[i].hz > supported_peak)
			supported_peak = snapshot->catalog.modes[i].hz;
	for (i = 0; i < snapshot->entry_count; i++) {
		struct srcfg_entry *entry = &snapshot->entries[i];
		struct srp_config *config = &snapshot->policy;
		const char *key = entry->key;
		enum srcfg_status status = SRCFG_OK;
		int32_t parsed;

		entry->known = true;
#define LEVEL(name, member) if (!strcmp(key, name)) status = level_value(entry->value, &config->member)
#define BOOL(name, member) if (!strcmp(key, name)) status = boolean_value(entry->value, &config->member)
		LEVEL("apps_default", apps_default);
		else LEVEL("apps_inactive", apps_inactive);
		else LEVEL("games_default", games_default);
		else LEVEL("gesture", gesture);
		else LEVEL("screen_off", screen_off);
		else LEVEL("low_battery_mode", low_battery_mode);
		else LEVEL("low_brightness", low_brightness);
		else BOOL("enable", enabled);
		else BOOL("strict", strict);
		else BOOL("compatible", compatible);
		else BOOL("disable_ltpo", disable_ltpo);
		else if (!strcmp(key, "display_modes"))
			status = display_overrides(entry->value, snapshot);
		else if (!strcmp(key, "low_battery_threshold") || !strcmp(key, "brightness_threshold") ||
			 !strcmp(key, "tap_delay") || !strcmp(key, "scroll_delay") ||
			 !strcmp(key, "normal_peak_rr") || !strcmp(key, "oc_step_interval")) {
			status = number(entry->value, &parsed);
			if (status == SRCFG_OK) {
				if (!strcmp(key, "low_battery_threshold")) config->low_battery_threshold = parsed;
				else if (!strcmp(key, "brightness_threshold")) {
					config->brightness_threshold = parsed >= 0 && parsed <= 100 ? parsed : 0;
					if (config->brightness_threshold != parsed) snapshot->normalized_value_count++;
				} else if (!strcmp(key, "tap_delay")) {
					config->tap_delay_ms = parsed > 0 ? parsed : 900;
					if (config->tap_delay_ms != parsed) snapshot->normalized_value_count++;
				} else if (!strcmp(key, "scroll_delay")) {
					config->scroll_delay_ms = parsed > 0 ? parsed : 2000;
					if (config->scroll_delay_ms != parsed) snapshot->normalized_value_count++;
				} else if (parsed < 0) status = SRCFG_VALUE;
				else if (!strcmp(key, "normal_peak_rr")) config->normal_peak_hz = (uint32_t)parsed;
				else config->oc_step_interval_ms = (uint32_t)parsed;
			}
		} else {
			entry->known = false;
			snapshot->unknown_key_count++;
		}
#undef LEVEL
#undef BOOL
		if (status != SRCFG_OK)
			return fail(error, status, SRCFG_SOURCE_GLOBAL, entry->line);
	}
	if (snapshot->policy.normal_peak_hz > supported_peak)
		snapshot->policy.normal_peak_hz = supported_peak;
	return SRCFG_OK;
}

static enum srcfg_status parse_apps(char *data, struct srcfg_snapshot *snapshot,
		const struct srcfg_options *options, struct srcfg_error *error)
{
	struct rule_line *rules = NULL;
	size_t count = 0, capacity = 0, line_number = 0, i, j;
	char *line = data;
	enum srcfg_status status = SRCFG_OK;

	while (line) {
		char *next = strchr(line, '\n'), *tokens[3], *text;
		struct rule_line row = {0};
		void *storage;

		line_number++;
		if (next) *next++ = '\0';
		text = trim(line);
		line = next;
		if (!*text || *text == '#') continue;
		if (fields(text, tokens, 3) != 3) { status = SRCFG_SYNTAX; goto failed; }
		status = number(tokens[1], &row.rule.min_hz);
		if (status != SRCFG_OK) goto failed;
		status = number(tokens[2], &row.rule.max_hz);
		if (status != SRCFG_OK) goto failed;
		if (row.rule.min_hz < 0 || row.rule.max_hz < 0) { status = SRCFG_VALUE; goto failed; }
		if ((row.rule.min_hz && !srp_mode_for_rule_hz(&snapshot->catalog, row.rule.min_hz)) ||
		    (row.rule.max_hz && !srp_mode_for_rule_hz(&snapshot->catalog, row.rule.max_hz))) {
			status = SRCFG_UNSUPPORTED_RATE;
			goto failed;
		}
		storage = rules;
		status = grow(&storage, &capacity, count, sizeof(*rules));
		if (status != SRCFG_OK) goto failed;
		rules = storage;
		row.rule.package = duplicate(tokens[0], strlen(tokens[0]));
		if (!row.rule.package) { status = SRCFG_NOMEM; goto failed; }
		row.line = line_number;
		rules[count++] = row;
	}
	if (!count && !(options && (options->flags & SRCFG_ALLOW_EMPTY_APPS))) {
		status = SRCFG_EMPTY;
		line_number = 0;
		goto failed;
	}
	if (count > SIZE_MAX / sizeof(*snapshot->rules)) { status = SRCFG_OVERFLOW; goto failed; }
	if (count) {
		snapshot->rules = malloc(count * sizeof(*snapshot->rules));
		if (!snapshot->rules) { status = SRCFG_NOMEM; goto failed; }
	}
	if (count > 1) qsort(rules, count, sizeof(*rules), compare_rules);
	for (i = 0; i < count; i = j) {
		for (j = i + 1; j < count && !strcmp(rules[i].rule.package, rules[j].rule.package); j++) { }
		snapshot->rules[snapshot->rule_count++] = rules[j - 1].rule;
		rules[j - 1].rule.package = NULL;
		snapshot->duplicate_app_count += j - i - 1;
	}
failed:
	for (i = 0; i < count; i++) free((void *)rules[i].rule.package);
	free(rules);
	return status == SRCFG_OK ? status : fail(error, status, SRCFG_SOURCE_APPS, line_number);
}

void srcfg_snapshot_free(struct srcfg_snapshot *snapshot)
{
	size_t i;

	if (!snapshot) return;
	for (i = 0; i < snapshot->rule_count; i++) free((void *)snapshot->rules[i].package);
	for (i = 0; i < snapshot->entry_count; i++) {
		free(snapshot->entries[i].key);
		free(snapshot->entries[i].value);
	}
	free(snapshot->rules);
	free(snapshot->entries);
	free((void *)snapshot->catalog.modes);
	free(snapshot);
}

enum srcfg_status srcfg_parse_buffers(const char *apps, size_t apps_len,
		const char *global, size_t global_len, const struct srp_catalog *catalog,
		const struct srcfg_options *options, struct srcfg_snapshot **out,
		struct srcfg_error *error)
{
	struct srcfg_snapshot *snapshot = NULL;
	struct srp_mode *owned_modes;
	char *apps_copy = NULL, *global_copy = NULL;
	enum srcfg_status status;
	size_t maximum = limit(options);

	if (!out || (!apps && apps_len) || (!global && global_len))
		return fail(error, SRCFG_INVALID_ARGUMENT, SRCFG_SOURCE_NONE, 0);
	if (apps_len > maximum || global_len > maximum - apps_len)
		return fail(error, SRCFG_TOO_LARGE, SRCFG_SOURCE_NONE, 0);
	if (apps_len == SIZE_MAX || global_len == SIZE_MAX)
		return fail(error, SRCFG_OVERFLOW, SRCFG_SOURCE_NONE, 0);
	if ((apps_len && memchr(apps, '\0', apps_len)) || (global_len && memchr(global, '\0', global_len)))
		return fail(error, SRCFG_SYNTAX, SRCFG_SOURCE_NONE, 0);
	if (!catalog || catalog->count > SIZE_MAX / sizeof(*owned_modes) ||
	    srp_validate_catalog(catalog) != SRP_OK)
		return fail(error, SRCFG_CATALOG, SRCFG_SOURCE_CATALOG, 0);
	snapshot = calloc(1, sizeof(*snapshot));
	owned_modes = malloc(catalog->count * sizeof(*owned_modes));
	apps_copy = duplicate(apps ? apps : "", apps_len);
	global_copy = duplicate(global ? global : "", global_len);
	if (!snapshot || !owned_modes || !apps_copy || !global_copy) {
		free(owned_modes);
		status = fail(error, SRCFG_NOMEM, SRCFG_SOURCE_NONE, 0);
		goto done;
	}
	memcpy(owned_modes, catalog->modes, catalog->count * sizeof(*owned_modes));
	snapshot->catalog = *catalog;
	snapshot->catalog.modes = owned_modes;
	srp_config_default(&snapshot->policy);
	if (options) {
		snapshot->policy.strict = options->default_strict;
		snapshot->policy.compatible = options->default_compatible;
		snapshot->policy.video_optimize = options->video_optimize;
	}
	status = parse_global(global_copy, snapshot, options, error);
	if (status != SRCFG_OK) goto done;
	status = apply_globals(snapshot, error);
	if (status != SRCFG_OK) goto done;
	if (srp_validate_catalog(&snapshot->catalog) != SRP_OK || srp_validate_config(&snapshot->policy) != SRP_OK) {
		status = fail(error, SRCFG_CATALOG, SRCFG_SOURCE_CATALOG, 0);
		goto done;
	}
	status = parse_apps(apps_copy, snapshot, options, error);
	if (status != SRCFG_OK) goto done;
	*out = snapshot;
	snapshot = NULL;
	status = fail(error, SRCFG_OK, SRCFG_SOURCE_NONE, 0);
done:
	free(apps_copy);
	free(global_copy);
	srcfg_snapshot_free(snapshot);
	return status;
}

static bool same_stat(const struct stat *before, const struct stat *after)
{
	return before->st_dev == after->st_dev && before->st_ino == after->st_ino &&
	       before->st_size == after->st_size &&
	       before->st_mtim.tv_sec == after->st_mtim.tv_sec &&
	       before->st_mtim.tv_nsec == after->st_mtim.tv_nsec &&
	       before->st_ctim.tv_sec == after->st_ctim.tv_sec &&
	       before->st_ctim.tv_nsec == after->st_ctim.tv_nsec;
}

static enum srcfg_status read_contents(int fd, size_t size, char **out,
		enum srcfg_source source, struct srcfg_error *error)
{
	size_t offset = 0;
	char *data = malloc(size ? size : 1);

	if (!data) return fail(error, SRCFG_NOMEM, source, 0);
	while (offset < size) {
		size_t chunk = size - offset > 1024U * 1024U ? 1024U * 1024U : size - offset;
		ssize_t got = read(fd, data + offset, chunk);

		if (got < 0 && errno == EINTR) continue;
		if (got < 0) { free(data); return io_fail(error, source); }
		if (!got) { free(data); return fail(error, SRCFG_CHANGED, source, 0); }
		offset += (size_t)got;
	}
	*out = data;
	return SRCFG_OK;
}

enum srcfg_status srcfg_load_paths(const char *apps_path, const char *global_path,
		const struct srp_catalog *catalog, const struct srcfg_options *options,
		struct srcfg_snapshot **out, struct srcfg_error *error)
{
	const char *paths[2] = {apps_path, global_path};
	int fds[2] = {-1, -1};
	char *data[2] = {NULL, NULL};
	struct stat before[2], after, named;
	size_t sizes[2], i, maximum = limit(options);
	enum srcfg_status status = SRCFG_OK;

	if (!apps_path || !global_path || !out)
		return fail(error, SRCFG_INVALID_ARGUMENT, SRCFG_SOURCE_NONE, 0);
	for (i = 0; i < 2; i++) {
		enum srcfg_source source = i ? SRCFG_SOURCE_GLOBAL : SRCFG_SOURCE_APPS;

		/* Do not block on a FIFO/device substituted for a config before fstat. */
		fds[i] = open(paths[i], O_RDONLY | O_CLOEXEC | O_NONBLOCK);
		if (fds[i] < 0 || fstat(fds[i], &before[i]) < 0) { status = io_fail(error, source); goto done; }
		if (!S_ISREG(before[i].st_mode)) { errno = EINVAL; status = io_fail(error, source); goto done; }
		if (before[i].st_size < 0 || (uintmax_t)before[i].st_size > maximum) {
			status = fail(error, SRCFG_TOO_LARGE, source, 0);
			goto done;
		}
		sizes[i] = (size_t)before[i].st_size;
	}
	if (sizes[0] > maximum || sizes[1] > maximum - sizes[0]) {
		status = fail(error, SRCFG_TOO_LARGE, SRCFG_SOURCE_NONE, 0);
		goto done;
	}
	for (i = 0; i < 2; i++) {
		enum srcfg_source source = i ? SRCFG_SOURCE_GLOBAL : SRCFG_SOURCE_APPS;

		status = read_contents(fds[i], sizes[i], &data[i], source, error);
		if (status != SRCFG_OK) goto done;
	}
	for (i = 0; i < 2; i++) {
		enum srcfg_source source = i ? SRCFG_SOURCE_GLOBAL : SRCFG_SOURCE_APPS;

		if (fstat(fds[i], &after) < 0) { status = io_fail(error, source); goto done; }
		if (stat(paths[i], &named) < 0) { status = io_fail(error, source); goto done; }
		if (!same_stat(&before[i], &after) || !same_stat(&before[i], &named)) {
			status = fail(error, SRCFG_CHANGED, source, 0);
			goto done;
		}
	}
	status = srcfg_parse_buffers(data[0], sizes[0], data[1], sizes[1], catalog, options, out, error);
done:
	for (i = 0; i < 2; i++) {
		if (fds[i] >= 0) close(fds[i]);
		free(data[i]);
	}
	return status;
}

enum srcfg_status srcfg_replace_buffers(_Atomic(struct srcfg_snapshot *) *current,
		const char *apps, size_t apps_len, const char *global, size_t global_len,
		const struct srp_catalog *catalog, const struct srcfg_options *options,
		struct srcfg_snapshot **retired, struct srcfg_error *error)
{
	struct srcfg_snapshot *candidate = NULL;
	enum srcfg_status status;

	if (!current || !retired) return fail(error, SRCFG_INVALID_ARGUMENT, SRCFG_SOURCE_NONE, 0);
	*retired = NULL;
	status = srcfg_parse_buffers(apps, apps_len, global, global_len, catalog, options, &candidate, error);
	if (status == SRCFG_OK)
		*retired = atomic_exchange_explicit(current, candidate, memory_order_acq_rel);
	return status;
}

enum srcfg_status srcfg_replace_paths(_Atomic(struct srcfg_snapshot *) *current,
		const char *apps_path, const char *global_path,
		const struct srp_catalog *catalog, const struct srcfg_options *options,
		struct srcfg_snapshot **retired, struct srcfg_error *error)
{
	struct srcfg_snapshot *candidate = NULL;
	enum srcfg_status status;

	if (!current || !retired) return fail(error, SRCFG_INVALID_ARGUMENT, SRCFG_SOURCE_NONE, 0);
	*retired = NULL;
	status = srcfg_load_paths(apps_path, global_path, catalog, options, &candidate, error);
	if (status == SRCFG_OK)
		*retired = atomic_exchange_explicit(current, candidate, memory_order_acq_rel);
	return status;
}
