#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/ioprio.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "pattern.h"

static void die(const char *why)
{
    perror(why);
    exit(1);
}

static void reject_active_swap(dev_t device)
{
    FILE *swaps = fopen("/proc/swaps", "re");
    char line[2048], path[1024];
    if (!swaps)
        die("read active swaps");
    while (fgets(line, sizeof(line), swaps)) {
        struct stat st;
        if (sscanf(line, "%1023s", path) == 1 && !stat(path, &st) &&
            S_ISBLK(st.st_mode) && st.st_rdev == device) {
            fprintf(stderr, "Refuse an active swap device, including aliases\n");
            exit(1);
        }
    }
    if (ferror(swaps))
        die("read active swap list");
    fclose(swaps);
}

int main(int argc, char **argv)
{
    const char *prefix = "/dev/block/zram";
    if ((argc != 4 && argc != 5) || getuid() || sysconf(_SC_PAGESIZE) != PROBE_PAGE_BYTES ||
        strncmp(argv[1], prefix, strlen(prefix)) ||
        (strcmp(argv[2], "write") && strcmp(argv[2], "verify") && strcmp(argv[2], "flush")))
        return 2;
    char *end;
    const char *number = argv[1] + strlen(prefix);
    unsigned long index = strtoul(number, &end, 10);
    if (!*number || *number == '0' || *end || index == 0 || index >= 1024)
        return 2;
    uint64_t seed = strtoull(argv[3], &end, 0);
    if (!*argv[3] || *end)
        return 2;
    const char *pattern_name = argc == 5 ? argv[4] : "mixed";
    if (strcmp(pattern_name, "mixed") && strcmp(pattern_name, "sddc-pairs"))
        return 2;
    int pairs = !strcmp(pattern_name, "sddc-pairs");
    int writing = !strcmp(argv[2], "write");
    int flush = !strcmp(argv[2], "flush");
    struct stat before;
    if (lstat(argv[1], &before) || !S_ISBLK(before.st_mode)) {
        fprintf(stderr, "Expected a direct block node, not a symlink\n");
        return 1;
    }
    reject_active_swap(before.st_rdev);
    int fd = open(argv[1], (writing || flush ? O_RDWR : O_RDONLY) |
                  O_DIRECT | O_CLOEXEC | O_NOFOLLOW | O_EXCL);
    if (fd < 0)
        die("open exclusive private block device");
    struct stat actual;
    if (fstat(fd, &actual) || actual.st_rdev != before.st_rdev || !S_ISBLK(actual.st_mode))
        die("block identity changed");
    reject_active_swap(actual.st_rdev);
    char sysfs[128];
    snprintf(sysfs, sizeof(sysfs), "/sys/block/zram%lu/dev", index);
    FILE *identity = fopen(sysfs, "re");
    unsigned major_id, minor_id;
    if (!identity || fscanf(identity, "%u:%u", &major_id, &minor_id) != 2 ||
        major(actual.st_rdev) != major_id || minor(actual.st_rdev) != minor_id)
        die("zram sysfs/block identity mismatch");
    fclose(identity);
    uint64_t bytes = 0;
    if (ioctl(fd, BLKGETSIZE64, &bytes) || bytes != PROBE_BYTES)
        die("expected exactly 32 MiB private ZRAM");
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (unsigned cpu = 0; cpu < 6; cpu++)
        CPU_SET(cpu, &mask);
    if (setpriority(PRIO_PROCESS, 0, 19) || sched_setaffinity(0, sizeof(mask), &mask) ||
        syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << 13))
        die("probe background priority/affinity");
    if (flush) {
        /* Called AFTER sysfs writeback; the fill-time sync is insufficient. */
        if (fdatasync(fd) || fsync(fd))
            die("post-writeback block fdatasync/fsync");
        printf("{\"passed\":true,\"mode\":\"flush\",\"fdatasync\":true,\"fsync\":true}\n");
        close(fd);
        return 0;
    }
    unsigned char *expected = NULL, *data = NULL;
    if (posix_memalign((void **)&expected, PROBE_PAGE_BYTES, PROBE_PAGE_BYTES) ||
        posix_memalign((void **)&data, PROBE_PAGE_BYTES, PROBE_PAGE_BYTES))
        die("aligned test buffers");
    uint64_t hash = UINT64_C(14695981039346656037);
    for (unsigned page = 0; page < PROBE_PAGES; page++) {
        if (pairs)
            probe_pattern_pairs(expected, page, seed);
        else
            probe_pattern(expected, page, seed);
        ssize_t done;
        do {
            done = writing ? pwrite(fd, expected, PROBE_PAGE_BYTES, (off_t)page * PROBE_PAGE_BYTES)
                           : pread(fd, data, PROBE_PAGE_BYTES, (off_t)page * PROBE_PAGE_BYTES);
        } while (done < 0 && errno == EINTR);
        if (done != PROBE_PAGE_BYTES)
            die("direct 4 KiB block I/O");
        if (!writing && memcmp(expected, data, PROBE_PAGE_BYTES)) {
            fprintf(stderr, "known-pattern mismatch page=%u seed=%" PRIu64 "\n", page, seed);
            return 1;
        }
        hash = probe_hash(hash, writing ? expected : data, PROBE_PAGE_BYTES);
    }
    if (writing && fdatasync(fd))
        die("fill-time fdatasync");
    printf("{\"passed\":true,\"mode\":\"%s\",\"pattern_schema\":\"%s\","
           "\"pages\":%u,\"bytes\":%u,\"seed\":%" PRIu64 ",\"fnv1a64\":\"%016" PRIx64 "\"}\n",
           argv[2], pairs ? PROBE_PAIRS_SCHEMA : PROBE_SCHEMA, PROBE_PAGES, PROBE_BYTES, seed, hash);
    free(expected);
    free(data);
    close(fd);
    return 0;
}
