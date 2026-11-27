#ifndef ACE6_OWNED_LOGIC_H
#define ACE6_OWNED_LOGIC_H

#include <stdint.h>

#define OWNED_PAGES 1024U
#define OWNED_SAMPLES 64U
#define OWNED_BYTES (OWNED_PAGES * 4096U)
#define OWNED_PAGEOUT_BYTES (OWNED_SAMPLES * 4096U)

static inline uint64_t owned_swap_offset(uint64_t entry)
{
    return (entry & ((UINT64_C(1) << 55) - 1)) >> 5;
}

static inline int owned_valid_swap_entry(uint64_t entry, uint64_t slots)
{
    return (entry & (UINT64_C(1) << 62)) && !(entry & (UINT64_C(1) << 63)) &&
           owned_swap_offset(entry) > 0 && owned_swap_offset(entry) < slots;
}

#endif
