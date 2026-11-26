/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SCENE_REFRESH_DISPLAY_H
#define SCENE_REFRESH_DISPLAY_H

#include <stdint.h>
#include <stdio.h>
#include "scene_refresh_policy.h"

/* Public SurfaceFlinger dump, never private Scene process memory. */
struct srd_snapshot {
	uint64_t physical_display_id;
	int32_t active_mode_id;
	struct srp_mode *modes;
	size_t count;
};

/* Parser is shared with the host tests. Output must be zero-initialized or own
 * a previously returned snapshot. Successful replacement frees its old modes;
 * failure preserves it. */
int srd_parse(FILE *file, struct srd_snapshot *out);
void srd_snapshot_free(struct srd_snapshot *snapshot);

struct srd_connection;
int srd_connect(struct srd_connection **out);
void srd_disconnect(struct srd_connection *connection);
int srd_read(struct srd_connection *connection, struct srd_snapshot *out);
/* ACK is not physical confirmation. Caller confirms on a later display event.
 * A catalog is valid only for its display generation; invalidate on resolution
 * changes. Do not call a synchronous Binder backend on the affinity event loop. */
int srd_request_mode(struct srd_connection *connection,
		     const struct srd_snapshot *snapshot, int32_t mode_id);

#endif
