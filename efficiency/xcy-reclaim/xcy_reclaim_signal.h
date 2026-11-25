#ifndef XCY_RECLAIM_SIGNAL_H
#define XCY_RECLAIM_SIGNAL_H

#include "xcy_reclaim_plan.h"
#include "xcy_reclaim_runtime.h"

struct pid;

struct xcy_reclaim_targets {
	struct pid **pids;
	size_t count;
	size_t selected;
};

int xcy_reclaim_targets_pin(const struct xcy_reclaim_plan *plan, size_t selected,
			     struct xcy_reclaim_targets *targets);
void xcy_reclaim_targets_put(struct xcy_reclaim_targets *targets);

/* The guard checks whether the protection/history generation is unchanged.
 * It must only inspect kernel-owned state and must not wait for Framework. */
typedef bool (*xcy_reclaim_guard_fn)(void *context, u64 now_ms, bool urgent);

int xcy_reclaim_execute(struct xcy_reclaim_runtime *runtime, u64 request,
			const struct xcy_reclaim_plan *plan, const struct xcy_reclaim_targets *targets,
			xcy_reclaim_guard_fn valid_guard, void *context);

#endif
