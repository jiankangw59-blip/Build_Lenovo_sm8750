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
#include <poll.h>
#include <signal.h>
#include <sys/syscall.h>
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

static int write_setting_file(int directory, const char *name, const void *bytes, size_t length)
{
	const char *cursor = bytes;
	int file = openat(directory, name, O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC | O_NOFOLLOW, 0600);

	if (file < 0)
		return -1;
	while (length) {
		ssize_t written = write(file, cursor, length);

		if (written < 0 && errno == EINTR)
			continue;
		if (written <= 0) {
			close(file);
			return -1;
		}
		cursor += written;
		length -= written;
	}
	if (fsync(file)) {
		close(file);
		return -1;
	}
	return close(file);
}

static int unpack_settings(const char *path)
{
	__u32 header[4], rules_abi;
	unsigned char *payload;
	struct fp_config config;
	size_t length;
	int directory, result;

	if (fread(header, sizeof(header), 1, stdin) != 1 || !header[0] || header[0] > 49152 ||
	    header[1] != sizeof(config) || header[2] != sizeof(struct fp_game_rules) || header[3] > 1) {
		fprintf(stderr, "Invalid settings envelope\n");
		return 1;
	}
	length = header[0] + header[1] + header[2];
	payload = malloc(length);
	if (!payload)
		return 1;
	if (fread(payload, length, 1, stdin) != 1 || fgetc(stdin) != EOF || ferror(stdin) ||
	    memchr(payload, 0, header[0])) {
		free(payload);
		return 1;
	}
	memcpy(&config, payload + header[0], sizeof(config));
	/* JSON need not end at an aligned address. */
	memcpy(&rules_abi, payload + header[0] + header[1], sizeof(rules_abi));
	if (config.abi != FP_ABI || rules_abi != FP_GAME_RULES_ABI) {
		free(payload);
		return 1;
	}
	directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory < 0) {
		free(payload);
		return 1;
	}
	result = write_setting_file(directory, "settings.json", payload, header[0]) ||
		write_setting_file(directory, "config.bin", payload + header[0], header[1]) ||
		write_setting_file(directory, "games.bin", payload + header[0] + header[1], header[2]) ||
		write_setting_file(directory, "enabled", header[3] ? "1\n" : "0\n", 2);
	close(directory);
	free(payload);
	return result ? 1 : 0;
}

static int suspend_legacy(void)
{
	const char *expected = "/data/adb/modules/fas_rs/fas-eventd";
	char path[64], executable[512];
	FILE *file;
	long pid;
	ssize_t length;
	int fd, result;
	struct pollfd stopped;

	if (!access("/data/adb/fas-rs-events/cleanup-unconfirmed", F_OK)) {
		fprintf(stderr, "Legacy FAS has unconfirmed cleanup\n");
		return 1;
	}
	file = fopen("/data/adb/fas-rs-events/run/pid", "re");
	if (!file) {
		if (errno != ENOENT)
			return 1;
		puts("legacy_was_running=0");
		return 0;
	}
	result = fscanf(file, "%ld", &pid);
	fclose(file);
	if (result != 1 || pid <= 1 || pid > 4194304)
		return 1;
	snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
	length = readlink(path, executable, sizeof(executable) - 1);
	if (length < 0 && errno == ENOENT) {
		puts("legacy_was_running=0");
		return 0;
	}
	if (length <= 0)
		return 1;
	executable[length] = 0;
	if (strcmp(executable, expected)) {
		puts("legacy_was_running=0");
		return 0;
	}
	fd = syscall(SYS_pidfd_open, (pid_t)pid, 0);
	if (fd < 0)
		return 1;
	/* The PID could have been recycled before pidfd_open(). */
	length = readlink(path, executable, sizeof(executable) - 1);
	if (length <= 0 || (executable[length] = 0, strcmp(executable, expected))) {
		close(fd);
		puts("legacy_was_running=0");
		return 0;
	}
	if (syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0)) {
		int disappeared = errno == ESRCH;
		close(fd);
		if (disappeared)
			puts("legacy_was_running=0");
		return disappeared ? 0 : 1;
	}
	stopped = (struct pollfd){ .fd = fd, .events = POLLIN };
	do { result = poll(&stopped, 1, 20000); } while (result < 0 && errno == EINTR);
	close(fd);
	if (result <= 0 || !(stopped.revents & POLLIN)) {
		fprintf(stderr, "Legacy FAS cleanup did not finish; native control remains unchanged\n");
		return 1;
	}
	puts("legacy_was_running=1");
	return 0;
}

int main(int argc, char **argv)
{
	struct fp_config config;
	FILE *input;
	int device, gpu = -1, result = 0;
	char status[2048];
	ssize_t count;

	if (argc == 3 && !strcmp(argv[1], "unpack-settings"))
		return unpack_settings(argv[2]);
	if (argc == 2 && !strcmp(argv[1], "suspend-legacy"))
		return suspend_legacy();
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
