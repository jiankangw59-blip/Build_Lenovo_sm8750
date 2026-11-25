#ifndef XCY_RECLAIM_PLAN_H
#define XCY_RECLAIM_PLAN_H

#include "xcy_reclaim_candidates.h"

struct xcy_reclaim_member {
	uint32_t pid;
	uint32_t uid;
	uint32_t kernel_uid;
	uint64_t starttime;
};

struct xcy_reclaim_group {
	uint32_t representative;
	uint16_t first;
	uint16_t count;
	bool departed;
};

struct xcy_reclaim_plan {
	struct xcy_reclaim_group groups[XCY_RECLAIM_MEMBERS_MAX];
	struct xcy_reclaim_member members[XCY_RECLAIM_MEMBERS_MAX];
	size_t group_count;
	size_t member_count;
};

/* These observations come from the kernel's event-driven visibility history. */
struct xcy_reclaim_age {
	uint64_t background_since_ms;
	int recent;
};

typedef struct xcy_reclaim_age (*xcy_reclaim_age_fn)(const xcy_memory_process_t *,
						   uint64_t now_ms, void *context);

int xcy_reclaim_plan_build(struct xcy_reclaim_plan *plan,
			   const xcy_memory_snapshot_t *inventory,
			   const struct xcy_reclaim_protection *protection, uint64_t now_ms,
			   xcy_reclaim_age_fn age, void *context);
int xcy_reclaim_plan_pick(const struct xcy_reclaim_plan *plan,
			  const xcy_memory_snapshot_t *inventory,
			  const xcy_memory_snapshot_t *pss, uint64_t now_ms, int urgent,
			  xcy_reclaim_age_fn age, void *context, size_t *selected);
int xcy_reclaim_plan_revalidate(const struct xcy_reclaim_plan *plan, size_t selected,
			       const xcy_memory_snapshot_t *first, const xcy_memory_snapshot_t *fresh,
			       const struct xcy_reclaim_protection *protection, uint64_t now_ms,
			       int urgent, xcy_reclaim_age_fn age, void *context);

#endif
