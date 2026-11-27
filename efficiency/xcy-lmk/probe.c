// SPDX-License-Identifier: GPL-2.0
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mount.h>
#include <sys/reboot.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/wait.h>
#include <unistd.h>

#include "xcy_lmk_uapi.h"

static void check(int passed, const char *name)
{
	printf("%s %s\n", passed ? "PASS" : "FAIL", name);
	if (!passed)
		exit(EXIT_FAILURE);
}

static unsigned long long self_start_ticks(void)
{
	char buffer[4096];
	FILE *stat = fopen("/proc/self/stat", "r");
	check(stat != NULL && fgets(buffer, sizeof(buffer), stat) != NULL, "read-process-stat");
	fclose(stat);
	char *last = strrchr(buffer, ')');
	check(last != NULL, "find-process-name");
	char *save;
	char *field = strtok_r(last + 1, " ", &save);
	for (int i = 3; field && i < 22; i++)
		field = strtok_r(NULL, " ", &save);
	return field ? strtoull(field, NULL, 10) : 0;
}

static void run(void)
{
	FILE *misc = fopen("/proc/misc", "r");
	check(misc != NULL, "read-misc-devices");
	int minor, found = 0;
	char name[128];
	while (fscanf(misc, "%d %127s", &minor, name) == 2) {
		if (!strcmp(name, "xcy_lmk")) {
			found = 1;
			break;
		}
	}
	fclose(misc);
	check(found && mknod("/dev/xcy_lmk", S_IFCHR | 0600, makedev(10, minor)) == 0, "register-device");
	int fd = open("/dev/xcy_lmk", O_RDONLY | O_CLOEXEC);
	check(fd >= 0, "open-device");

	struct xcy_lmk_candidate candidates[] = {
		{ .pss_kib = 1000, .flags = XCY_LMK_CANDIDATE_ELIGIBLE | XCY_LMK_CANDIDATE_RECENT },
		{ .pss_kib = 599, .background_age_ms = 240000, .flags = XCY_LMK_CANDIDATE_ELIGIBLE },
		{ .pss_kib = 10000 },
	};
	struct xcy_lmk_batch batch = {
		.entries = (uintptr_t)candidates, .version = XCY_LMK_ABI_VERSION, .count = 3,
	};
	check(ioctl(fd, XCY_LMK_RANK, &batch) == 0 && batch.selected == XCY_LMK_NO_SELECTION,
	      "normal-recent-and-ineligible-protection");
	candidates[1].pss_kib = 600;
	check(ioctl(fd, XCY_LMK_RANK, &batch) == 0 && batch.selected == 1, "normal-minimum-pool");
	batch.flags = XCY_LMK_RANK_URGENT;
	candidates[1].pss_kib = 756;
	check(ioctl(fd, XCY_LMK_RANK, &batch) == 0 && batch.selected == 1, "urgent-window-inclusive");
	candidates[1].pss_kib--;
	check(ioctl(fd, XCY_LMK_RANK, &batch) == 0 && batch.selected == 0, "urgent-recent-allowed");
	batch.version++;
	check(ioctl(fd, XCY_LMK_RANK, &batch) == -1 && errno == EINVAL, "reject-invalid-abi");
	batch.version--;
	batch.count = XCY_LMK_MAX_ENTRIES + 1;
	check(ioctl(fd, XCY_LMK_RANK, &batch) == -1 && errno == EINVAL, "reject-oversized-batch");

	unsigned long long start = self_start_ticks();
	check(start > 0, "read-start-ticks");
	struct xcy_lmk_task tasks[] = {
		{ .pid = getpid(), .uid = getuid(), .start_ticks = start },
		{ .pid = getpid(), .uid = getuid(), .start_ticks = start + 1 },
		{ .pid = getpid(), .uid = getuid() + 1U, .start_ticks = start },
	};
	batch = (struct xcy_lmk_batch){ .entries = (uintptr_t)tasks,
		.version = XCY_LMK_ABI_VERSION, .count = 3 };
	check(ioctl(fd, XCY_LMK_READ_TASKS, &batch) == 0, "read-task-batch");
	check(tasks[0].status == 0 && tasks[0].rss_kib > 0, "valid-task-identity");
	check(tasks[1].status == -ESTALE && tasks[2].status == -ESTALE, "reject-stale-task-identities");
	check(setuid(65534) == 0, "drop-capability");
	check(ioctl(fd, XCY_LMK_READ_TASKS, &batch) == -1 && errno == EPERM,
	      "reject-ioctl-without-capability-even-on-inherited-fd");
	close(fd);
}

int main(void)
{
	setvbuf(stdout, NULL, _IONBF, 0);
	check(mount("proc", "/proc", "proc", 0, NULL) == 0, "mount-proc");
	pid_t pid = fork();
	check(pid >= 0, "fork-probe");
	if (pid == 0) {
		run();
		return 0;
	}
	int status;
	if (waitpid(pid, &status, 0) == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0)
		puts("XCY_LMK_RUNTIME_PASSED");
	reboot(RB_POWER_OFF);
	return 1;
}
