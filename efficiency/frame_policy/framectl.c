// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include <errno.h>
#include <ctype.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/vfs.h>
#include <poll.h>
#include <signal.h>
#include <stdint.h>
#include <limits.h>
#include <sys/syscall.h>
#include <unistd.h>
#include <linux/magic.h>

#include "frame_policy_uapi.h"
#include "scene_policy_uapi.h"
#include "fas_control.h"
#include "scene_aux_policy.h"

#ifndef FRAME_POLICY_DEVICE
#define FRAME_POLICY_DEVICE "/dev/frame_policy"
#endif
#ifndef FRAME_GPU_ROOT
#define FRAME_GPU_ROOT "/sys/class/kgsl/kgsl-3d0"
#endif
_Static_assert(sizeof(struct fp_fas_tuning) == 80, "FAS tuning ABI must be 80 bytes");
_Static_assert(sizeof(struct fp_scene_aux_table) <= 65536, "Scene aux body bound");
_Static_assert(sizeof(struct fp_scene_aux_request) == 24, "Scene aux request ABI size");

static void empty_aux(struct fp_scene_aux_table *table)
{
	unsigned int i, j;
	memset(table, 0, sizeof(*table));
	table->abi = FP_SCENE_AUX_ABI;
	for (i = 0; i < FP_MAX_CPUS; i++)
		for (j = 0; j < FP_SCENE_AUX_KNOBS; j++)
			table->fds[i][j] = -1;
}

static int aux_capability(unsigned long long *epoch, const struct fp_scene_aux_table *table)
{
	FILE *file = fopen(FRAME_POLICY_DEVICE, "re");
	char line[2048];
	int found = 0;
	bool invalid = false;
	unsigned int seen = 0, cpu, profile;
	unsigned long long values[3] = {0};
	static const char *const keys[] = {"aux_abi=", "aux_table_bytes=", "aux_feedback_supported="};
	bool feedback = table->options.valid_mask != 0;
	*epoch = 0;
	if (!file)
		return -1;
	while (fgets(line, sizeof(line), file)) {
		unsigned int index;
		if (!strncmp(line, "features=", 9)) {
			char *save, *token;
			for (token = strtok_r(line + 9, ",\r\n", &save); token; token = strtok_r(NULL, ",\r\n", &save))
				if (!strcmp(token, "scene_aux")) found = 1;
		} else if (!strncmp(line, "scene_table_epoch=", 18)) {
			char *end;
			if (!isdigit((unsigned char)line[18])) { invalid = true; continue; }
			errno = 0;
			*epoch = strtoull(line + 18, &end, 10);
			while (isspace((unsigned char)*end)) end++;
			if (errno || *end) invalid = true;
		}
		for (index = 0; index < 3; index++) {
			size_t length = strlen(keys[index]);
			char *end;
			if (strncmp(line, keys[index], length)) continue;
			if (seen & (1U << index)) { invalid = true; continue; }
			seen |= 1U << index;
			if (!isdigit((unsigned char)line[length])) { invalid = true; continue; }
			errno = 0;
			values[index] = strtoull(line + length, &end, 10);
			while (isspace((unsigned char)*end)) end++;
			if (errno || *end) invalid = true;
		}
	}
	if (ferror(file)) found = -1;
	fclose(file);
	for (profile = 0; profile < table->profile_count; profile++)
		for (cpu = 0; cpu < table->cpu_count; cpu++)
			feedback |= table->profiles[profile].cpus[cpu].feedback_mask != 0;
	if (found > 0 && ((seen & 3) != 3 || values[0] != FP_SCENE_AUX_ABI || values[1] != sizeof(*table) ||
	    (table->profile_count && feedback && (!(seen & 4) || values[2] != 1)))) {
		fprintf(stderr, "Scene aux kernel ABI/body size or full feedback support is unavailable\n");
		invalid = true;
	}
	return invalid ? -1 : found;
}

