#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#include "xcy_reclaim_protection.h"

#define XCY_SCENE_NAMES_MAX 128U

enum xcy_scene_field {
	XCY_SCENE_VERSION,
	XCY_SCENE_SOURCE,
	XCY_SCENE_TIME,
	XCY_SCENE_UNMAPPED,
	XCY_SCENE_FOREGROUND,
	XCY_SCENE_FOCUSED,
	XCY_SCENE_ACTIVE,
	XCY_SCENE_VISIBLE,
	XCY_SCENE_MAPPED,
	XCY_SCENE_WINDOWED,
	XCY_SCENE_SMALL,
	XCY_SCENE_ANALYZED_SMALL,
	XCY_SCENE_SYSTEM_SMALL,
	XCY_SCENE_FIELDS,
};

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

static int xcy_scene_add(const char **names, size_t *count, const char *value)
{
	xcy_memory_token_t token = { .text = value, .len = strlen(value) };
	size_t i;

	if (!xcy_memory_package(token))
		return -EINVAL;
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
	static const char *const keys[XCY_SCENE_FIELDS] = {
		[XCY_SCENE_VERSION] = "version",
		[XCY_SCENE_SOURCE] = "source",
		[XCY_SCENE_TIME] = "time_sec",
		[XCY_SCENE_UNMAPPED] = "unmapped_shell_count",
		[XCY_SCENE_FOREGROUND] = "scene_foreground_pkg",
		[XCY_SCENE_FOCUSED] = "focused_pkg",
		[XCY_SCENE_ACTIVE] = "active_pkg",
		[XCY_SCENE_VISIBLE] = "visible_pkgs",
		[XCY_SCENE_MAPPED] = "mapped_shell_pkgs",
		[XCY_SCENE_WINDOWED] = "windowed_pkgs",
		[XCY_SCENE_SMALL] = "small_window_pkgs",
		[XCY_SCENE_ANALYZED_SMALL] = "scene_analyzed_small_window_pkgs",
		[XCY_SCENE_SYSTEM_SMALL] = "scene_include_system_small_window_pkgs",
	};
	char *value[XCY_SCENE_FIELDS] = {0};
	char *cursor = data;
	uint64_t observed, unmapped, age;
	size_t i;
	int error = 0;

	if (!data || !*data || data[strlen(data) - 1] != '\n')
		return -EINVAL;
	while (cursor && *cursor) {
		char *line = xcy_scene_next(&cursor, '\n');
		char *entry = strchr(line, '=');

		if (!entry || entry == line)
			return -EINVAL;
		*entry++ = 0;
		for (i = 0; i < XCY_SCENE_FIELDS; ++i)
			if (!strcmp(line, keys[i])) {
				if (value[i])
					return -EINVAL;
				value[i] = entry;
				break;
			}
	}
	for (i = 0; i < XCY_SCENE_FIELDS; ++i)
		if (!value[i])
			return -EINVAL;
	if (strcmp(value[XCY_SCENE_VERSION], "1") ||
	    !xcy_scene_uint(value[XCY_SCENE_TIME], &observed) ||
	    !xcy_scene_uint(value[XCY_SCENE_UNMAPPED], &unmapped))
		return -EINVAL;
	if (!observed)
		observed = fallback_sec;
	if (strcmp(value[XCY_SCENE_SOURCE], "scene_accessibility") ||
	    unmapped || !observed || wall_now_sec < observed ||
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
	for (i = XCY_SCENE_FOREGROUND; i < XCY_SCENE_FIELDS && !error; ++i) {
		char *pkg;

		if (!*value[i])
			continue;
		if (i < XCY_SCENE_VISIBLE)
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
