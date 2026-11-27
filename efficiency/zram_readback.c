#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PAGE_BYTES 4096

static void pattern(unsigned char *out, unsigned page, uint64_t seed) {
    uint64_t state = seed ^ ((uint64_t)(page + 1) * UINT64_C(0x9e3779b97f4a7c15));
    switch (page % 4) {
    case 0: memset(out, 0, PAGE_BYTES); return;
    case 1: memset(out, (page + seed) % 251 + 1, PAGE_BYTES); return;
    case 2:
        for (unsigned offset = 0; offset < PAGE_BYTES; offset += 8) {
            state ^= state << 13; state ^= state >> 7; state ^= state << 17;
            memcpy(out + offset, &state, sizeof(state));
        }
        return;
    default:
        memset(out, 0, PAGE_BYTES);
        for (unsigned offset = 0; offset < PAGE_BYTES; offset += 128) {
            state ^= state << 13; state ^= state >> 7; state ^= state << 17;
            memcpy(out + offset, &state, sizeof(state));
        }
    }
}

int main(int argc, char **argv) {
    if (argc != 5 || (strcmp(argv[2], "write") && strcmp(argv[2], "verify"))) {
        fprintf(stderr, "usage: zram_readback /dev/block/zramN write|verify pages seed\n");
        return 2;
    }
    const char *prefix = "/dev/block/zram";
    char *end = NULL;
    if (strncmp(argv[1], prefix, strlen(prefix))) return 2;
    unsigned long device = strtoul(argv[1] + strlen(prefix), &end, 10);
    // zram0 is the live swap device: the probe must never open it.
    if (!end || *end || device == 0 || device > 1024) return 2;
    unsigned long pages = strtoul(argv[3], &end, 10);
    if (!pages || pages > 65536 || *end) return 2;
    uint64_t seed = strtoull(argv[4], &end, 0);
    if (*end) return 2;
    int writing = !strcmp(argv[2], "write");
    int fd = open(argv[1], (writing ? O_RDWR : O_RDONLY) | O_DIRECT | O_CLOEXEC);
    if (fd < 0) { perror("open direct block device"); return 1; }
    unsigned char *expected = NULL, *actual = NULL;
    if (posix_memalign((void **)&expected, PAGE_BYTES, PAGE_BYTES) ||
        posix_memalign((void **)&actual, PAGE_BYTES, PAGE_BYTES)) {
        fprintf(stderr, "aligned allocation failed\n"); close(fd); free(expected); free(actual); return 1;
    }
    int result = 0;
    for (unsigned long i = 0; i < pages; ++i) {
        pattern(expected, i, seed);
        ssize_t bytes;
        do {
            bytes = writing ? pwrite(fd, expected, PAGE_BYTES, (off_t)i * PAGE_BYTES)
                            : pread(fd, actual, PAGE_BYTES, (off_t)i * PAGE_BYTES);
        } while (bytes < 0 && errno == EINTR);
        if (bytes != PAGE_BYTES) {
            fprintf(stderr, "direct I/O failed at page %lu: bytes=%zd errno=%d\n", i, bytes, errno);
            result = 1; break;
        }
        if (!writing && memcmp(expected, actual, PAGE_BYTES)) {
            fprintf(stderr, "readback mismatch at page %lu seed=%llu\n", i, (unsigned long long)seed);
            result = 1; break;
        }
    }
    if (!result && writing && fdatasync(fd)) { perror("fdatasync"); result = 1; }
    if (!result) printf("{\"device\":\"%s\",\"mode\":\"%s\",\"pages\":%lu,\"bytes\":%lu,\"seed\":%llu,\"passed\":true}\n",
                        argv[1], argv[2], pages, pages * PAGE_BYTES, (unsigned long long)seed);
    close(fd); free(expected); free(actual);
    return result;
}
