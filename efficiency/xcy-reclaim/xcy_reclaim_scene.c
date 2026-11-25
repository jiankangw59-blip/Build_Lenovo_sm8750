#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#include "xcy_reclaim_protection.h"

#define XCY_SCENE_NAMES_MAX 128U
#define XCY_SCENE_FIELDS_MAX 9U

static char *xcy_scene_next(char **cursor, char delimiter)
{
	char *item = *cursor, *next;

	if (!item)
		return NULL;
	next = strchr(item, delimiter);
	if (next)
		*next++ = 0;
	*cursor = next;
	return item;
}

static bool xcy_scene_name(const char *value, size_t length)
{
	xcy_memory_token_t token = { .text = value, .len = length };

	return length && length < XCY_MEMORY_PACKAGE_BYTES && xcy_memory_package(token);
}

static int xcy_scene_add(const char **names, size_t *count, const char *value)
{
	size_t i;

	if (!xcy_scene_name(value, strlen(value)))
		return 0;
	for (i = 0; i < *count; ++i)
		if (!strcmp(names[i], value))
			return 0;
	if (*count == XCY_SCENE_NAMES_MAX)
		return -E2BIG;
	names[(*count)++] = value;
	return 0;
}

static bool xcy_scene_uint(const char *value, uint64_t *out)
{
	xcy_memory_token_t token = { .text = value, .len = strlen(value) };

	return xcy_memory_uint(token, out);
}

/* The pointers in the frame refer to the mutable, NUL-terminated file copy.
 * Ownership of that allocation transfers only when parsing succeeds. */
int xcy_reclaim_scene_parse(char *data, uint64_t wall_now_sec, uint64_t fallback_sec,
			    uint64_t now, uint64_t maximum_age,
			    struct xcy_reclaim_protection_frame *frame)
{
	static const char *const keys[XCY_SCENE_FIELDS_MAX] = {
		"scene_foreground_pkg", "focused_pkg", "active_pkg",
		"visible_pkgs", "mapped_shell_pkgs", "windowed_pkgs", "small_window_pkgs",
		"scene_analyzed_small_window_pkgs", "scene_include_system_small_window_pkgs",
	};
	char *value[XCY_SCENE_FIELDS_MAX] = {0};
	char *cursor = data, *line;
	uint64_t observed = 0, unmapped = 0, age;
	bool source = false;
	size_t i;
	int error = 0;

	while ((line = xcy_scene_next(&cursor, '\n'))) {
		char *entry = strchr(line, '=');

		if (!entry)
			continue;
		*entry++ = 0;
		if (!strcmp(line, "source"))
			source = !strcmp(entry, "scene_accessibility");
		if (!strcmp(line, "time_sec") && !xcy_scene_uint(entry, &observed))
			return -EINVAL;
		if (!strcmp(line, "unmapped_shell_count") && !xcy_scene_uint(entry, &unmapped))
			return -EINVAL;
		for (i = 0; i < XCY_SCENE_FIELDS_MAX; ++i)
			if (!strcmp(line, keys[i])) {
				value[i] = entry;
				break;
			}
	}
	if (!observed)
		observed = fallback_sec;
	if (!source || unmapped || !observed || wall_now_sec < observed ||
	    wall_now_sec - observed > maximum_age / 1000U)
		return -ESTALE;
	age = (wall_now_sec - observed) * 1000U;
	if (age >= now)
		return -ESTALE;
	frame->names = xcy_snapshot_calloc(XCY_SCENE_NAMES_MAX * 2, sizeof(*frame->names));
	if (!frame->names)
		return -ENOMEM;
	frame->view.visible.values = frame->names;
	frame->view.active.values = frame->names + XCY_SCENE_NAMES_MAX;
	for (i = 0; i < XCY_SCENE_FIELDS_MAX && !error; ++i) {
		char *pkg;

		if (!value[i])
			continue;
		if (i < 3)
			error = xcy_scene_add(frame->names + XCY_SCENE_NAMES_MAX,
					      &frame->view.active.count, value[i]);
		while (!error && (pkg = xcy_scene_next(&value[i], ',')))
			error = xcy_scene_add(frame->names, &frame->view.visible.count, pkg);
	}
	if (error || !frame->view.visible.count) {
		xcy_reclaim_protection_free(frame);
		return error ? error : -ESTALE;
	}
	frame->storage = data;
	frame->scene_observed_ms = now - age;
	frame->scene_valid_ms = maximum_age;
	return 0;
}
