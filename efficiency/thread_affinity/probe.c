// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <unistd.h>

static void *run_thread(void *arg)
{
	if (prctl(PR_SET_NAME, (char *)arg) < 0)
		abort();
	free(arg);
	for (;;)
		pause();
}

int main(int argc, char **argv)
{
	char *line = NULL;
	size_t capacity = 0;
	FILE *commands;
	int fd;

	if (argc != 2)
		return 2;
	fd = open(argv[1], O_RDWR | O_CLOEXEC);
	if (fd < 0) {
		perror(argv[1]);
		return 1;
	}
	commands = fdopen(fd, "r");
	if (!commands)
		return 1;
	if (prctl(PR_SET_NAME, "a6probe_base") < 0)
		return 1;
	printf("ready %d\n", getpid());
	fflush(stdout);
	while (getline(&line, &capacity, commands) != -1) {
		line[strcspn(line, "\r\n")] = '\0';
		if (!strncmp(line, "name ", 5)) {
			if (prctl(PR_SET_NAME, line + 5) < 0)
				return 1;
		} else if (!strncmp(line, "spawn ", 6)) {
			pthread_t thread;
			char *name = strdup(line + 6);
			if (!name || pthread_create(&thread, NULL, run_thread, name))
				return 1;
			pthread_detach(thread);
		} else if (!strcmp(line, "exit")) {
			break;
		}
	}
	free(line);
	fclose(commands);
	return 0;
}
