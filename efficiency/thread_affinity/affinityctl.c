// SPDX-License-Identifier: GPL-2.0
#define _POSIX_C_SOURCE 200809L
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include "adaptive_watch.h"

#define DEVICE "/dev/ace6_thread_affinity"
#define MAX_BYTES (512 * 1024)
#define MAX_PROFILES 512
#define SCENE_PATH "/data/user/0/com.omarea.vtools/files/xiaocaiye_a11y_state"
#define DISPLAY_PATH "/sys/class/backlight/panel0-backlight/brightness"

static const char *const sources[] = {
	"template_rules.prop", "app_profile_rules.prop", "auto_rules.prop",
	"measured_game_rules.prop", "applist.prop",
};

static size_t append_row(char *buffer, size_t used, size_t tier, const char *selector,
			 const char *row, size_t length)
{
	size_t prefix = strlen(selector);

	if (used + prefix + length + 3 > MAX_BYTES) {
		fprintf(stderr, "Rule configuration exceeds %u bytes\n", MAX_BYTES);
		exit(1);
	}
	buffer[used++] = '0' + tier;
	buffer[used++] = '\t';
	memcpy(buffer + used, selector, prefix);
	used += prefix;
	memcpy(buffer + used, row, length);
	used += length;
	buffer[used++] = '\n';
	return used;
}

struct shared_profile {
	char *body;
	size_t used;
	bool defined;
};

static size_t append_profiles(char *buffer, size_t used, FILE *file)
{
	struct shared_profile profiles[MAX_PROFILES] = {};
	struct shared_profile *current = NULL;
	char *line = NULL;
	size_t capacity = 0, row = 0;
	ssize_t length;

	while ((length = getline(&line, &capacity, file)) >= 0) {
		char *start = line;
		row++;
		if (memchr(line, '\0', (size_t)length))
			goto invalid;
		while (length && isspace((unsigned char)line[length - 1]))
			line[--length] = '\0';
		while (*start && isspace((unsigned char)*start))
			start++;
		if (!*start || *start == '#')
			continue;
		if (!strncmp(start, "@profile ", 9) || !strncmp(start, "@use ", 5)) {
			bool definition = start[1] == 'p';
			char *digits = start + (definition ? 9 : 5), *tail;
			unsigned long id = strtoul(digits, &tail, 10);
			struct shared_profile *profile;

			if (!isdigit((unsigned char)*digits) || tail == digits ||
			    id == 0 || id >= MAX_PROFILES)
				goto invalid;
			profile = &profiles[id];
			if (definition) {
				if (*tail || profile->defined)
					goto invalid;
				profile->defined = true;
				current = profile;
				continue;
			}
			if (*tail != ' ' || !profile->body || !tail[1] || strpbrk(tail + 1, " \t"))
				goto invalid;
			current = NULL;
			for (size_t pos = 0; pos < profile->used;) {
				const char *entry = profile->body + pos;
				const char *newline = memchr(entry, '\n', profile->used - pos);
				size_t count = (size_t)(newline - entry);

				used = append_row(buffer, used, 1, tail + 1, entry, count);
				pos += count + 1;
			}
			continue;
		}
		if (!current || (*start != '{' && *start != '=') ||
		    (size_t)length + 1 > MAX_BYTES - current->used)
			goto invalid;
		char *body = realloc(current->body, current->used + (size_t)length + 1);
		if (!body) {
			perror("allocate shared profile");
			exit(1);
		}
		current->body = body;
		memcpy(body + current->used, line, (size_t)length);
		current->used += (size_t)length;
		body[current->used++] = '\n';
	}
	if (ferror(file))
		goto invalid;
	for (size_t i = 0; i < MAX_PROFILES; i++)
		free(profiles[i].body);
	free(line);
	return used;
invalid:
	fprintf(stderr, "Invalid app_profile_rules.profiles near line %zu\n", row);
	exit(1);
}

