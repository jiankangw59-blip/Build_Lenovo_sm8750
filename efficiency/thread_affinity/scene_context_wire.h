/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SCENE_CONTEXT_WIRE_H
#define SCENE_CONTEXT_WIRE_H
#ifdef __KERNEL__
#include <linux/errno.h>
#include <linux/string.h>
#include <linux/types.h>
#else
#include <stdbool.h>
#include <errno.h>
#include <string.h>
#endif

#define VISIBLE_PACKAGES_LEN 1024
#define SCENE_PACKAGE_LEN 128
struct scene_context {
	char foreground[SCENE_PACKAGE_LEN];
	char visible[VISIBLE_PACKAGES_LEN];
	char windowed[VISIBLE_PACKAGES_LEN];
	bool display_off, game;
};

static inline bool scene_wire_char(char c)
{
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
		(c >= '0' && c <= '9') || c == '.' || c == '_';
}

static inline int scene_wire_copy(char *out, size_t capacity, const char *text, bool list)
{
	size_t length = strlen(text), token = 0, i;

	if (!length) return -EINVAL;
	if (!strcmp(text, "-")) { out[0] = 0; return 0; }
	if (length >= capacity) return -E2BIG;
	for (i = 0; i < length; i++) {
		if (list && text[i] == ',' && token) { token = 0; continue; }
		if (!scene_wire_char(text[i]) || ++token >= SCENE_PACKAGE_LEN) return -EINVAL;
	}
	if (!token) return -EINVAL;
	memcpy(out, text, length + 1);
	return 0;
}

/* Legacy foreground/visible/off[/game] and negotiated [.../game/windowed]. */
static inline int parse_scene_context(char *text, struct scene_context *out)
{
	char *fields[5], *at = text;
	unsigned int count = 1, i;
	int error;

	fields[0] = text;
	for (; *at; at++) {
		if (*at == '\n') {
			if (at[1]) return -EINVAL;
			*at = 0; break;
		}
		if (*at != '\t') continue;
		if (count == 5) return -EINVAL;
		*at = 0; fields[count++] = at + 1;
	}
	if (count < 3 || (strcmp(fields[2], "0") && strcmp(fields[2], "1")) ||
	    (count >= 4 && *fields[3] && strcmp(fields[3], "0") && strcmp(fields[3], "1")))
		return -EINVAL;
	memset(out, 0, sizeof(*out));
	for (i = 0; i < count; i++) {
		if (i == 2 || i == 3) continue;
		error = scene_wire_copy(i == 0 ? out->foreground : i == 1 ? out->visible : out->windowed,
			i == 0 ? sizeof(out->foreground) : sizeof(out->visible), fields[i], i != 0);
		if (error) return error;
	}
	out->display_off = fields[2][0] == '1';
	out->game = count >= 4 && fields[3][0] == '1' && out->foreground[0] && !out->display_off;
	return 0;
}
#endif
