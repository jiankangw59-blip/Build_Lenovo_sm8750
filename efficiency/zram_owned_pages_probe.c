/* Write back only this process's own swap slots on the sole active ZRAM. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>

#define PAGES 1024
#define SAMPLES 64
#define PAGE_BYTES 4096

static void reject(const char *message)
{
    fprintf(stderr, "%s: %s\n", message, strerror(errno));
    exit(1);
}

static void read_node(const char *path, char *buffer, size_t size)
{
    FILE *file = fopen(path, "r");
    if (!file || !fgets(buffer, size, file))
        reject(path);
    fclose(file);
}

static void sole_zram(void)
{
    char line[512], device[256];
    unsigned int entries = 0;
    FILE *file = fopen("/proc/swaps", "r");
    if (!file || !fgets(line, sizeof(line), file))
        reject("read swaps");
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%255s", device) != 1)
            reject("malformed swaps");
        if (strcmp(device, "/dev/block/zram0") && strcmp(device, "/dev/zram0")) {
            errno = EBUSY;
            reject("another swap device is active");
        }
        entries++;
    }
    fclose(file);
    if (entries != 1) {
        errno = EBUSY;
        reject("expected one active zram0");
    }
}

static void counters(uint64_t values[3])
{
    char buffer[128];
    read_node("/sys/block/zram0/bd_stat", buffer, sizeof(buffer));
    if (sscanf(buffer, "%" SCNu64 " %" SCNu64 " %" SCNu64,
               &values[0], &values[1], &values[2]) != 3) {
        errno = EINVAL;
        reject("malformed ZRAM backing counters");
    }
}

static uint64_t pagemap(int fd, const void *address)
{
    uint64_t entry;
    off_t offset = (off_t)((uintptr_t)address / PAGE_BYTES * sizeof(entry));
    if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry))
        reject("read own pagemap");
    return entry;
}

static uint32_t next_word(uint32_t state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

int main(void)
{
    char buffer[512];
    uint64_t initial[3], written[3], readback[3], entries[SAMPLES], offsets[SAMPLES];
    if (getuid() != 0 || sysconf(_SC_PAGESIZE) != PAGE_BYTES) {
        errno = EPERM;
        reject("requires root and a 4K-page kernel");
    }
    if (!access("/sys/block/zram0/hybridswap_loop_device", F_OK) ||
        !access("/sys/block/zram0/hybridswap_core_enable", F_OK) ||
        !access("/sys/module/oplus_bsp_hybridswap_zram", F_OK)) {
        errno = EBUSY;
        reject("vendor HybridSwap is outside this native-ZRAM probe");
    }
    sole_zram();
    read_node("/sys/block/zram0/comp_algorithm", buffer, sizeof(buffer));
    if (!strstr(buffer, "[lz4kd]")) {
        errno = EINVAL;
        reject("pure lz4kd is not selected");
    }
    read_node("/sys/block/zram0/backing_dev", buffer, sizeof(buffer));
    if (!buffer[0] || !strncmp(buffer, "none", 4)) {
        errno = ENODEV;
        reject("no backing device configured");
    }
    read_node("/sys/block/zram0/writeback_limit_enable", buffer, sizeof(buffer));
    if (strtoul(buffer, NULL, 10)) {
        read_node("/sys/block/zram0/writeback_limit", buffer, sizeof(buffer));
        if (strtoull(buffer, NULL, 10) < SAMPLES) {
            errno = EDQUOT;
            reject("remaining writeback budget is insufficient");
        }
    }
    read_node("/sys/block/zram0/disksize", buffer, sizeof(buffer));
    uint64_t slots = strtoull(buffer, NULL, 10) / PAGE_BYTES;
    counters(initial);
    uint32_t *data = mmap(NULL, PAGES * PAGE_BYTES, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED)
        reject("allocate test memory");
    if (madvise(data, PAGES * PAGE_BYTES, MADV_NOHUGEPAGE))
        reject("disable THP for test memory");
    for (unsigned int page = 0; page < PAGES; page++) {
        uint32_t state = (page + 1) * UINT32_C(2654435761);
        for (unsigned int word = 0; word < PAGE_BYTES / sizeof(*data); word++) {
            state = next_word(state);
            data[page * (PAGE_BYTES / sizeof(*data)) + word] = state;
        }
    }
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm < 0 || madvise(data, PAGES * PAGE_BYTES, MADV_PAGEOUT))
        reject("page out test memory");
    unsigned int sampled = 0;
    for (unsigned int attempt = 0; attempt < 20; attempt++) {
        sampled = 0;
        for (unsigned int sample = 0; sample < SAMPLES; sample++) {
            void *address = (char *)data + sample * (PAGES / SAMPLES) * PAGE_BYTES;
            uint64_t entry = pagemap(pm, address);
            if ((entry & (UINT64_C(1) << 62)) && !(entry & (UINT64_C(1) << 63)))
                sampled++;
            entries[sample] = entry;
        }
        if (sampled == SAMPLES)
            break;
        usleep(20000);
    }
    if (sampled != SAMPLES) {
        errno = EAGAIN;
        reject("test pages did not all enter swap");
    }
    for (unsigned int sample = 0; sample < SAMPLES; sample++) {
        offsets[sample] = (entries[sample] & ((UINT64_C(1) << 55) - 1)) >> 5;
        if (!offsets[sample] || offsets[sample] >= slots ||
            (entries[sample] & 31) != (entries[0] & 31)) {
            errno = EINVAL;
            reject("invalid owned swap slot");
        }
        for (unsigned int previous = 0; previous < sample; previous++)
            if (offsets[previous] == offsets[sample]) {
                errno = EINVAL;
                reject("unexpected shared swap slot");
            }
    }
    for (unsigned int sample = 0; sample < SAMPLES; sample++) {
        sole_zram();
        void *address = (char *)data + sample * (PAGES / SAMPLES) * PAGE_BYTES;
        if (pagemap(pm, address) != entries[sample]) {
            errno = EAGAIN;
            reject("owned swap entry changed before writeback");
        }
        int wb = open("/sys/block/zram0/writeback", O_WRONLY | O_CLOEXEC);
        int length = snprintf(buffer, sizeof(buffer), "page_index=%" PRIu64 "\n", offsets[sample]);
        if (wb < 0 || write(wb, buffer, (size_t)length) != length)
            reject("write back owned page");
        close(wb);
    }
    counters(written);
    if (written[2] < initial[2] + SAMPLES) {
        errno = EIO;
        reject("backing writes did not cover the selected test pages");
    }
    for (unsigned int page = 0; page < PAGES; page++) {
        uint32_t state = (page + 1) * UINT32_C(2654435761);
        volatile uint32_t *actual = data + page * (PAGE_BYTES / sizeof(*data));
        for (unsigned int word = 0; word < PAGE_BYTES / sizeof(*data); word++) {
            state = next_word(state);
            if (actual[word] != state) {
                fprintf(stderr, "readback mismatch: page=%u word=%u\n", page, word);
                return 1;
            }
        }
    }
    counters(readback);
    if (readback[1] < written[1] + SAMPLES) {
        errno = EIO;
        reject("readback did not reach the backing device for every test page");
    }
    close(pm);
    munmap(data, PAGES * PAGE_BYTES);
    printf("{\"passed\":true,\"device\":\"zram0\",\"owned_pages_written_back\":%u,"
           "\"bytes_verified\":%u,\"backing_write_delta\":%" PRIu64 ","
           "\"backing_read_delta\":%" PRIu64 "}\n",
           SAMPLES, PAGES * PAGE_BYTES, written[2] - initial[2], readback[1] - written[1]);
    return 0;
}