static int aux_resources(struct fp_scene_aux_table *table)
{
	static const char *const names[] = {"up_rate_limit_us", "down_rate_limit_us", "hispeed_freq", "boost", "enable", "min_cpus", "max_cpus"};
	unsigned int cpu, knob;
	for (cpu = 0; cpu < table->cpu_count; cpu++) {
		u32 required = fp_scene_aux_required(table, cpu);
		for (knob = 0; knob < FP_SCENE_AUX_KNOBS; knob++) {
			struct stat metadata;
			struct statfs filesystem;
			char path[192], value[64], *end;
			ssize_t length;
			int fd;
			if (!(required & fp_scene_aux_knob_bit(knob))) continue;
			if (knob < FP_AUX_KNOB_CORE_ENABLE)
				snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpufreq/policy%u/walt/%s", table->cpu_ids[cpu], names[knob]);
			else
				snprintf(path, sizeof(path), "/sys/devices/system/cpu/cpu%u/core_ctl/%s", table->cpu_ids[cpu], names[knob]);
			fd = open(path, O_RDWR | O_CLOEXEC | O_NOFOLLOW);
			if (fd < 0) { perror(path); return -1; }
			table->fds[cpu][knob] = fd;
			if (fstat(fd, &metadata) || fstatfs(fd, &filesystem) || !S_ISREG(metadata.st_mode) || filesystem.f_type != SYSFS_MAGIC) {
				fprintf(stderr, "Aux resource is not the expected sysfs file: %s\n", path); return -1;
			}
			length = pread(fd, value, sizeof(value) - 1, 0);
			if (length <= 0 || length == (ssize_t)sizeof(value) - 1) {
				fprintf(stderr, "Cannot read an aux scalar: %s\n", path); return -1;
			}
			value[length] = 0;
			errno = 0;
			(void)strtoll(value, &end, 10);
			if (errno || end == value) { fprintf(stderr, "Invalid aux scalar: %s\n", path); return -1; }
			while (isspace((unsigned char)*end)) end++;
			if (*end) { fprintf(stderr, "Invalid aux scalar: %s\n", path); return -1; }
		}
	}
	return 0;
}

/* Read-only permission/resource preflight for the exact original KGSL subset.
 * Runtime ownership remains in the kernel; no GPU FD enters the ABI.
 */
static int gpu_aux_scalar(int directory, dev_t device, const char *name, int flags, unsigned int *value)
{
	struct stat metadata;
	struct statfs filesystem;
	char text[64], *end;
	unsigned long long parsed;
	ssize_t length;
	int file = openat(directory, name, flags | O_CLOEXEC | O_NOFOLLOW), result = -1;
	if (file < 0) { perror(name); return -1; }
	if (fstat(file, &metadata) || fstatfs(file, &filesystem) || !S_ISREG(metadata.st_mode) ||
	    metadata.st_dev != device || filesystem.f_type != SYSFS_MAGIC) goto out;
	length = pread(file, text, sizeof(text) - 1, 0);
	if (length <= 0 || length == (ssize_t)sizeof(text) - 1) goto out;
	text[length] = 0;
	if (!isdigit((unsigned char)text[0])) goto out;
	errno = 0;
	parsed = strtoull(text, &end, 10);
	while (isspace((unsigned char)*end)) end++;
	if (errno || *end || parsed > UINT_MAX) goto out;
	*value = (unsigned int)parsed;
	result = 0;
out:
	close(file);
	if (result) fprintf(stderr, "Invalid or unreadable KGSL auxiliary scalar: %s\n", name);
	return result;
}

static int gpu_aux_resources(void)
{
	struct stat metadata;
	struct statfs filesystem;
	unsigned int count = 0, level = 0;
	int directory = open(FRAME_GPU_ROOT, O_RDONLY | O_DIRECTORY | O_CLOEXEC), devfreq = -1, result = -1;
	if (directory < 0) { perror(FRAME_GPU_ROOT); return -1; }
	if (fstat(directory, &metadata) || fstatfs(directory, &filesystem) || !S_ISDIR(metadata.st_mode) ||
	    filesystem.f_type != SYSFS_MAGIC) goto out;
	if (gpu_aux_scalar(directory, metadata.st_dev, "num_pwrlevels", O_RDONLY, &count) || !count ||
	    gpu_aux_scalar(directory, metadata.st_dev, "min_pwrlevel", O_RDWR, &level) || level >= count) goto out;
	/* Original stat follows this class symlink. ENOENT means no-default gate. */
	devfreq = openat(directory, "devfreq", O_RDONLY | O_DIRECTORY | O_CLOEXEC);
	if (devfreq < 0) {
		if (errno != ENOENT) goto out;
	} else if (fstatfs(devfreq, &filesystem) || filesystem.f_type != SYSFS_MAGIC ||
	           gpu_aux_scalar(directory, metadata.st_dev, "default_pwrlevel", O_RDWR, &level) || level >= count) {
		goto out;
	}
	result = 0;
out:
	if (devfreq >= 0) close(devfreq);
	close(directory);
	if (result) fprintf(stderr, "KGSL auxiliary resource preflight failed\n");
	return result;
}