static size_t append_source(char *buffer, size_t used, const char *dir, size_t tier)
{
	char path[4096];
	char *line = NULL;
	size_t capacity = 0;
	ssize_t length;
	FILE *file;

	if (tier == 1) {
		if (snprintf(path, sizeof(path), "%s/app_profile_rules.profiles", dir) >= (int)sizeof(path)) {
			fprintf(stderr, "Configuration path too long\n");
			exit(1);
		}
		file = fopen(path, "r");
		if (file) {
			used = append_profiles(buffer, used, file);
			fclose(file);
			return used;
		}
		if (errno != ENOENT) {
			perror(path);
			exit(1);
		}
	}
	if (snprintf(path, sizeof(path), "%s/%s", dir, sources[tier]) >= (int)sizeof(path)) {
		fprintf(stderr, "Configuration path too long\n");
		exit(1);
	}
	file = fopen(path, "r");
	if (!file) {
		if (errno == ENOENT && tier != 4)
			return used;
		perror(path);
		exit(1);
	}
	while ((length = getline(&line, &capacity, file)) >= 0) {
		char *start = line;
		while (*start && isspace((unsigned char)*start))
			start++;
		if (!*start || *start == '#')
			continue;
		if (memchr(line, '\0', (size_t)length)) {
			fprintf(stderr, "NUL byte in %s\n", path);
			exit(1);
		}
		length -= start - line;
		while (length && isspace((unsigned char)start[length - 1]))
			length--;
		used = append_row(buffer, used, tier, "", start, (size_t)length);
	}
	if (ferror(file)) {
		perror(path);
		exit(1);
	}
	free(line);
	fclose(file);
	return used;
}

static int status(void)
{
	char data[768];
	int fd = open(DEVICE, O_RDONLY | O_CLOEXEC);
	ssize_t n;

	if (fd < 0) {
		perror(DEVICE);
		return 1;
	}
	n = read(fd, data, sizeof(data));
	close(fd);
	if (n < 0) {
		perror("read affinity status");
		return 1;
	}
	return fwrite(data, 1, (size_t)n, stdout) != (size_t)n;
}

int main(int argc, char **argv)
{
	bool check = argc == 3 && !strcmp(argv[1], "check");
	bool load = argc == 3 && !strcmp(argv[1], "load");
	bool adaptive = (argc == 3 || argc == 4) && !strcmp(argv[1], "adapt-load");
	bool dump = argc == 3 && !strcmp(argv[1], "dump");
	bool clear = argc == 2 && !strcmp(argv[1], "clear");
	char *buffer;
	size_t used = 0, tier;
	ssize_t n;
	int fd;

	if (argc == 2 && !strcmp(argv[1], "status"))
		return status();
	if ((argc == 2 || argc == 4) && !strcmp(argv[1], "watch"))
		return watch_context(argc == 4 ? argv[2] : SCENE_PATH, argc == 4 ? argv[3] : DISPLAY_PATH, NULL);
	if ((argc == 2 || argc == 5) && !strcmp(argv[1], "watch-games"))
		return watch_context(argc == 5 ? argv[2] : SCENE_PATH, argc == 5 ? argv[3] : DISPLAY_PATH,
				     argc == 5 ? argv[4] : "/data/user/0/com.omarea.vtools/shared_prefs/games.xml");
	if (!check && !load && !adaptive && !dump && !clear) {
		fprintf(stderr,
			"Usage: %s status | clear | {check,load,dump} RULE_DIRECTORY\n"
			"       %s adapt-load RULE_DIRECTORY [PROCESS_PATTERN]\n"
			"       %s watch [SCENE_FILE BRIGHTNESS_FILE]\n"
			"       %s watch-games [SCENE_FILE BRIGHTNESS_FILE GAMES_FILE]\n",
			argv[0], argv[0], argv[0], argv[0]);
		return 2;
	}
	buffer = malloc(MAX_BYTES);
	if (!buffer) {
		perror("allocate configuration buffer");
		return 1;
	}
	if (check) {
		memcpy(buffer, "?check\n", 7);
		used = 7;
	}
	if (adaptive) {
		const char *scope = argc == 4 ? argv[3] : "*";

		if (!*scope || strlen(scope) >= 128 || strpbrk(scope, " \t\r\n")) {
			fprintf(stderr, "Invalid adaptive process selector\n");
			free(buffer);
			return 2;
		}
		used = snprintf(buffer, MAX_BYTES, "?adapt\t%s\n", scope);
	}
	if (check || load || adaptive || dump) {
		for (tier = 0; tier < sizeof(sources) / sizeof(sources[0]); tier++)
			used = append_source(buffer, used, argv[2], tier);
	}
	if (clear)
		buffer[used++] = '\n';
	if (!used || (check && used == 7)) {
		fprintf(stderr, "No rules in configuration. Use clear to disable policy.\n");
		return 1;
	}
	if (dump) {
		int status = fwrite(buffer, 1, used, stdout) != used;
		free(buffer);
		return status;
	}
	fd = open(DEVICE, O_WRONLY | O_CLOEXEC);
	if (fd < 0) {
		perror(DEVICE);
		return 1;
	}
	n = write(fd, buffer, used);
	if (n < 0)
		perror("submit affinity configuration");
	else if ((size_t)n != used)
		fprintf(stderr, "Partial rule submission\n");
	close(fd);
	free(buffer);
	if (n < 0 || (size_t)n != used)
		return 1;
	return status();
}
