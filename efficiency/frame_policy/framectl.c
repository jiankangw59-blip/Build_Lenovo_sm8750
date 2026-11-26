// SPDX-License-Identifier: GPL-2.0
#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <unistd.h>

#include "frame_policy_uapi.h"

int main(int argc, char **argv)
{
	struct fp_config config;
	FILE *input;
	int device, result = 0;
	char status[2048];
	ssize_t count;

	if (argc < 2 || argc > 3 ||
	    (strcmp(argv[1], "status") && strcmp(argv[1], "disable") &&
	     !(argc == 3 && !strcmp(argv[1], "load")))) {
		fprintf(stderr, "Usage: %s status | disable | load CONFIG.bin\n", argv[0]);
		return 2;
	}
	device = open("/dev/frame_policy", O_RDWR | O_CLOEXEC);
	if (device < 0) {
		perror("open frame policy");
		return 1;
	}
	if (!strcmp(argv[1], "load")) {
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
		result = ioctl(device, FP_CONFIG, &config);
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
