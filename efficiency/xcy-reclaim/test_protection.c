#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "xcy_reclaim_protection.h"

static const char complete[] =
	"XCY_PROTECT_V1 round-12 1000 5100 900 8 180000 10123 1200 1 1 1 1 1 1 1 180000 org.example.game\n"
	"V org.example.visible\nA org.example.active\nN org.example.white\nF org.example.full\n"
	"E org.example.process:remote\nU 10100\nEND round-12\n";

static const char scene[] =
	"XCY_PROTECT_V1 scene 1000 5100 900 8 180000 0 0 1 1 0 0 0 0 0 180000 -\n"
	"V org.example.visible\nA org.example.active\nEND scene\n";

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
	puts("protection frame rejects stale, partial and mismatched sources; Scene-only data cannot arm selection");
	return 0;
}
