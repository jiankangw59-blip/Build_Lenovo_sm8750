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
#define STATUS_BYTES 1536
#define SCENE_PATH "/data/user/0/com.omarea.vtools/files/xiaocaiye_a11y_state"
#define DISPLAY_PATH "/sys/class/backlight/panel0-backlight/brightness"

static const char *const sources[] = {
	"template_rules.prop", "app_profile_rules.prop", "auto_rules.prop",
	"measured_game_rules.prop", "applist.prop",
};

static size_t append_bytes(char *buffer, size_t used, const char *text, size_t length)
{
	if (length > MAX_BYTES - used) {
		fprintf(stderr, "Rule configuration exceeds %u bytes\n", MAX_BYTES);
		exit(1);
	}
	memcpy(buffer + used, text, length);
	return used + length;
}

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
	bool emitted;
};

static size_t append_profiles(char *buffer, size_t used, FILE *file, bool compact)
{
	struct shared_profile profiles[MAX_PROFILES] = {};
	struct shared_profile *current = NULL;
	char *line = NULL;
	size_t capacity = 0, row = 0, initial = used;
	ssize_t length;
	bool expand = false;

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
			if (compact) {
				/* Legacy concatenation can give these selectors a different meaning. */
				if (strpbrk(tail + 1, "{}=")) {
					expand = true;
					break;
				}
				if (!profile->emitted) {
					char definition_line[32];
					int len = snprintf(definition_line, sizeof(definition_line), "@profile %lu\n", id);

					used = append_bytes(buffer, used, definition_line, (size_t)len);
					used = append_bytes(buffer, used, profile->body, profile->used);
					profile->emitted = true;
				}
				used = append_bytes(buffer, used, start, (size_t)length - (start - line));
				used = append_bytes(buffer, used, "\n", 1);
				continue;
			}
			for (size_t pos = 0; pos < profile->used;) {
				const char *entry = profile->body + pos;
				const char *newline = memchr(entry, '\n', profile->used - pos);
				size_t count = (size_t)(newline - entry);

				used = append_row(buffer, used, 1, tail + 1, entry, count);
				pos += count + 1;
			}
			continue;
		}
		if (compact)
			length -= start - line;
		if (!current || (*start != '{' && *start != '=') ||
		    (size_t)length + 1 > MAX_BYTES - current->used)
			goto invalid;
		char *body = realloc(current->body, current->used + (size_t)length + 1);
		if (!body) {
			perror("allocate shared profile");
			exit(1);
		}
		current->body = body;
		memcpy(body + current->used, compact ? start : line, (size_t)length);
		current->used += (size_t)length;
		body[current->used++] = '\n';
	}
	if (ferror(file))
		goto invalid;
	for (size_t i = 0; i < MAX_PROFILES; i++)
		free(profiles[i].body);
	free(line);
	if (expand) {
		if (fseek(file, 0, SEEK_SET) != 0) {
			perror("rewind app profile rules");
			exit(1);
		}
		return append_profiles(buffer, initial, file, false);
	}
	return used;
invalid:
	fprintf(stderr, "Invalid app_profile_rules.profiles near line %zu\n", row);
	exit(1);
}

static size_t append_source(char *buffer, size_t used, const char *dir, size_t tier,
			    bool compact)
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
			used = append_profiles(buffer, used, file, compact);
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

static ssize_t read_status(char *data, size_t size)
{
	int fd = open(DEVICE, O_RDONLY | O_CLOEXEC);
	int saved_errno;
	ssize_t n;

	if (fd < 0)
		return -1;
	do {
		n = read(fd, data, size);
	} while (n < 0 && errno == EINTR);
	saved_errno = errno;
	close(fd);
	errno = saved_errno;
	return n;
}

static bool supports_profile_wire(void)
{
	const char cap[] = "profile_wire=1";
	char data[STATUS_BYTES];
	ssize_t n = read_status(data, sizeof(data));
	const char *line = data;

	while (n > 0) {
		const char *end = memchr(line, '\n', (size_t)n);

		if (!end)
			break;
		if ((size_t)(end - line) == sizeof(cap) - 1 && !memcmp(line, cap, sizeof(cap) - 1))
			return true;
		n -= end - line + 1;
		line = end + 1;
	}
	return false;
}

static int status(void)
{
	char data[STATUS_BYTES];
	ssize_t n = read_status(data, sizeof(data));

	if (n < 0) {
		perror(DEVICE);
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
	bool dump_compact = argc == 3 && !strcmp(argv[1], "dump-compact");
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
	if (!check && !load && !adaptive && !dump && !dump_compact && !clear) {
		fprintf(stderr,
			"Usage: %s status | clear | {check,load,dump,dump-compact} RULE_DIRECTORY\n"
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
	if (check || load || adaptive || dump || dump_compact) {
		bool compact = dump_compact || (!dump && supports_profile_wire());

		for (tier = 0; tier < sizeof(sources) / sizeof(sources[0]); tier++)
			used = append_source(buffer, used, argv[2], tier, compact);
	}
	if (clear)
		buffer[used++] = '\n';
	if (!used || (check && used == 7)) {
		fprintf(stderr, "No rules in configuration. Use clear to disable policy.\n");
		return 1;
	}
	if (dump || dump_compact) {
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
	/* The write is committed even if the subsequent status read fails. */
	status();
	return 0;
}
