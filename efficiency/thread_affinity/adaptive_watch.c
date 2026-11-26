// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/signalfd.h>
#include <unistd.h>

#include "adaptive_watch.h"

#define DEVICE "/dev/ace6_thread_affinity"
#define PACKAGE_SIZE 128
#define VISIBLE_SIZE 1024
#define NOTICE_SIZE (PACKAGE_SIZE + 2 * VISIBLE_SIZE + 32)
#define SNAPSHOT_EVENTS (IN_CLOSE_WRITE | IN_MOVED_TO | IN_DELETE | IN_MOVED_FROM)

struct source {
	char *directory;
	const char *name;
	int watch;
};
static bool context_windowed_cap;

static bool package_name(char dst[PACKAGE_SIZE], const char *value)
{
	size_t len = strcspn(value, ":\r\n \t");
	size_t i;

	if (!len || len >= PACKAGE_SIZE)
		return false;
	for (i = 0; i < len; i++)
		if (!isalnum((unsigned char)value[i]) && value[i] != '.' && value[i] != '_')
			return false;
	memcpy(dst, value, len);
	dst[len] = '\0';
	return true;
}

static int compare_names(const void *a, const void *b)
{
	return strcmp(a, b);
}

static int visible_packages(char out[VISIBLE_SIZE], char *value)
{
	char names[32][PACKAGE_SIZE];
	char *next;
	char *token;
	size_t count = 0, used = 0, i;

	out[0] = '\0';
	for (token = strtok_r(value, ", |\t\r\n", &next);
	     token;
	     token = strtok_r(NULL, ", |\t\r\n", &next)) {
		if (count == sizeof(names) / sizeof(names[0])) { errno = E2BIG; return -1; }
		if (!package_name(names[count], token)) { errno = EINVAL; return -1; }
		count++;
	}
	qsort(names, count, sizeof(names[0]), compare_names);
	for (i = 0; i < count; i++) {
		size_t len = strlen(names[i]);

		if (i && !strcmp(names[i], names[i - 1]))
			continue;
		if (used + (used != 0) + len >= VISIBLE_SIZE) { errno = E2BIG; return -1; }
		if (used)
			out[used++] = ',';
		memcpy(out + used, names[i], len + 1);
		used += len;
	}
	return 0;
}

static int read_scene(const char *path, char foreground[PACKAGE_SIZE], char visible[VISIBLE_SIZE],
		       char windowed[VISIBLE_SIZE])
{
	char candidates[3][PACKAGE_SIZE] = {};
	char line[4096];
	FILE *file = fopen(path, "re");
	size_t i;

	foreground[0] = '\0';
	visible[0] = '\0';
	windowed[0] = '\0';
	if (!file)
		return 0;
	while (fgets(line, sizeof(line), file)) {
		if (!strncmp(line, "scene_foreground_pkg=", 21))
			package_name(candidates[0], line + 21);
		else if (!strncmp(line, "focused_pkg=", 12))
			package_name(candidates[1], line + 12);
		else if (!strncmp(line, "active_pkg=", 11))
			package_name(candidates[2], line + 11);
		else if (!strncmp(line, "visible_pkgs=", 13)) {
			if (visible_packages(visible, line + 13)) { fclose(file); return -1; }
		} else if (!strncmp(line, "windowed_pkgs=", 14)) {
			if (visible_packages(windowed, line + 14)) { fclose(file); return -1; }
		}
	}
	if (!ferror(file)) {
		for (i = 0; i < sizeof(candidates) / sizeof(candidates[0]); i++) {
			if (candidates[i][0]) {
				strcpy(foreground, candidates[i]);
				break;
			}
		}
	} else {
		visible[0] = '\0';
		windowed[0] = '\0';
	}
	fclose(file);
	return 0;
}

static bool display_off(const char *path)
{
	FILE *file = fopen(path, "re");
	unsigned int value;
	bool off;

	if (!file)
		return false;
	off = fscanf(file, "%u", &value) == 1 && value <= 1;
	fclose(file);
	return off;
}

static bool is_scene_game(const char *path, const char *package)
{
	char line[4096], name[PACKAGE_SIZE + 8];
	bool game = false;
	FILE *file;

	if (!path || !*package || !strcmp(package, "com.oplus.games") ||
	    !strcmp(package, "com.nearme.gamecenter") || !strcmp(package, "com.xiaoji.egggame") ||
	    !strcmp(package, "com.antutu.ABenchMark") || !strncmp(package, "com.winlator", 12))
		return false;
	file = fopen(path, "re");
	if (!file)
		return false;
	snprintf(name, sizeof(name), "name=\"%s\"", package);
	while (fgets(line, sizeof(line), file)) {
		if (strstr(line, "<boolean ") && strstr(line, name) && strstr(line, "value=\"true\"")) {
			game = true;
			break;
		}
	}
	fclose(file);
	return game;
}

static int submit_context(int ctl, const char *node, const char *brightness, const char *games,
			  char last[NOTICE_SIZE])
{
	char foreground[PACKAGE_SIZE], visible[VISIBLE_SIZE], windowed[VISIBLE_SIZE], text[NOTICE_SIZE];
	bool off;
	int len;
	ssize_t wrote;

	if (read_scene(node, foreground, visible, windowed))
		return -1;
	off = display_off(brightness);
	len = snprintf(text, sizeof(text), "?context\t%s\t%s\t%d",
		       foreground[0] ? foreground : "-", visible[0] ? visible : "-",
		       off);
	if (games || context_windowed_cap)
		len += snprintf(text + len, sizeof(text) - len, "\t%d", !off && is_scene_game(games, foreground));
	if (context_windowed_cap)
		len += snprintf(text + len, sizeof(text) - len, "\t%s", windowed[0] ? windowed : "-");
	text[len++] = '\n';
	text[len] = '\0';
	if (!strcmp(text, last))
		return 0;
	wrote = write(ctl, text, (size_t)len);
	if (wrote != len) {
		if (wrote >= 0)
			errno = EIO;
		return -1;
	}
	memcpy(last, text, (size_t)len + 1);
	return 0;
}

