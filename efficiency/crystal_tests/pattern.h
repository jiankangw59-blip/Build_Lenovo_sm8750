#ifndef ACE6_CRYSTAL_TEST_PATTERN_H
#define ACE6_CRYSTAL_TEST_PATTERN_H

#include <stdint.h>

#define PROBE_PAGE_BYTES 4096U
#define PROBE_PAGES 8192U
#define PROBE_BYTES (PROBE_PAGE_BYTES * PROBE_PAGES)
#define PROBE_SCHEMA "zram-probe-four-way-v1"
#define PROBE_PAIRS_SCHEMA "sddc-pairs-2k-v1"

/* Identical to efficiency/zram_readback.c, made explicitly little-endian. */
static inline void probe_pattern(unsigned char *out, unsigned page, uint64_t seed)
{
    uint64_t state = seed ^ ((uint64_t)(page + 1) * UINT64_C(0x9e3779b97f4a7c15));
    unsigned mode = page % 4;
    unsigned char fill = mode == 1 ? (unsigned char)((page + seed) % 251 + 1) : 0;
    for (unsigned offset = 0; offset < PROBE_PAGE_BYTES; offset++)
        out[offset] = fill;
    if (mode < 2)
        return;
    for (unsigned offset = 0; offset < PROBE_PAGE_BYTES; offset += mode == 2 ? 8 : 128) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        for (unsigned byte = 0; byte < 8; byte++)
            out[offset + byte] = (unsigned char)(state >> (byte * 8));
    }
}

static inline uint64_t probe_hash(uint64_t hash, const unsigned char *data, unsigned bytes)
{
    for (unsigned i = 0; i < bytes; i++) {
        hash ^= data[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

/* Eight-page families: two identical pages per variant. Each contains a
 * 2 KiB random block repeated twice, then both copies receive a few matching
 * edits. Ordinary compressed objects are large enough for SDDC admission. */
static inline void probe_pattern_pairs(unsigned char *out, unsigned page, uint64_t seed)
{
    uint64_t state = seed ^ ((uint64_t)(page / 8 + 1) * UINT64_C(0x9e3779b97f4a7c15));
    for (unsigned offset = 0; offset < PROBE_PAGE_BYTES / 2; offset += 8) {
        state ^= state << 13;
        state ^= state >> 7;
        state ^= state << 17;
        for (unsigned byte = 0; byte < 8; byte++) {
            unsigned char value = (unsigned char)(state >> (byte * 8));
            out[offset + byte] = value;
            out[offset + byte + PROBE_PAGE_BYTES / 2] = value;
        }
    }
    unsigned variant = (page % 8) / 2;
    if (variant)
        for (unsigned edit = 0; edit < 4; edit++) {
            unsigned position = (variant * 149 + edit * 67) % (PROBE_PAGE_BYTES / 2);
            out[position] ^= (unsigned char)(0x31 + variant * 17 + edit);
            out[position + PROBE_PAGE_BYTES / 2] = out[position];
        }
}

#endif
