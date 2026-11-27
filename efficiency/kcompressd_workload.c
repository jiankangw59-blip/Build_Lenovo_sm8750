/* Own 64 MiB of cold anonymous data; never force pageout or global pressure. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/ioprio.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define PAGE_BYTES 4096
#define PAGES 16384

static void fail(const char *what)
{
    perror(what);
    exit(1);
}

static uint32_t next(uint32_t state)
{
    state ^= state << 13;
    state ^= state >> 17;
    state ^= state << 5;
    return state;
}

static uint32_t pattern(unsigned page, unsigned word, uint32_t random)
{
    switch (page & 3) {
    case 0: return (word & 31) * UINT32_C(2654435761) ^ page;
    case 1: return word < 64 ? random : 0;
    case 2: return random;
    default: return (word & 127) ^ (page * UINT32_C(2246822519));
    }
}

int main(int argc, char **argv)
{
    if (argc != 2 || getuid() || sysconf(_SC_PAGESIZE) != PAGE_BYTES)
        return 2;
    char *end;
    unsigned seconds = strtoul(argv[1], &end, 10);
    if (*end || seconds < 10 || seconds > 120)
        return 2;
    FILE *mem = fopen("/proc/meminfo", "re");
    char line[256];
    unsigned long available = 0;
    if (!mem)
        fail("meminfo");
    while (fgets(line, sizeof(line), mem))
        if (sscanf(line, "MemAvailable: %lu kB", &available) == 1)
            break;
    fclose(mem);
    if (available < 3 * 1024 * 1024) {
        errno = ENOMEM;
        fail("preserve at least 3 GiB available RAM before this bounded test");
    }
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (unsigned cpu = 0; cpu < 6; cpu++)
        CPU_SET(cpu, &mask);
    if (setpriority(PRIO_PROCESS, 0, 19) || sched_setaffinity(0, sizeof(mask), &mask) ||
        syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << 13))
        fail("background test priority");
    int oom = open("/proc/self/oom_score_adj", O_WRONLY | O_CLOEXEC);
    if (oom < 0 || write(oom, "1000\n", 5) != 5)
        fail("test process OOM score");
    close(oom);
    uint32_t *data = mmap(NULL, PAGES * PAGE_BYTES, PROT_READ | PROT_WRITE,
                          MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED || madvise(data, PAGES * PAGE_BYTES, MADV_NOHUGEPAGE))
        fail("private 64 MiB test memory");
    for (unsigned page = 0; page < PAGES; page++) {
        uint32_t state = (page + 1) * UINT32_C(2654435761);
        for (unsigned word = 0; word < PAGE_BYTES / sizeof(*data); word++) {
            state = next(state);
            data[page * (PAGE_BYTES / sizeof(*data)) + word] = pattern(page, word, state);
        }
    }
    /* MADV_COLD allows normal reclaim; MADV_PAGEOUT would bypass kswapd. */
    if (madvise(data, PAGES * PAGE_BYTES, MADV_COLD))
        fail("mark only this process's private pages cold");
    int timer = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC);
    struct itimerspec spec = {.it_value.tv_sec = seconds};
    uint64_t expirations;
    if (timer < 0 || timerfd_settime(timer, 0, &spec, NULL) ||
        read(timer, &expirations, sizeof(expirations)) != sizeof(expirations))
        fail("finite observation interval");
    close(timer);
    uint64_t slots[PAGES];
    unsigned count = 0;
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    if (pm < 0)
        fail("own pagemap");
    for (unsigned page = 0; page < PAGES; page++) {
        uint64_t entry;
        uintptr_t address = (uintptr_t)data + page * PAGE_BYTES;
        off_t offset = (off_t)(address / PAGE_BYTES * sizeof(entry));
        if (pread(pm, &entry, sizeof(entry), offset) != sizeof(entry))
            fail("own PTE");
        if ((entry & (UINT64_C(1) << 62)) && !(entry & (UINT64_C(1) << 63)))
            slots[count++] = (entry & ((UINT64_C(1) << 55) - 1)) >> 5;
    }
    close(pm);
    for (unsigned page = 0; page < PAGES; page++) {
        uint32_t state = (page + 1) * UINT32_C(2654435761);
        volatile uint32_t *actual = data + page * (PAGE_BYTES / sizeof(*data));
        for (unsigned word = 0; word < PAGE_BYTES / sizeof(*data); word++) {
            state = next(state);
            if (actual[word] != pattern(page, word, state)) {
                fprintf(stderr, "readback mismatch page=%u word=%u\n", page, word);
                return 1;
            }
        }
    }
    munmap(data, PAGES * PAGE_BYTES);
    printf("{\"bytes_verified\":%u,\"checksum_verified\":true,\"own_swapped_pages\":%u,"
           "\"observation_seconds\":%u,\"forced_pageout\":false,\"swap_slots\":[",
           PAGES * PAGE_BYTES, count, seconds);
    for (unsigned n = 0; n < count; n++)
        printf("%s%" PRIu64, n ? "," : "", slots[n]);
    printf("]}\n");
    return 0;
}
