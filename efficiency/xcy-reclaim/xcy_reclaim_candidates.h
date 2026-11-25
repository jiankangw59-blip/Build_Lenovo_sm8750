#ifndef XCY_RECLAIM_CANDIDATES_H
#define XCY_RECLAIM_CANDIDATES_H

#include "xcy_reclaim_importance.h"
#include "xcy_reclaim_rank.h"

#define XCY_RECLAIM_MEMBERS_MAX 512U

struct xcy_reclaim_names {
	const char *const *values;
	size_t count;
};

/* Raw state from Scene, Android audio and the module's allowlist and lease
 * sources. The transport owns these arrays for the whole decision. */
struct xcy_reclaim_protection {
	struct xcy_reclaim_names visible;
	struct xcy_reclaim_names active;
	struct xcy_reclaim_names package_whitelist;
	struct xcy_reclaim_names full_whitelist;
	struct xcy_reclaim_names exact_whitelist;
	const char *current_game;
	const uint32_t *playback_uids;
	size_t playback_count;
	uint32_t sync_uid;
	uint64_t sync_expires_ms;
	uint64_t observed_ms;
	uint64_t expires_ms;
	int ready;
};

/* The candidate is an app group, with verified original inventory row
 * indices. This avoids materializing another large Framework snapshot. */
struct xcy_reclaim_candidate {
	uint32_t uid;
	uint32_t representative;
	size_t count;
	uint32_t members[XCY_RECLAIM_MEMBERS_MAX];
};

int xcy_reclaim_collect_package(const xcy_memory_snapshot_t *inventory,
				const xcy_policy_result_t *results,
				const struct xcy_reclaim_protection *protection,
				uint64_t now_ms, size_t representative,
				struct xcy_reclaim_candidate *candidate);

#endif
