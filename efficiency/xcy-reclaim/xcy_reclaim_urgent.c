#include "xcy_reclaim_rank.h"

#ifdef __KERNEL__
#include <asm/sysreg.h>
#endif

/* Preserve the original Android build's binary64 calculation at the 90%
 * cutoff. Kernel callers enter their SIMD critical section before calling. */
static double urgent_score(const struct xcy_reclaim_rank_entry *item)
{
	double age = (double)item->age_ms;
	double base = 0.6;

	return (double)item->pss_kb * (base + (1.0 - base) * age / (age + 600000.0));
}

#ifdef __KERNEL__
static noinline size_t rank_binary64(const struct xcy_reclaim_rank_entry *items,
#else
static size_t rank_binary64(const struct xcy_reclaim_rank_entry *items,
#endif
			    size_t count, uint64_t minimum_pss)
{
	double maximum = 0.0;
	size_t selected = SIZE_MAX;
	size_t i;

	for (i = 0; i < count; ++i) {
		double score;

		if (!items[i].valid || items[i].pss_kb < minimum_pss)
			continue;
		score = urgent_score(&items[i]);
		if (score > maximum)
			maximum = score;
	}
	for (i = 0; i < count; ++i) {
		const struct xcy_reclaim_rank_entry *item = &items[i];

		if (!item->valid || item->pss_kb < minimum_pss ||
		    !(urgent_score(item) >= maximum * 0.9))
			continue;
		if (selected == SIZE_MAX || item->age_ms > items[selected].age_ms ||
		    (item->age_ms == items[selected].age_ms && item->pss_kb > items[selected].pss_kb))
			selected = i;
	}
	return selected;
}

size_t xcy_reclaim_rank_urgent_binary64(const struct xcy_reclaim_rank_entry *items,
				       size_t count, uint64_t minimum_pss)
{
	size_t result;
#ifdef __KERNEL__
	u64 saved_fpcr = read_sysreg(fpcr);
	u64 saved_fpsr = read_sysreg(fpsr);

	write_sysreg(0, fpcr);
	isb();
#endif
	result = rank_binary64(items, count, minimum_pss);
#ifdef __KERNEL__
	write_sysreg(saved_fpcr, fpcr);
	write_sysreg(saved_fpsr, fpsr);
#endif
	return result;
}