static int handle_aux(const char *path, bool load)
{
	struct fp_scene_aux_table *table = malloc(sizeof(*table));
	struct fp_scene_aux_request request = {.abi = FP_SCENE_AUX_ABI};
	unsigned long long epoch;
	unsigned int cpu, knob;
	FILE *file;
	int capability, device = -1, result = 1;
	if (!table) return 1;
	empty_aux(table);
	file = fopen(path, "rb");
	if (!file || fread(table, sizeof(*table), 1, file) != 1 || fgetc(file) != EOF || ferror(file) || !fp_scene_aux_table_valid(table, false)) {
		if (file) fclose(file);
		fprintf(stderr, "Invalid exact %zu-byte Scene aux disk table\n", sizeof(*table));
		free(table); return 1;
	}
	fclose(file);
	capability = aux_capability(&epoch, table);
	if (capability < 0 || (table->profile_count && !capability)) {
		fprintf(stderr, "Current kernel does not advertise Scene aux capability\n"); goto out;
	}
	if (!capability) { result = 0; goto out; } /* An old kernel has no aux to clear. */
	if (aux_resources(table)) goto out;
	if (table->options.gpu_lock && gpu_aux_resources()) goto out;
	if (!load) { result = 0; goto out; }
	if (table->profile_count && !epoch) { fprintf(stderr, "Scene table epoch is unavailable\n"); goto out; }
	table->expected_scene_epoch = table->profile_count ? epoch : 0;
	if (!fp_scene_aux_table_valid(table, true)) goto out;
	device = open(FRAME_POLICY_DEVICE, O_RDWR | O_CLOEXEC);
	request.length = sizeof(*table);
	request.user_ptr = (uintptr_t)table;
	if (device < 0 || ioctl(device, FP_SCENE_AUX, &request) < 0) { perror("load Scene aux"); goto out; }
	result = 0;
out:
	if (device >= 0) close(device);
	for (cpu = 0; cpu < FP_MAX_CPUS; cpu++)
		for (knob = 0; knob < FP_SCENE_AUX_KNOBS; knob++)
			if (table->fds[cpu][knob] >= 0) close(table->fds[cpu][knob]);
	free(table);
	return result;
}

static int read_fas_tuning(const char *path, struct fp_fas_tuning *tuning)
{
	FILE *input = fopen(path, "rb");
	int valid = input && fread(tuning, sizeof(*tuning), 1, input) == 1 &&
		fgetc(input) == EOF && !ferror(input) && fp_fas_tuning_valid(tuning);

	if (input)
		fclose(input);
	if (!valid) {
		fprintf(stderr, "FAS tuning must be a valid, exact 80-byte ABI1 configuration\n");
		return -1;
	}
	return 0;
}

