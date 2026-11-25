#ifndef XCY_RECLAIM_RANK_H
#define XCY_RECLAIM_RANK_H

#include "xcy_reclaim_port.h"

struct xcy_reclaim_rank_entry {
	uint64_t pss_kb;
	uint64_t age_ms;
	int valid;
	int recent;
};

size_t xcy_reclaim_rank(const struct xcy_reclaim_rank_entry *items, size_t count,
			int urgent);

size_t xcy_reclaim_rank_urgent_binary64(const struct xcy_reclaim_rank_entry *items,
				       size_t count, uint64_t minimum_pss);

#endif
