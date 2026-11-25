#ifndef XCY_RECLAIM_ENGINE_H
#define XCY_RECLAIM_ENGINE_H

#include "xcy_reclaim_plan.h"
#include "xcy_reclaim_reply.h"

struct xcy_reclaim_policy;

struct xcy_reclaim_source_view {
	struct xcy_reclaim_protection protection;
	u64 grace_ms;
	struct xcy_reclaim_policy *policy;
	void *reference;
};

/* A view is immutable while held. `valid_now` compares its generation with
 * the latest Scene/Audio/PMS/allowlist inputs using bounded kernel reads. */
struct xcy_reclaim_sources {
	void (*reset)(void);
	int (*publish)(const struct xcy_reclaim_reply *reply, const char *frame, size_t length);
	void (*observe)(const xcy_memory_snapshot_t *snapshot, const struct xcy_reclaim_source_view *view);
	int (*get)(const struct xcy_reclaim_reply *reply, struct xcy_reclaim_source_view *view);
	void (*put)(struct xcy_reclaim_source_view *view);
	bool (*valid_now)(const struct xcy_reclaim_source_view *view, u64 now_ms);
	struct xcy_reclaim_age (*age)(const xcy_memory_process_t *owner, u64 now_ms);
};

int xcy_reclaim_engine_register(const struct xcy_reclaim_sources *sources);

#endif
