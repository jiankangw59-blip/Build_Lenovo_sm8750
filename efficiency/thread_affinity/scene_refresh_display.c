// SPDX-License-Identifier: GPL-2.0
#define _GNU_SOURCE
#include "scene_refresh_display.h"
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <math.h>
#include <stdlib.h>
#include <string.h>

void srd_snapshot_free(struct srd_snapshot *snapshot)
{
	free(snapshot->modes);
	memset(snapshot, 0, sizeof(*snapshot));
}

static int parse_mode(const char *line, struct srp_mode *mode)
{
	unsigned int id, hwc, width, height;
	double hz;
	int used = 0;
	if (sscanf(line, " {id=%u, hwcId=%u, resolution=%ux%u, vsyncRate=%lf Hz%n",
		   &id, &hwc, &width, &height, &hz, &used) != 5 || !used ||
	    id > INT32_MAX || !width || !height || !isfinite(hz) || hz < 1 || hz > 1000)
		return -1;
	/* Scene's catalog uses integer Hz, with normal fractional panel tolerance. */
	double rounded = floor(hz + 0.5);
	if (fabs(hz - rounded) > 0.1)
		return -1;
	*mode = (struct srp_mode){ (int32_t)id, (uint32_t)rounded, width, height };
	return 0;
}

int srd_parse(FILE *file, struct srd_snapshot *out)
{
	struct srd_snapshot next = { .active_mode_id = -1 };
	struct srp_mode active = {0};
	char *line = NULL;
	size_t bytes = 0, capacity = 0, consumed = 0;
	bool display = false, listing = false, complete = false;
	ssize_t length;
	int error = EPROTO;

	while ((length = getline(&line, &bytes, file)) >= 0) {
		consumed += (size_t)length;
		if (consumed > 8U * 1024 * 1024) { error = E2BIG; goto fail; }
		if (!display) {
			if (sscanf(line, "Pacesetter Display %" SCNu64,
				   &next.physical_display_id) == 1)
				display = true;
			continue;
		}
		char *text = line + strspn(line, " \t");
		/* An incomplete first display must never borrow another display's
		 * catalog while retaining the first physical ID. */
		if (!strncmp(text, "Pacesetter Display ", 19) || !strncmp(text, "Display ", 8))
			goto fail;
		if (!strncmp(text, "activeMode=", 11)) {
			if (next.active_mode_id >= 0 || parse_mode(text + 11, &active)) goto fail;
			next.active_mode_id = active.mode_id;
		} else if (!strncmp(text, "displayModes=", 13)) {
			if (listing) goto fail;
			listing = true;
		} else if (!strncmp(text, "displayManagerPolicy=", 21)) {
			complete = true;
			break;
		} else if (listing && *text == '{') {
			struct srp_mode mode;
			if (parse_mode(text, &mode)) goto fail;
			for (size_t i = 0; i < next.count; i++)
				if (next.modes[i].mode_id == mode.mode_id) goto fail;
			if (next.count == capacity) {
				size_t larger = capacity ? capacity * 2 : 16;
				if (larger > 4096) { error = E2BIG; goto fail; }
				void *buffer = realloc(next.modes, larger * sizeof(*next.modes));
				if (!buffer) { error = ENOMEM; goto fail; }
				next.modes = buffer;
				capacity = larger;
			}
			next.modes[next.count++] = mode;
		}
	}
	if (ferror(file)) { error = EIO; goto fail; }
	if (!complete || !listing || next.active_mode_id < 0) goto fail;
	bool found = false;
	/* Never let a display-rate policy silently switch display resolution. */
	size_t kept = 0;
	for (size_t i = 0; i < next.count; i++) {
		struct srp_mode mode = next.modes[i];
		if (mode.width != active.width || mode.height != active.height) continue;
		for (size_t j = 0; j < kept; j++)
			if (next.modes[j].hz == mode.hz) goto fail;
		if (mode.mode_id == active.mode_id) {
			if (mode.hz != active.hz) goto fail;
			found = true;
		}
		next.modes[kept++] = mode;
	}
	if (!found || !kept) goto fail;
	next.count = kept;
	free(line);
	srd_snapshot_free(out);
	*out = next;
	return 0;
fail:
	free(line);
	srd_snapshot_free(&next);
	errno = error;
	return -1;
}

#ifdef __ANDROID__
#include <android/binder_ibinder.h>
#include <android/binder_parcel.h>
#include <dlfcn.h>
#include <fcntl.h>
#include <pthread.h>
#include <unistd.h>

struct srd_connection { AIBinder *binder; };
static pthread_once_t class_once = PTHREAD_ONCE_INIT;
static AIBinder_Class *composer_class;

