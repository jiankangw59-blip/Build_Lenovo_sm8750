#ifndef XCY_RECLAIM_PROTECTION_H
#define XCY_RECLAIM_PROTECTION_H

#include "xcy_reclaim_candidates.h"

#define XCY_RECLAIM_STATE_BYTES 4096U
#define XCY_RECLAIM_AUDIO_PREFIX "XCY_AUDIO_V1 "
#define XCY_RECLAIM_AUDIO_LOST "XCY_AUDIO_INVALID\n"

struct xcy_reclaim_protection_frame {
	struct xcy_reclaim_protection view;
	char nonce[XCY_MEMORY_TOKEN_BYTES];
	uint64_t scene_observed_ms;
	uint64_t scene_valid_ms;
	char *storage;
	const char **names;
	uint32_t *uids;
};

void xcy_reclaim_protection_free(struct xcy_reclaim_protection_frame *frame);
int xcy_reclaim_audio_parse(const char *data, size_t length, const char *nonce,
			    uint64_t earliest_ms, uint64_t now_ms,
			    struct xcy_reclaim_protection_frame *frame);
int xcy_reclaim_scene_parse(char *data, uint64_t wall_now_sec, uint64_t fallback_sec,
			    uint64_t now, uint64_t maximum_age,
			    struct xcy_reclaim_protection_frame *frame);

#endif
