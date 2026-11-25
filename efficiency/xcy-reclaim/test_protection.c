#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "xcy_reclaim_lease.h"
#include "xcy_reclaim_protection.h"

static const char complete[] =
	"XCY_PROTECT_V1 round-12 1000 5100 900 8 180000 10123 1200 1 1 1 1 1 1 1 180000 org.example.game\n"
	"V org.example.visible\nA org.example.active\nN org.example.white\nF org.example.full\n"
	"E org.example.process:remote\nU 10100\nEND round-12\n";

static const char scene[] =
	"XCY_PROTECT_V1 scene 1000 5100 900 8 180000 0 0 1 1 0 0 0 0 0 180000 -\n"
	"V org.example.visible\nA org.example.active\nEND scene\n";

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

int main(void)
{
	struct xcy_reclaim_protection_frame frame = {0};
	char broken[sizeof(complete)];

	assert(!xcy_reclaim_protection_parse(complete, sizeof(complete) - 1, "round-12", 1000, 1020, &frame));
	assert(frame.view.ready && frame.scene_generation == 8 && frame.scene_observed_ms == 900);
	assert(frame.grace_ms == 180000 && frame.scene_valid_ms == 180000);
	assert(frame.view.sync_uid == 10123 && frame.view.sync_expires_ms == 1200);
	assert(frame.view.playback_count == 1 && frame.view.playback_uids[0] == 10100);
	assert(!strcmp(frame.view.current_game, "org.example.game"));
	assert(!strcmp(frame.view.visible.values[0], "org.example.visible"));
	assert(!strcmp(frame.view.active.values[0], "org.example.active"));
	assert(!strcmp(frame.view.package_whitelist.values[0], "org.example.white"));
	assert(!strcmp(frame.view.full_whitelist.values[0], "org.example.full"));
	assert(!strcmp(frame.view.exact_whitelist.values[0], "org.example.process:remote"));
	xcy_reclaim_protection_free(&frame);
	assert(xcy_reclaim_protection_parse(complete, sizeof(complete) - 1, "round-13", 1000, 1020, &frame) == -EINVAL);
	assert(xcy_reclaim_protection_parse(complete, sizeof(complete) - 1, "round-12", 1001, 1020, &frame) == -EINVAL);
	assert(xcy_reclaim_protection_parse(complete, sizeof(complete) - 1, "round-12", 1000, 5100, &frame) == -EINVAL);
	assert(xcy_reclaim_protection_parse(complete, sizeof(complete) - 2, "round-12", 1000, 1020, &frame) == -EINVAL);
	memcpy(broken, complete, sizeof(broken));
	*strstr(broken, "N org.example.white") = 'F';
	assert(xcy_reclaim_protection_parse(broken, sizeof(broken) - 1, "round-12", 1000, 1020, &frame) == -EINVAL);
	assert(!xcy_reclaim_protection_parse(scene, sizeof(scene) - 1, "scene", 0, 1020, &frame));
	assert(!frame.view.ready && frame.view.visible.count == 1 && frame.view.active.count == 1);
	xcy_reclaim_protection_free(&frame);
	test_sync_lease();
	puts("protection frame rejects stale, partial and mismatched sources; Scene-only data cannot arm selection");
	return 0;
}
