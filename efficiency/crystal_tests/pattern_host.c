#include "pattern.h"
#include "owned_logic.h"
#include "lifecycle_pattern.h"
#include "private_logic.h"
#include "backing_logic.h"

#ifdef _WIN32
#define API __declspec(dllexport)
#else
#define API __attribute__((visibility("default")))
#endif

API int backing_path_valid(const char *path)
{
    return private_backing_path(path);
}

API void pattern_fill(unsigned char *out, unsigned page, uint64_t seed)
{
    probe_pattern(out, page, seed);
}

API void pattern_pairs_fill(unsigned char *out, unsigned page, uint64_t seed)
{
    probe_pattern_pairs(out, page, seed);
}

/* Host test only; called serially by Python, never used on the device. */
API uint64_t pattern_checksum(uint64_t seed)
{
    static unsigned char page[PROBE_PAGE_BYTES];
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned index = 0; index < PROBE_PAGES; index++) {
        probe_pattern(page, index, seed);
        hash = probe_hash(hash, page, PROBE_PAGE_BYTES);
    }
    return hash;
}

API uint64_t owned_entry_offset(uint64_t entry, uint64_t slots)
{
    return owned_valid_swap_entry(entry, slots) ? owned_swap_offset(entry) : 0;
}

API uint64_t owned_pattern_checksum(uint64_t seed)
{
    static unsigned char page[PROBE_PAGE_BYTES];
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned index = 0; index < OWNED_PAGES; index++) {
        probe_pattern_pairs(page, index, seed);
        hash = probe_hash(hash, page, PROBE_PAGE_BYTES);
    }
    return hash;
}

API void lifecycle_pattern_fill(unsigned char *out, unsigned page,
                                 uint64_t old_seed, uint64_t new_seed, unsigned state)
{
    lifecycle_expected(out, page, old_seed, new_seed, state);
}

API unsigned lifecycle_private_index(const char *path)
{
    return private_zram_index(path);
}

API int lifecycle_known_version(const unsigned char *actual, unsigned page,
                                  uint64_t old_seed, uint64_t new_seed)
{
    static unsigned char expected[PROBE_PAGE_BYTES];
    if (!lifecycle_nonzero(actual))
        return -1;
    probe_pattern_pairs(expected, page, old_seed);
    if (lifecycle_matches(actual, expected))
        return 0;
    probe_pattern_pairs(expected, page, new_seed);
    return lifecycle_matches(actual, expected) ? 1 : -1;
}

API int lifecycle_safe_seed(uint64_t seed)
{
    return lifecycle_seed_safe(seed);
}
