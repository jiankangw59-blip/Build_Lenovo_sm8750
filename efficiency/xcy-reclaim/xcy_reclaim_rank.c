#ifdef __KERNEL__
#include <asm/neon.h>
#endif

#include "xcy_reclaim_rank.h"

#define XCY_RECLAIM_MAX_RANK_ENTRIES 512U

size_t xcy_reclaim_rank(const struct xcy_reclaim_rank_entry *items, size_t count,
			int urgent)
{
	uint64_t minimum_pss, maximum_pss = 0, oldest = 0;
	size_t selected = SIZE_MAX;
	size_t i;

	if (!items || count > XCY_RECLAIM_MAX_RANK_ENTRIES)
		return SIZE_MAX;
	for (i = 0; i < count; ++i)
		if (items[i].valid && items[i].pss_kb > maximum_pss)
			maximum_pss = items[i].pss_kb;
	if (!maximum_pss)
		return SIZE_MAX;
	minimum_pss = (maximum_pss / 5) * 3 + ((maximum_pss % 5) * 3 + 4) / 5;

	if (urgent) {
#ifdef __KERNEL__
		kernel_neon_begin();
#endif
		selected = xcy_reclaim_rank_urgent_binary64(items, count, minimum_pss);
#ifdef __KERNEL__
		kernel_neon_end();
#endif
		return selected;
	}

	for (i = 0; i < count; ++i)
		if (items[i].valid && !items[i].recent && items[i].pss_kb >= minimum_pss &&
		    items[i].age_ms > oldest)
			oldest = items[i].age_ms;
	for (i = 0; i < count; ++i) {
		const struct xcy_reclaim_rank_entry *item = &items[i];

		if (!item->valid || item->recent || item->pss_kb < minimum_pss ||
		    oldest - item->age_ms > 60000)
			continue;
		if (selected == SIZE_MAX || item->pss_kb > items[selected].pss_kb ||
		    (item->pss_kb == items[selected].pss_kb && item->age_ms > items[selected].age_ms))
			selected = i;
	}
	return selected;
}
