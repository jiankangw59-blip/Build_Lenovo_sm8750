#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "xcy_reclaim_lease.h"
#include "xcy_reclaim_protection.h"

static void test_sync_lease(void)
{
	const char boot[] = "01234567-89ab-cdef-0123-456789abcdef";
	const char previous_boot[] = "01234567-89ab-cdef-0123-456789abcdee";
	const char active[] =
		"{\"version\":1,\"uid\":10123,\"pid\":120,\"startTime\":\"95\","
		"\"expiresAt\":3000,\"owner\":\"headless\",\"state\":\"active\","
		"\"bootId\":\"01234567-89ab-cdef-0123-456789abcdef\",\"token\":\"opaque\"}";
	char malformed[sizeof(active) + 9];
	xcy_sync_lease_t lease;
	size_t size = sizeof(active) - 1;

	assert(xcy_parse_sync_lease(active, size, boot, 2000, &lease) == 1);
	assert(lease.uid == 10123 && lease.pid == 120 && lease.start_ticks == 95);
	assert(lease.expires_ms == 3000 && lease.headless);
	assert(xcy_parse_sync_lease(active, size, boot, 3000, &lease) == 2);
	assert(xcy_parse_sync_lease(active, size, previous_boot, 100, &lease) == 2);
	assert(xcy_parse_sync_lease(active, size - 1, boot, 2000, &lease) == 0);
	snprintf(malformed, sizeof(malformed), "%.*s,\"pid\":1}", (int)size - 1, active);
	assert(xcy_parse_sync_lease(malformed, strlen(malformed), boot, 2000, &lease) == 0);
}

static void test_audio(void)
{
	const char valid[] = "XCY_AUDIO_V1 round-12 1000 5100 1\nU 10100\nEND round-12\n";
	const char empty[] = "XCY_AUDIO_V1 round-12 1000 5100 0\nEND round-12\n";
	const char truncated[] = "XCY_AUDIO_V1 round-12 1000 5100 1\nEND round-12\n";
	struct xcy_reclaim_protection_frame frame = {0};

	assert(!xcy_reclaim_audio_parse(valid, sizeof(valid) - 1, "round-12", 1000, 1020, &frame));
	assert(frame.view.ready && frame.view.playback_count == 1 && frame.view.playback_uids[0] == 10100);
	xcy_reclaim_protection_free(&frame);
	assert(!xcy_reclaim_audio_parse(empty, sizeof(empty) - 1, "round-12", 1000, 1020, &frame));
	assert(frame.view.ready && !frame.view.playback_count);
	xcy_reclaim_protection_free(&frame);
	assert(xcy_reclaim_audio_parse(truncated, sizeof(truncated) - 1, "round-12", 1000, 1020, &frame) == -EINVAL);
	assert(xcy_reclaim_audio_parse(valid, sizeof(valid) - 1, "round-13", 1000, 1020, &frame) == -EINVAL);
	assert(xcy_reclaim_audio_parse(valid, sizeof(valid) - 1, "round-12", 1001, 1020, &frame) == -EINVAL);
	assert(xcy_reclaim_audio_parse(valid, sizeof(valid) - 1, "round-12", 1000, 5100, &frame) == -EINVAL);
}

static void test_scene(void)
{
	const char export[] =
		"version=1\nsource=scene_accessibility\ntime_sec=1200\n"
		"scene_foreground_pkg=org.example.active\nfocused_pkg=org.example.active\n"
		"visible_pkgs=org.example.visible,org.example.active\n"
		"scene_analyzed_small_window_pkgs=org.example.small\n"
		"unmapped_shell_count=0\n";
	struct xcy_reclaim_protection_frame frame = {0};
	char *data = xcy_snapshot_calloc(sizeof(export), 1);

	assert(data);
	memcpy(data, export, sizeof(export));
	assert(!xcy_reclaim_scene_parse(data, 1201, 0, 10080, 180000, &frame));
	assert(frame.scene_observed_ms == 9080 && frame.scene_valid_ms == 180000);
	assert(frame.view.visible.count == 3 && frame.view.active.count == 1);
	assert(!strcmp(frame.view.active.values[0], "org.example.active"));
	assert(!strcmp(frame.view.visible.values[2], "org.example.small"));
	xcy_reclaim_protection_free(&frame);

	data = xcy_snapshot_calloc(sizeof(export), 1);
	assert(data);
	memcpy(data, export, sizeof(export));
	assert(xcy_reclaim_scene_parse(data, 1400, 0, 300000, 180000, &frame) == -ESTALE);
	memcpy(data, export, sizeof(export));
	data[sizeof(export) - 3] = '1';
	assert(xcy_reclaim_scene_parse(data, 1201, 0, 10080, 180000, &frame) == -ESTALE);
	xcy_snapshot_free(data);
}

int main(void)
{
	test_audio();
	test_scene();
	test_sync_lease();
	puts("Audio, Scene and sync sources enforce freshness and complete observations");
	return 0;
}