/* Read-only capability gate; never probe by changing a live configuration. */
static int require_fas_tuning(void)
{
	FILE *input = fopen(FRAME_POLICY_DEVICE, "re");
	char line[2048];
	int found = 0;

	if (!input) {
		perror("read frame policy capabilities");
		return -1;
	}
	while (fgets(line, sizeof(line), input)) {
		char *save, *token;

		if (strncmp(line, "features=", 9))
			continue;
		for (token = strtok_r(line + 9, ",\r\n", &save); token;
		     token = strtok_r(NULL, ",\r\n", &save))
			if (!strcmp(token, "fas_tuning"))
				found = 1;
	}
	if (ferror(input))
		found = 0;
	fclose(input);
	if (!found)
		fprintf(stderr, "Current kernel does not advertise FAS tuning capability\n");
	return found ? 0 : -1;
}

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
	__u32 header[10] = {0}, rules_abi, scene_abi, enabled, scene_required, aux_required = 0;
	unsigned char *payload;
	struct fp_scene_aux_table *aux;
	struct fp_config config;
	struct fp_fas_tuning fas = { .abi = FP_FAS_ABI, .mode = 1, .margin_base_mfps = 1000 };
	size_t length, words, fas_size, aux_size;
	int directory, result;

	if (fread(header, sizeof(header[0]), 1, stdin) != 1 ||
	    (header[0] != 0x46505331U && header[0] != 0x46505332U && header[0] != 0x46505333U)) {
		fprintf(stderr, "Invalid settings envelope version\n");
		return 1;
	}
	words = header[0] == 0x46505331U ? 7 : header[0] == 0x46505332U ? 8 : 10;
	if (fread(header + 1, sizeof(header[0]), words - 1, stdin) != words - 1) {
		fprintf(stderr, "Truncated settings envelope\n");
		return 1;
	}
	fas_size = words >= 8 ? header[5] : 0;
	aux_size = words == 10 ? header[6] : 0;
	enabled = header[words == 10 ? 7 : words - 2];
	scene_required = header[words == 10 ? 8 : words - 1];
	if (words == 10) aux_required = header[9];
	if (
	    !header[1] || header[1] > 49152 || header[2] != sizeof(config) ||
	    header[3] != sizeof(struct fp_game_rules) || header[4] != sizeof(struct fp_scene_table) ||
	    (words >= 8 && fas_size != sizeof(fas)) || (words == 10 && aux_size != sizeof(*aux)) ||
	    enabled > 1 || scene_required > 1 || aux_required > 1) {
		fprintf(stderr, "Invalid settings envelope\n");
		return 1;
	}
	length = header[1] + header[2] + header[3] + header[4] + fas_size + aux_size;
	payload = malloc(length);
	aux = malloc(sizeof(*aux));
	if (!payload || !aux) {
		free(payload); free(aux);
		return 1;
	}
	empty_aux(aux);
	if (fread(payload, length, 1, stdin) != 1 || fgetc(stdin) != EOF || ferror(stdin) ||
	    memchr(payload, 0, header[1])) {
		free(payload);
		free(aux);
		return 1;
	}
	memcpy(&config, payload + header[1], sizeof(config));
	/* JSON need not end at an aligned address. */
	memcpy(&rules_abi, payload + header[1] + header[2], sizeof(rules_abi));
	memcpy(&scene_abi, payload + header[1] + header[2] + header[3], sizeof(scene_abi));
	if (fas_size)
		memcpy(&fas, payload + header[1] + header[2] + header[3] + header[4], sizeof(fas));
	if (aux_size)
		memcpy(aux, payload + header[1] + header[2] + header[3] + header[4] + fas_size, sizeof(*aux));
	if (config.abi != FP_ABI || rules_abi != FP_GAME_RULES_ABI || scene_abi != FP_SCENE_ABI ||
	    !fp_fas_tuning_valid(&fas) || !fp_scene_aux_table_valid(aux, false) ||
	    (aux_required != !!aux->profile_count)) {
		free(payload);
		free(aux);
		return 1;
	}
	directory = open(path, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
	if (directory < 0) {
		free(payload);
		free(aux);
		return 1;
	}
	result = write_setting_file(directory, "settings.json", payload, header[1]) ||
		write_setting_file(directory, "config.bin", payload + header[1], header[2]) ||
		write_setting_file(directory, "games.bin", payload + header[1] + header[2], header[3]) ||
		write_setting_file(directory, "scene.bin", payload + header[1] + header[2] + header[3], header[4]) ||
		write_setting_file(directory, "fas.bin", &fas, sizeof(fas)) ||
		write_setting_file(directory, "enabled", enabled ? "1\n" : "0\n", 2) ||
		write_setting_file(directory, "scene-required", scene_required ? "1\n" : "0\n", 2) ||
		write_setting_file(directory, "aux.bin", aux, sizeof(*aux)) ||
		write_setting_file(directory, "aux-required", aux_required ? "1\n" : "0\n", 2);
	close(directory);
	free(payload);
	free(aux);
	return result ? 1 : 0;
}

#ifndef LEGACY_STATE
#define LEGACY_STATE "/data/adb/frame-policy"
#endif
#ifndef LEGACY_EVENTS
#define LEGACY_EVENTS "/data/adb/fas-rs-events"
#endif
#ifndef LEGACY_EXECUTABLE
#define LEGACY_EXECUTABLE "/data/adb/modules/fas_rs/fas-eventd"
#endif
#define LEGACY_PENDING "legacy-stop-pending"

struct legacy_handoff {
	char boot[40];
	long pid;
	unsigned long long start_time;
};

static int legacy_boot(char boot[40])
{
	FILE *file = fopen("/proc/sys/kernel/random/boot_id", "re");
	int result;

	if (!file)
		return -1;
	result = fscanf(file, "%39s", boot);
	fclose(file);
	return result == 1 && strlen(boot) == 36 ? 0 : -1;
}

