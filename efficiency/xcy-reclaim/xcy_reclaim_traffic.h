#ifndef XCY_RECLAIM_TRAFFIC_H
#define XCY_RECLAIM_TRAFFIC_H

#include "xcy_reclaim_plan.h"

#define XCY_RECLAIM_BIND_TRAFFIC 0x58435901U
#define XCY_DOWNLOAD_RATE (2U * 1024 * 1024)
#define XCY_DOWNLOAD_HOLD_MS 45000U

struct xcy_reclaim_traffic;

/* RX is already included in the original RX + TX limit with the same floor.
 * Counter resets and deltas with no observable time interval are uncertain. */
static inline bool xcy_download_rate_busy(uint64_t previous, uint64_t next, uint64_t duration_ms)
{
	uint64_t delta, seconds, remaining_ms, threshold;

	if (next < previous)
		return true;
	delta = next - previous;
	if (!duration_ms)
		return delta != 0;
	seconds = duration_ms / 1000;
	remaining_ms = duration_ms % 1000;
	if (seconds > (XCY_RECLAIM_UINT64_MAX - XCY_DOWNLOAD_RATE) / XCY_DOWNLOAD_RATE)
		return false;
	threshold = XCY_DOWNLOAD_RATE * seconds + (XCY_DOWNLOAD_RATE * remaining_ms + 999) / 1000;
	return delta >= threshold;
}

#ifdef __KERNEL__
int xcy_reclaim_traffic_bind(unsigned int fd);
int xcy_reclaim_traffic_start(const struct xcy_reclaim_plan *plan, struct xcy_reclaim_traffic **result);
bool xcy_reclaim_traffic_busy(struct xcy_reclaim_traffic *traffic, const struct xcy_reclaim_plan *plan,
			      size_t selected, bool advance);
bool xcy_reclaim_traffic_held(u32 uid, u64 now);
void xcy_reclaim_traffic_put(struct xcy_reclaim_traffic *traffic);
#endif

#endif
