// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <time.h>
#include <unistd.h>

struct probe_thread {
	int duty;
	char name[16];
};

static long long clock_ns(clockid_t clock)
{
	struct timespec now;

	if (clock_gettime(clock, &now) < 0)
		abort();
	return (long long)now.tv_sec * 1000000000 + now.tv_nsec;
}

static void *run_thread(void *arg)
{
	struct probe_thread *thread = arg;
	int duty = thread->duty;

	if (prctl(PR_SET_NAME, thread->name) < 0)
		abort();
	free(thread);
	for (;;) {
		long long until, work;
		struct timespec sleep_at;

		if (!duty) {
			pause();
			continue;
		}
		until = clock_ns(CLOCK_MONOTONIC) + 100000000;
		work = clock_ns(CLOCK_THREAD_CPUTIME_ID) + duty * 1000000;
		while (clock_ns(CLOCK_THREAD_CPUTIME_ID) < work && clock_ns(CLOCK_MONOTONIC) < until)
			;
		sleep_at = (struct timespec){ .tv_sec = until / 1000000000, .tv_nsec = until % 1000000000 };
		clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &sleep_at, NULL);
	}
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
		} else if (!strncmp(line, "spawn ", 6) || !strncmp(line, "burn ", 5)) {
			pthread_t thread;
			struct probe_thread *probe = calloc(1, sizeof(*probe));

			if (!probe)
				return 1;
			if (!strncmp(line, "spawn ", 6))
				snprintf(probe->name, sizeof(probe->name), "%s", line + 6);
			else if (sscanf(line + 5, "%d %15s", &probe->duty, probe->name) != 2 ||
				 probe->duty < 1 || probe->duty > 100)
				return 1;
			if (pthread_create(&thread, NULL, run_thread, probe))
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