static int legacy_start_time(long pid, unsigned long long *start)
{
	char path[64], buffer[4096], *end, *token, *save;
	unsigned int field;
	FILE *file;

	snprintf(path, sizeof(path), "/proc/%ld/stat", pid);
	file = fopen(path, "re");
	if (!file)
		return errno == ENOENT ? 0 : -1;
	if (!fgets(buffer, sizeof(buffer), file)) {
		fclose(file);
		return -1;
	}
	fclose(file);
	end = strrchr(buffer, ')');
	if (!end)
		return -1;
	for (field = 3, token = strtok_r(end + 1, " \n", &save); token;
	     field++, token = strtok_r(NULL, " \n", &save)) {
		if (field == 22) {
			errno = 0;
			*start = strtoull(token, &end, 10);
			return !errno && !*end && *start ? 1 : -1;
		}
	}
	return -1;
}

static int legacy_state_directory(void)
{
	struct stat state;
	int directory = open(LEGACY_STATE, O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);

	if (directory < 0)
		return -1;
	if (fstat(directory, &state) || state.st_uid != geteuid() || (state.st_mode & 0077)) {
		close(directory);
		errno = EACCES;
		return -1;
	}
	return directory;
}

/* 1 = valid record, 0 = absent, -1 = unreadable/invalid. No file is modified. */
static int legacy_read_handoff(struct legacy_handoff *handoff)
{
	char buffer[256], tag[32], extra;
	struct stat state;
	ssize_t length;
	int directory = legacy_state_directory(), file;

	if (directory < 0)
		return errno == ENOENT ? 0 : -1;
	file = openat(directory, LEGACY_PENDING, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	close(directory);
	if (file < 0)
		return errno == ENOENT ? 0 : -1;
	if (fstat(file, &state) || !S_ISREG(state.st_mode) || state.st_uid != geteuid() ||
	    state.st_size <= 0 || state.st_size >= (off_t)sizeof(buffer)) {
		close(file);
		return -1;
	}
	length = read(file, buffer, sizeof(buffer) - 1);
	close(file);
	if (length <= 0)
		return -1;
	buffer[length] = 0;
	if (sscanf(buffer, "%31s %39s %ld %llu %c", tag, handoff->boot,
		   &handoff->pid, &handoff->start_time, &extra) != 4 ||
	    strcmp(tag, "FP_LEGACY_STOP_V1") || strlen(handoff->boot) != 36 ||
	    handoff->pid <= 1 || handoff->pid > 4194304 || !handoff->start_time)
		return -1;
	return 1;
}

/* Publish a complete, fsynced record before SIGTERM. Never overwrite a record. */
static int legacy_record_handoff(const struct legacy_handoff *handoff)
{
	char temporary[80], buffer[160];
	int directory = legacy_state_directory(), file, length, result = -1;

	if (directory < 0)
		return -1;
	snprintf(temporary, sizeof(temporary), ".legacy-stop-pending.%ld", (long)getpid());
	length = snprintf(buffer, sizeof(buffer), "FP_LEGACY_STOP_V1 %s %ld %llu\n",
			  handoff->boot, handoff->pid, handoff->start_time);
	file = openat(directory, temporary, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
	if (file >= 0) {
		if (write(file, buffer, length) == length && !fsync(file) &&
		    !linkat(directory, temporary, directory, LEGACY_PENDING, 0))
			result = 0;
		close(file);
		unlinkat(directory, temporary, 0);
		if (fsync(directory))
			result = -1;
	}
	close(directory);
	return result;
}

static int legacy_sync_handoff(void)
{
	int directory = legacy_state_directory(), file, result;

	if (directory < 0)
		return -1;
	file = openat(directory, LEGACY_PENDING, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
	result = file < 0 ? -1 : fsync(file);
	if (file >= 0)
		close(file);
	if (fsync(directory))
		result = -1;
	close(directory);
	return result;
}

static int legacy_archive_previous_boot(void)
{
	int directory = legacy_state_directory(), result;

	if (directory < 0)
		return -1;
	result = renameat(directory, LEGACY_PENDING, directory, "legacy-stop-pending.previous-boot");
	if (!result)
		result = fsync(directory);
	close(directory);
	return result;
}

static int legacy_journals_clean(void)
{
	const char *journals[] = {
		LEGACY_EVENTS "/cleanup-unconfirmed",
		LEGACY_EVENTS "/run/worker-pending",
	};
	size_t i;

	for (i = 0; i < sizeof(journals) / sizeof(journals[0]); i++) {
		if (!access(journals[i], F_OK) || errno != ENOENT) {
			fprintf(stderr, "Legacy FAS worker cleanup has not been confirmed\n");
			return 1;
		}
	}
	return 0;
}

static int legacy_executable(const char *executable)
{
	return !strcmp(executable, LEGACY_EXECUTABLE) ||
		!strcmp(executable, LEGACY_EXECUTABLE " (deleted)");
}

static int legacy_identity_live(long pid, unsigned long long start_time)
{
	char path[64], executable[512];
	unsigned long long observed;
	ssize_t length;
	int result;

	snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
	length = readlink(path, executable, sizeof(executable) - 1);
	if (length < 0)
		return errno == ENOENT ? 0 : -1;
	if (!length)
		return -1;
	executable[length] = 0;
	if (!legacy_executable(executable))
		return 0;
	if (!start_time)
		return 1;
	result = legacy_start_time(pid, &observed);
	return result == 1 ? observed == start_time : result;
}

static int legacy_current_pid(long *pid)
{
	FILE *file = fopen(LEGACY_EVENTS "/run/pid", "re");
	char extra;
	int result;

	if (!file)
		return errno == ENOENT ? 0 : -1;
	result = fscanf(file, "%ld %c", pid, &extra);
	fclose(file);
	if (result != 1 || *pid <= 1 || *pid > 4194304)
		return -1;
	return legacy_identity_live(*pid, 0);
}

/* Read-only recovery gate: both the old identity and current supervisor must
 * be gone, and the worker's reliable cleanup journals must be clear.
 */
static int legacy_clean(void)
{
	struct legacy_handoff handoff;
	char boot[40];
	long pid;
	int record = legacy_read_handoff(&handoff), result;

	if (record < 0 || legacy_boot(boot) || legacy_journals_clean())
		return 1;
	if (record && !strcmp(handoff.boot, boot)) {
		result = legacy_identity_live(handoff.pid, handoff.start_time);
		if (result != 0) {
			fprintf(stderr, "Legacy handoff is pending; original supervisor has not exited\n");
			return 1;
		}
	}
	result = legacy_current_pid(&pid);
	if (result != 0) {
		fprintf(stderr, "Legacy supervisor is still running or cannot be identified\n");
		return 1;
	}
	printf("legacy_was_running=%d\n", record && !strcmp(handoff.boot, boot));
	return 0;
}

static int suspend_legacy(void)
{
	char path[64], executable[512];
	struct legacy_handoff handoff;
	long pid;
	ssize_t length;
	int fd, result, record;
	struct pollfd stopped;

	if (!access(LEGACY_EVENTS "/cleanup-unconfirmed", F_OK)) {
		fprintf(stderr, "Legacy FAS has unconfirmed cleanup\n");
		return 1;
	}
	record = legacy_read_handoff(&handoff);
	if (record < 0)
		return 1;
	if (record) {
		char boot[40];

		if (legacy_boot(boot))
			return 1;
		if (strcmp(boot, handoff.boot)) {
			/* The previous boot's process identity cannot still be alive.
			 * Keep its audit record, then independently stop this boot's FAS.
			 */
			if (legacy_archive_previous_boot())
				return 1;
			record = 0;
		} else {
			result = legacy_identity_live(handoff.pid, handoff.start_time);
			if (result < 0)
				return 1;
			if (!result)
				return legacy_clean();
			pid = handoff.pid; /* Explicit retry of the same recorded stop. */
		}
	}
	if (!record) {
		result = legacy_current_pid(&pid);
		if (result < 0)
			return 1;
		if (!result)
			return legacy_clean();
	}
	snprintf(path, sizeof(path), "/proc/%ld/exe", pid);
	length = readlink(path, executable, sizeof(executable) - 1);
	if (length < 0 && errno == ENOENT) {
		return legacy_clean();
	}
	if (length <= 0)
		return 1;
	executable[length] = 0;
	if (!legacy_executable(executable)) {
		return legacy_clean();
	}
	fd = syscall(SYS_pidfd_open, (pid_t)pid, 0);
	if (fd < 0)
		return errno == ESRCH ? legacy_clean() : 1;
	/* The PID could have been recycled before pidfd_open(). */
	length = readlink(path, executable, sizeof(executable) - 1);
	if (length < 0) {
		int gone = errno == ENOENT;
		close(fd);
		return gone ? legacy_clean() : 1;
	}
	if (!length || (executable[length] = 0, !legacy_executable(executable))) {
		close(fd);
		return !length ? 1 : legacy_clean();
	}
	if (!record) {
		handoff.pid = pid;
		if (legacy_boot(handoff.boot) || legacy_start_time(pid, &handoff.start_time) != 1 ||
		    legacy_record_handoff(&handoff)) {
			close(fd);
			fprintf(stderr, "Cannot persist legacy handoff; no stop signal was sent\n");
			return 1;
		}
	} else if (legacy_identity_live(pid, handoff.start_time) != 1 || legacy_sync_handoff()) {
		close(fd);
		return 1;
	}
	if (syscall(SYS_pidfd_send_signal, fd, SIGTERM, NULL, 0)) {
		int disappeared = errno == ESRCH;
		close(fd);
		return disappeared ? legacy_clean() : 1;
	}
	stopped = (struct pollfd){ .fd = fd, .events = POLLIN };
	do { result = poll(&stopped, 1, 20000); } while (result < 0 && errno == EINTR);
	close(fd);
	if (result <= 0 || !(stopped.revents & POLLIN)) {
		fprintf(stderr, "Legacy FAS cleanup remains pending; inspect legacy-stop-pending before recovery\n");
		return 1;
	}
	return legacy_clean();
}

int main(int argc, char **argv)
{
	struct fp_config config;
	struct fp_fas_tuning fas;
	FILE *input;
	int device, gpu = -1, result = 0;
	char status[4096];
	ssize_t count;
	bool load_fas = argc == 3 && !strcmp(argv[1], "load-fas");
	bool check_fas = argc == 3 && !strcmp(argv[1], "check-fas");

	if (argc == 3 && !strcmp(argv[1], "unpack-settings"))
		return unpack_settings(argv[2]);
	if (argc == 3 && (!strcmp(argv[1], "load-aux") || !strcmp(argv[1], "check-aux")))
		return handle_aux(argv[2], !strcmp(argv[1], "load-aux"));
	if (argc == 2 && !strcmp(argv[1], "suspend-legacy"))
		return suspend_legacy();
	if (argc == 2 && !strcmp(argv[1], "legacy-clean"))
		return legacy_clean();
	if ((load_fas || check_fas) &&
	    (read_fas_tuning(argv[2], &fas) || require_fas_tuning()))
		return 1;
	if (check_fas)
		return 0;
	if (argc < 2 || argc > 3 ||
	    (strcmp(argv[1], "status") && strcmp(argv[1], "disable") &&
	     !(argc == 3 && (!strcmp(argv[1], "load") || !strcmp(argv[1], "load-rules") ||
			    !strcmp(argv[1], "load-scene") || load_fas)))) {
		fprintf(stderr, "Usage: %s status | disable | load CONFIG.bin | load-rules RULES.bin | load-scene TABLE.bin | load-fas TUNING.bin | check-fas TUNING.bin | load-aux AUX.bin | check-aux AUX.bin\n", argv[0]);
		return 2;
	}
	device = open(FRAME_POLICY_DEVICE, O_RDWR | O_CLOEXEC);
	if (device < 0) {
		perror("open frame policy");
		return 1;
	}
	if (load_fas) {
		result = ioctl(device, FP_FAS_TUNING, &fas);
	} else if (!strcmp(argv[1], "load-rules") || !strcmp(argv[1], "load-scene")) {
		int scene = !strcmp(argv[1], "load-scene");
		size_t length = scene ? sizeof(struct fp_scene_table) : sizeof(struct fp_game_rules);
		__u32 abi, expected = scene ? FP_SCENE_ABI : FP_GAME_RULES_ABI;
		void *rules = malloc(length);

		if (!rules) {
			close(device);
			return 1;
		}
		input = fopen(argv[2], "rb");
		if (!input || fread(rules, length, 1, input) != 1 ||
		    fgetc(input) != EOF || ferror(input) ||
		    (memcpy(&abi, rules, sizeof(abi)), abi != expected)) {
			fprintf(stderr, "Frequency rules have an invalid size or version\n");
			if (input)
				fclose(input);
			free(rules);
			close(device);
			return 1;
		}
		fclose(input);
		result = ioctl(device, scene ? FP_SCENE_TABLE : FP_GAME_RULES, rules);
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
