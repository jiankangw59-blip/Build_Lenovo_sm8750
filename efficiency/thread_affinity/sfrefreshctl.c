// SPDX-License-Identifier: GPL-2.0
#include "scene_refresh_display.h"
#include <inttypes.h>
#include <string.h>

/* Deliberately read-only until the display ownership/restore path is wired. */
int main(int argc, char **argv)
{
	if (argc != 2 || strcmp(argv[1], "probe")) {
		fprintf(stderr, "usage: sfrefreshctl probe\n");
		return 2;
	}
	struct srd_connection *connection = NULL;
	struct srd_snapshot snapshot = {0};
	if (srd_connect(&connection) || srd_read(connection, &snapshot)) {
		perror("SurfaceFlinger probe");
		srd_disconnect(connection);
		return 1;
	}
	printf("{\"physical_display_id\":\"%" PRIu64 "\",\"active_mode_id\":%d,\"modes\":[",
	       snapshot.physical_display_id, snapshot.active_mode_id);
	for (size_t i = 0; i < snapshot.count; i++) {
		const struct srp_mode *mode = &snapshot.modes[i];
		printf("%s{\"mode_id\":%d,\"hz\":%u,\"width\":%u,\"height\":%u}",
		       i ? "," : "", mode->mode_id, mode->hz, mode->width, mode->height);
	}
	puts("]}");
	srd_snapshot_free(&snapshot);
	srd_disconnect(connection);
	return 0;
}