static void *class_create(void *value) { return value; }
static void class_destroy(void *value) { (void)value; }
static binder_status_t class_transact(AIBinder *binder, transaction_code_t code,
				     const AParcel *input, AParcel *output)
{
	(void)binder; (void)code; (void)input; (void)output;
	return STATUS_UNKNOWN_TRANSACTION;
}
static void init_class(void)
{
	composer_class = AIBinder_Class_define("android.ui.ISurfaceComposer",
		class_create, class_destroy, class_transact);
}

static int binder_error(binder_status_t status)
{
	if (status == STATUS_OK) return 0;
	fprintf(stderr, "SurfaceFlinger Binder status=%" PRId32 "\n", status);
	errno = status == STATUS_DEAD_OBJECT ? EPIPE :
		status == STATUS_PERMISSION_DENIED ? EACCES : EIO;
	return -1;
}

int srd_connect(struct srd_connection **out)
{
	/* checkService is a platform export, not an NDK header promise. */
	void *library = dlopen("libbinder_ndk.so", RTLD_NOW | RTLD_LOCAL);
	if (!library) { errno = ENOSYS; return -1; }
	AIBinder *(*check_service)(const char *) = NULL;
	void *symbol = dlsym(library, "AServiceManager_checkService");
	_Static_assert(sizeof(symbol) == sizeof(check_service), "function pointer size");
	memcpy(&check_service, &symbol, sizeof(symbol));
	if (!check_service) { dlclose(library); errno = ENOSYS; return -1; }
	AIBinder *binder = check_service("SurfaceFlinger");
	dlclose(library); /* Normal NDK calls below keep the linked library loaded. */
	if (!binder) { errno = ENODEV; return -1; }
	pthread_once(&class_once, init_class);
	if (!composer_class || !AIBinder_associateClass(binder, composer_class)) {
		AIBinder_decStrong(binder); errno = EPROTONOSUPPORT; return -1;
	}
	if (binder_error(AIBinder_ping(binder))) {
		fprintf(stderr, "SurfaceFlinger stage=ping\n");
		AIBinder_decStrong(binder); return -1;
	}
	struct srd_connection *connection = calloc(1, sizeof(*connection));
	if (!connection) { AIBinder_decStrong(binder); return -1; }
	connection->binder = binder;
	*out = connection;
	return 0;
}

void srd_disconnect(struct srd_connection *connection)
{
	if (!connection) return;
	AIBinder_decStrong(connection->binder);
	free(connection);
}

struct dump_job {
	AIBinder *binder;
	int fd;
	binder_status_t status;
};

static void *dump_to_pipe(void *argument)
{
	struct dump_job *job = argument;
	job->status = AIBinder_dump(job->binder, job->fd, NULL, 0);
	close(job->fd);
	return NULL;
}

int srd_read(struct srd_connection *connection, struct srd_snapshot *out)
{
	/* Some vendor Binder domains reject transferred memfds. Drain a pipe in
	 * parallel so dumps larger than pipe capacity cannot deadlock. No disk IO,
	 * timer, or permanent sampling thread. */
	int pipes[2];
	if (pipe2(pipes, O_CLOEXEC)) return -1;
	FILE *file = fdopen(pipes[0], "re");
	if (!file) {
		int saved = errno; close(pipes[0]); close(pipes[1]); errno = saved; return -1;
	}
	struct dump_job job = { connection->binder, pipes[1], STATUS_UNKNOWN_ERROR };
	pthread_t thread;
	int created = pthread_create(&thread, NULL, dump_to_pipe, &job);
	if (created) { fclose(file); close(pipes[1]); errno = created; return -1; }
	struct srd_snapshot next = {0};
	int result = srd_parse(file, &next), saved = errno;
	/* The parser finishes at the catalog. Always drain the remainder before
	 * joining the writer; it can still be writing a large layer dump. */
	char discard[8192];
	while (fread(discard, 1, sizeof(discard), file) > 0) {}
	fclose(file);
	pthread_join(thread, NULL);
	if (binder_error(job.status)) { srd_snapshot_free(&next); return -1; }
	if (!result) { srd_snapshot_free(out); *out = next; }
	errno = saved;
	return result;
}

int srd_request_mode(struct srd_connection *connection,
		     const struct srd_snapshot *snapshot, int32_t mode_id)
{
	bool found = false;
	for (size_t i = 0; i < snapshot->count; i++)
		found |= snapshot->modes[i].mode_id == mode_id;
	if (!found) { errno = EINVAL; return -1; }
	AParcel *input = NULL, *output = NULL;
	binder_status_t status = AIBinder_prepareTransaction(connection->binder, &input);
	if (status == STATUS_OK) status = AParcel_writeInt32(input, mode_id);
	if (status == STATUS_OK)
		status = AParcel_writeInt64(input, (int64_t)snapshot->physical_display_id);
	if (status == STATUS_OK)
		/* Match Scene's direct command: mode id, not Hz. SF owns validation. */
		status = AIBinder_transact(connection->binder, 1035, &input, &output, 0);
	if (input) AParcel_delete(input);
	if (output) AParcel_delete(output);
	return binder_error(status);
}
#endif
