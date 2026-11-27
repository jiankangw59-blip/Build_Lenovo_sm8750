// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "scene_context_wire.h"
#include "adaptive_watch.c"

static void save(const char *path, const char *text)
{
	FILE *file = fopen(path, "w");
	assert(file);
	assert(fputs(text, file) >= 0);
	assert(!fclose(file));
}

int main(void)
{
	struct scene_context context, previous;
	char text[NOTICE_SIZE], last[NOTICE_SIZE] = "", path[] = "/tmp/scene-wire-XXXXXX";
	int descriptor = mkstemp(path), pipefd[2];
	ssize_t length;

	assert(descriptor >= 0); close(descriptor);
	strcpy(text, "com.app\tcom.app,com.small\t0\n");
	assert(!parse_scene_context(text, &context));
	assert(!context.game && !context.display_off && !context.windowed[0]);
	strcpy(text, "com.app\tcom.app,com.small\t0\t1\tcom.small\n");
	assert(!parse_scene_context(text, &context) && context.game);
	assert(!strcmp(context.windowed, "com.small"));
	previous = context;
	strcpy(text, "com.app\tcom.app,com.small\t0\t1\tcom.app\n");
	assert(!parse_scene_context(text, &context));
	assert(memcmp(&context, &previous, sizeof(context))); /* Window-only semantic change. */
	strcpy(text, "com.app\tcom.app\t1\t1\t-\n");
	assert(!parse_scene_context(text, &context) && context.display_off && !context.game);
	strcpy(text, "-\t-\t0\t\n");
	assert(!parse_scene_context(text, &context));
	{
		const char *bad[] = {"a\tb\t0\t0\tw\textra", "a\tb\t2", "a\tb,,c\t0",
			"a\tb\t0\t2\tw", "a\tb\t0\t0\t", "a\tb\t0\t0\tx,", "a\tb\t0\nnext"};
		unsigned int i;
		for (i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
			strcpy(text, bad[i]); assert(parse_scene_context(text, &context) == -EINVAL);
		}
	}
	assert(!pipe2(pipefd, O_NONBLOCK | O_CLOEXEC));
	save(path, "active_pkg=com.other\nscene_foreground_pkg=com.app\n"
		"visible_pkgs=com.z,com.small,com.app,com.z\nwindowed_pkgs=com.small\n");
	context_windowed_cap = true;
	assert(!submit_context(pipefd[1], path, "/no-panel-state", NULL, last));
	length = read(pipefd[0], text, sizeof(text) - 1); assert(length > 9); text[length] = 0;
	assert(!strncmp(text, "?context\t", 9));
	assert(!parse_scene_context(text + 9, &context));
	assert(!strcmp(context.foreground, "com.app"));
	assert(!strcmp(context.visible, "com.app,com.small,com.z"));
	assert(!strcmp(context.windowed, "com.small"));
	assert(!submit_context(pipefd[1], path, "/no-panel-state", NULL, last));
	assert(read(pipefd[0], text, sizeof(text)) == -1 && errno == EAGAIN);
	save(path, "scene_foreground_pkg=com.app\nvisible_pkgs=com.z,com.small,com.app\n"
		"windowed_pkgs=com.z\n");
	assert(!submit_context(pipefd[1], path, "/no-panel-state", NULL, last));
	length = read(pipefd[0], text, sizeof(text) - 1); assert(length > 0); text[length] = 0;
	assert(!parse_scene_context(text + 9, &previous));
	assert(memcmp(&previous, &context, sizeof(context)));
	/* Capability negotiation keeps old kernels' exact legacy shape. */
	last[0] = 0; context_windowed_cap = false;
	assert(!submit_context(pipefd[1], path, "/no-panel-state", NULL, last));
	length = read(pipefd[0], text, sizeof(text) - 1); assert(length > 0); text[length] = 0;
	assert(!parse_scene_context(text + 9, &context) && !context.windowed[0]);
	assert(!strcmp(text + 9, "com.app"));
	/* Oversized visible/windowed sets fail closed; never publish a partial set. */
	{
		char list[4096] = "", package[32], output[VISIBLE_SIZE];
		unsigned int i;
		for (i = 0; i < 33; i++) {
			snprintf(package, sizeof(package), "com.app%u,", i);
			strcat(list, package);
		}
		assert(visible_packages(output, list) == -1 && errno == E2BIG);
	}
	close(pipefd[0]); close(pipefd[1]); unlink(path);
	puts("scene context: legacy, windowed semantics, validation, real watcher encoding and dedup passed");
	return 0;
}
