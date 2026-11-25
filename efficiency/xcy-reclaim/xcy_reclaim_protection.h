#ifndef XCY_RECLAIM_PROTECTION_H
#define XCY_RECLAIM_PROTECTION_H

#include "xcy_reclaim_candidates.h"

#define XCY_RECLAIM_STATE_BYTES (256U * 1024U)
#define XCY_RECLAIM_STATE_PREFIX "XCY_PROTECT_V1 "
#define XCY_RECLAIM_SCENE_NONCE "scene"
#define XCY_RECLAIM_SCENE_PREFIX "XCY_PROTECT_V1 scene "
#define XCY_RECLAIM_SCENE_LOST "XCY_SCENE_INVALID\n"

struct xcy_reclaim_protection_frame {
	struct xcy_reclaim_protection view;
	char nonce[XCY_MEMORY_TOKEN_BYTES];
	uint64_t scene_observed_ms;
	uint64_t scene_valid_ms;
	uint64_t scene_generation;
	uint64_t grace_ms;
	char *storage;
	const char **names;
	uint32_t *uids;
};

/* A protection frame is one complete, time-bounded observation from the
 * existing Android source. Each list has an explicit count even when empty. */
int xcy_reclaim_protection_parse(const char *data, size_t length, const char *nonce,
				 uint64_t earliest_ms, uint64_t now_ms,
				 struct xcy_reclaim_protection_frame *frame);
void xcy_reclaim_protection_free(struct xcy_reclaim_protection_frame *frame);

#endif
