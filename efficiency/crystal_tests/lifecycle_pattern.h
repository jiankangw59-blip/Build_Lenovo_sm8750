#ifndef ACE6_LIFECYCLE_PATTERN_H
#define ACE6_LIFECYCLE_PATTERN_H

#include "pattern.h"

#define LIFECYCLE_SOURCE_PAGES 2U
#define LIFECYCLE_GROUP_PAGES 8U
#define LIFECYCLE_SCHEMA "sddc-pairs-source-lifecycle-v1"

static inline int lifecycle_seed_safe(uint64_t seed)
{
    for (unsigned group = 1; group <= PROBE_PAGES / LIFECYCLE_GROUP_PAGES; group++)
        if (seed == (uint64_t)group * UINT64_C(0x9e3779b97f4a7c15))
            return 0;
    return 1;
}

/* State 0: all original. 1: each family's first pair replaced. 2: first pair
 * discarded and read as zeros. This prioritizes likely reference sources;
 * it cannot identify the kernel's actual REF slot choice. */
static inline void lifecycle_expected(unsigned char *out, unsigned page,
                                      uint64_t old_seed, uint64_t new_seed, unsigned state)
{
    if (page % LIFECYCLE_GROUP_PAGES < LIFECYCLE_SOURCE_PAGES) {
        if (state == 2) {
            for (unsigned byte = 0; byte < PROBE_PAGE_BYTES; byte++)
                out[byte] = 0;
            return;
        }
        if (state == 1) {
            probe_pattern_pairs(out, page, new_seed);
            return;
        }
    }
    probe_pattern_pairs(out, page, old_seed);
}

static inline int lifecycle_matches(const unsigned char *actual, const unsigned char *expected)
{
    for (unsigned byte = 0; byte < PROBE_PAGE_BYTES; byte++)
        if (actual[byte] != expected[byte])
            return 0;
    return 1;
}

static inline int lifecycle_nonzero(const unsigned char *actual)
{
    for (unsigned byte = 0; byte < PROBE_PAGE_BYTES; byte++)
        if (actual[byte])
            return 1;
    return 0;
}

#endif
