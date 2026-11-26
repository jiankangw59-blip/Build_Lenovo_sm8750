// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/vfs.h>
#include <unistd.h>
#include <linux/magic.h>

#include "frame_policy_uapi.h"

/* kernfs requires a write mode bit even for a privileged open. Scene can lock it. */
static int open_gpu_control(void)
{
	const char *path = "/sys/class/kgsl/kgsl-3d0/devfreq/mod_percent";
	struct stat before, after;
	struct statfs filesystem;
	char pinned[64];
	mode_t original, temporary;
	int pin, device, saved;

	device = open(path, O_RDWR | O_CLOEXEC);
	if (device >= 0 || errno != EACCES)
		return device;
	pin = open(path, O_PATH | O_NOFOLLOW | O_CLOEXEC);
	if (pin < 0)
		return -1;
	if (fstat(pin, &before) || fstatfs(pin, &filesystem) ||
	    !S_ISREG(before.st_mode) || filesystem.f_type != SYSFS_MAGIC) {
		close(pin);
		errno = EACCES;
		return -1;
	}
	original = before.st_mode & 07777;
	temporary = original | S_IRUSR | S_IWUSR;
	if (temporary == original) {
		close(pin); /* A different denial must not relax permissions. */
		errno = EACCES;
		return -1;
	}
	snprintf(pinned, sizeof(pinned), "/proc/self/fd/%d", pin);
	if (chmod(pinned, temporary)) {
		saved = errno;
		close(pin);
		errno = saved;
		return -1;
	}
	device = open(pinned, O_RDWR | O_CLOEXEC);
	saved = errno;
	/* Restore only our own mode; leave a concurrent owner's different mode intact. */
	if (fstat(pin, &after) ||
	    ((after.st_mode & 07777) == temporary && chmod(pinned, original))) {
		saved = errno;
		if (device >= 0)
			close(device);
		device = -1;
		fprintf(stderr, "Could not restore GPU control permissions\n");
	}
	close(pin);
	errno = saved;
	return device;
}

int main(int argc, char **argv)
{
	struct fp_config config;
	FILE *input;
	int device, gpu = -1, result = 0;
	char status[2048];
	ssize_t count;

	if (argc < 2 || argc > 3 ||
	    (strcmp(argv[1], "status") && strcmp(argv[1], "disable") &&
	     !(argc == 3 && (!strcmp(argv[1], "load") || !strcmp(argv[1], "load-rules"))))) {
		fprintf(stderr, "Usage: %s status | disable | load CONFIG.bin | load-rules RULES.bin\n", argv[0]);
		return 2;
	}
	device = open("/dev/frame_policy", O_RDWR | O_CLOEXEC);
	if (device < 0) {
		perror("open frame policy");
		return 1;
	}
	if (!strcmp(argv[1], "load-rules")) {
		struct fp_game_rules *rules = malloc(sizeof(*rules));

		if (!rules) {
			close(device);
			return 1;
		}
		input = fopen(argv[2], "rb");
		if (!input || fread(rules, sizeof(*rules), 1, input) != 1 ||
		    fgetc(input) != EOF || ferror(input) || rules->abi != FP_GAME_RULES_ABI) {
			fprintf(stderr, "Game frequency rules have an invalid size or version\n");
			if (input)
				fclose(input);
			free(rules);
			close(device);
			return 1;
		}
		fclose(input);
		result = ioctl(device, FP_GAME_RULES, rules);
		free(rules);
	} else if (!strcmp(argv[1], "load")) {
		input = fopen(argv[2], "rb");
		if (!input) {
			perror("open frame configuration");
			close(device);
			return 1;
		}
		if (fread(&config, sizeof(config), 1, input) != 1 || fgetc(input) != EOF ||
		    ferror(input)) {
			fprintf(stderr, "Frame configuration has an invalid size\n");
			fclose(input);
			close(device);
			return 1;
		}
		fclose(input);
		if (config.abi != FP_ABI) {
			fprintf(stderr, "Frame configuration ABI does not match this client\n");
			close(device);
			return 1;
		}
		if (config.flags & FP_GPU) {
			gpu = open_gpu_control();
			if (gpu < 0) {
				perror("open GPU governor control");
				close(device);
				return 1;
			}
		}
		config.gpu_percent_fd = gpu;
		result = ioctl(device, FP_CONFIG, &config);
		if (gpu >= 0)
			close(gpu);
	} else if (!strcmp(argv[1], "disable")) {
		result = ioctl(device, FP_DISABLE);
	}
	if (result < 0) {
		perror("update frame policy");
		close(device);
		return 1;
	}
	count = read(device, status, sizeof(status));
	if (count < 0) {
		perror("read frame policy");
		result = 1;
	} else if (fwrite(status, 1, (size_t)count, stdout) != (size_t)count) {
		result = 1;
	}
	close(device);
	return result;
}