static int refresh_context(int *ctl, const char *node, const char *brightness, const char *games,
			   char last[NOTICE_SIZE], bool *deferred)
{
	if (*ctl < 0) {
		char features[1024];
		ssize_t length;

		*ctl = open(DEVICE, O_RDWR | O_CLOEXEC);
		if (*ctl < 0)
			return -1;
		length = read(*ctl, features, sizeof(features) - 1);
		if (length < 0)
			return -1;
		features[length] = '\0';
		context_windowed_cap = strstr(features, "\ncontext_windowed=1\n") != NULL;
	}
	if (!submit_context(*ctl, node, brightness, games, last)) {
		if (*deferred)
			fprintf(stderr, "Scene context recovered\n");
		*deferred = false;
		return 0;
	}
	if (errno != E2BIG && errno != ENOMEM)
		return -1;

	if (!*deferred)
		fprintf(stderr, "Scene context deferred (%s); static rules remain active\n", strerror(errno));
	/* Drop the previous adaptive scene. The next event supplies a fresh one. */
	close(*ctl);
	*ctl = -1;
	last[0] = '\0';
	*deferred = true;
	return 0;
}

static int watch_parent(int inotify, const char *path, struct source *source)
{
	const char *slash = strrchr(path, '/');

	if (!slash || !slash[1]) {
		errno = EINVAL;
		return -1;
	}
	source->name = slash + 1;
	source->directory = slash == path ? strdup("/") : strndup(path, (size_t)(slash - path));
	if (!source->directory)
		return -1;
	source->watch = inotify_add_watch(inotify, source->directory,
					  SNAPSHOT_EVENTS | IN_DELETE_SELF | IN_MOVE_SELF);
	return source->watch < 0 ? -1 : 0;
}

static int read_events(int fd, const struct source *scene, const struct source *brightness,
		       const struct source *games, int panel_watch)
{
	char buffer[4096] __attribute__((aligned(__alignof__(struct inotify_event))));
	ssize_t size = read(fd, buffer, sizeof(buffer));
	bool changed = false;

	if (size < 0)
		return errno == EINTR || errno == EAGAIN ? 0 : -1;
	for (ssize_t offset = 0; offset < size;) {
		const struct inotify_event *event = (const void *)(buffer + offset);

		offset += sizeof(*event) + event->len;
		if ((event->wd == scene->watch || event->wd == brightness->watch ||
		     event->wd == games->watch) &&
		    (event->mask & (IN_DELETE_SELF | IN_MOVE_SELF | IN_IGNORED))) {
			errno = ENOENT;
			return -1;
		}
		if ((event->mask & IN_Q_OVERFLOW) || event->wd == panel_watch ||
		    (event->len && ((event->wd == scene->watch && !strcmp(event->name, scene->name)) ||
		     (event->wd == brightness->watch && !strcmp(event->name, brightness->name)) ||
		     (event->wd == games->watch && !strcmp(event->name, games->name)))))
			changed = true;
	}
	return changed;
}

int watch_context(const char *node, const char *brightness, const char *games)
{
	struct source scene = {}, panel = {}, game_list = { .watch = -2 };
	struct pollfd events[2];
	sigset_t signals;
	char last[NOTICE_SIZE] = "";
	bool deferred = false;
	int notification = -1, signal_fd = -1, ctl = -1, panel_watch = -2;
	int ret = 1;

	sigemptyset(&signals);
	sigaddset(&signals, SIGTERM);
	sigaddset(&signals, SIGINT);
	if (sigprocmask(SIG_BLOCK, &signals, NULL) < 0)
		goto out;
	signal_fd = signalfd(-1, &signals, SFD_CLOEXEC);
	if (signal_fd < 0)
		goto out;
	notification = inotify_init1(IN_CLOEXEC | IN_NONBLOCK);
	if (notification < 0 ||
	    watch_parent(notification, node, &scene) < 0 ||
	    watch_parent(notification, brightness, &panel) < 0 ||
	    (games && watch_parent(notification, games, &game_list) < 0))
		goto out;
	if (!strncmp(brightness, "/sys/", 5)) {
		int watch = inotify_add_watch(notification, brightness, IN_MODIFY);

		if (watch >= 0)
			panel_watch = watch;
	}
	if (refresh_context(&ctl, node, brightness, games, last, &deferred) < 0)
		goto out;
	events[0] = (struct pollfd){ .fd = notification, .events = POLLIN };
	events[1] = (struct pollfd){ .fd = signal_fd, .events = POLLIN };
	for (;;) {
		int ready = poll(events, 2, -1);

		if (ready < 0 && errno == EINTR)
			continue;
		if (ready < 0)
			goto out;
		if (events[1].revents & POLLIN) {
			ret = 0;
			break;
		}
		if (events[0].revents & (POLLERR | POLLHUP | POLLNVAL)) {
			errno = EIO;
			goto out;
		}
		if (events[0].revents & POLLIN) {
			int dirty = read_events(notification, &scene, &panel, &game_list, panel_watch);

			if (dirty < 0 || (dirty && refresh_context(&ctl, node, brightness, games, last, &deferred) < 0))
				goto out;
		}
	}
out:
	if (ret)
		perror("watch Scene context");
	if (ctl >= 0)
		close(ctl);
	if (notification >= 0)
		close(notification);
	if (signal_fd >= 0)
		close(signal_fd);
	free(scene.directory);
	free(panel.directory);
	free(game_list.directory);
	return ret;
}
