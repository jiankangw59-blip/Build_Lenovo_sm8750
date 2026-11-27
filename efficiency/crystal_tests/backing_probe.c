#define _GNU_SOURCE
/* Only ioctl/sync the exact newly owned private loop/file. No raw data I/O. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <linux/ioprio.h>
#include <linux/loop.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <sys/syscall.h>
#include <unistd.h>

#include "private_logic.h"
#include "backing_logic.h"

#define BACKING_BYTES (UINT64_C(128) * 1024 * 1024)
#define PRIVATE_BYTES (UINT64_C(32) * 1024 * 1024)

static void fail(const char *why)
{
    fprintf(stderr, "%s (errno=%d)\n", why, errno);
    exit(1);
}

static uint64_t number(const char *text)
{
    char *end;
    if (!text[0] || text[0] < '0' || text[0] > '9')
        fail("unsigned decimal identity required");
    errno = 0;
    uint64_t value = strtoull(text, &end, 10);
    if (errno || *end)
        fail("invalid decimal identity");
    return value;
}

static void not_swap(const struct stat *a, const struct stat *b,
                     const struct stat *owned)
{
    char line[2048], path[1024];
    FILE *file = fopen("/proc/swaps", "re");
    if (!file || !fgets(line, sizeof(line), file))
        fail("read active swap list");
    while (fgets(line, sizeof(line), file)) {
        struct stat st;
        if (sscanf(line, "%1023s", path) != 1 || stat(path, &st))
            fail("resolve every active swap before any ioctl/sync");
        if ((S_ISBLK(st.st_mode) && (st.st_rdev == a->st_rdev || st.st_rdev == b->st_rdev)) ||
            (S_ISREG(st.st_mode) && st.st_dev == owned->st_dev && st.st_ino == owned->st_ino))
            fail("private block/file became active swap");
    }
    if (ferror(file))
        fail("active swap read error");
    fclose(file);
}

static void read_text(const char *path, char *out, size_t size)
{
    FILE *file = fopen(path, "re");
    if (!file || !fgets(out, (int)size, file))
        fail("read private ownership sysfs");
    if (ferror(file) || !strchr(out, '\n'))
        fail("incomplete private ownership sysfs");
    fclose(file);
    out[strcspn(out, "\n")] = 0;
}

static unsigned resident_pages(int fd)
{
    unsigned count = 0;
    unsigned char *vec = calloc(BACKING_BYTES / 4096, 1);
    void *mapping = mmap(NULL, BACKING_BYTES, PROT_NONE, MAP_SHARED, fd, 0);
    if (!vec || mapping == MAP_FAILED || mincore(mapping, BACKING_BYTES, vec))
        fail("observe only owned backing page cache without faulting data");
    for (unsigned i = 0; i < BACKING_BYTES / 4096; i++)
        count += !!(vec[i] & 1);
    if (munmap(mapping, BACKING_BYTES))
        fail("unmap backing cache observation");
    free(vec);
    return count;
}

static void ownership(int zfd, int lfd, int ffd, unsigned index,
                      const char *loop, const char *backing,
                      uint64_t device, uint64_t inode, int pending, struct loop_info64 *info)
{
    struct stat zst, lst, fst, path_st;
    uint64_t capacity;
    char path[128], text[1024];
    unsigned ma, mi;
    if (fstat(zfd, &zst) || !S_ISBLK(zst.st_mode) ||
        fstat(lfd, &lst) || !S_ISBLK(lst.st_mode) ||
        fstat(ffd, &fst) || !S_ISREG(fst.st_mode) ||
        fst.st_uid || fst.st_gid || (fst.st_mode & 07777) != 0600 || fst.st_nlink != 1 ||
        (uint64_t)fst.st_size != BACKING_BYTES || (uint64_t)fst.st_dev != device ||
        (uint64_t)fst.st_ino != inode ||
        lstat(backing, &path_st) || !S_ISREG(path_st.st_mode) ||
        path_st.st_dev != fst.st_dev || path_st.st_ino != fst.st_ino)
        fail("exact owned regular-file/block identity required");
    not_swap(&zst, &lst, &fst);
    snprintf(path, sizeof(path), "/sys/block/zram%u/dev", index);
    read_text(path, text, sizeof(text));
    if (sscanf(text, "%u:%u", &ma, &mi) != 2 ||
        major(zst.st_rdev) != ma || minor(zst.st_rdev) != mi ||
        ioctl(zfd, BLKGETSIZE64, &capacity) || capacity != (pending ? 0 : PRIVATE_BYTES))
        fail("private zram device/capacity changed");
    snprintf(path, sizeof(path), "/sys/block/zram%u/backing_dev", index);
    read_text(path, text, sizeof(text));
    if (pending ? (strcmp(text, "none") && text[0]) : strcmp(text, loop))
        fail("private zram backing changed");
    snprintf(path, sizeof(path), "/sys/block/%s/dev", strrchr(loop, '/') + 1);
    read_text(path, text, sizeof(text));
    if (sscanf(text, "%u:%u", &ma, &mi) != 2 || major(lst.st_rdev) != ma || minor(lst.st_rdev) != mi)
        fail("loop block identity mismatch");
    if (ioctl(lfd, LOOP_GET_STATUS64, info) || info->lo_device != (uint64_t)fst.st_dev ||
        info->lo_inode != (uint64_t)fst.st_ino || info->lo_offset || info->lo_sizelimit ||
        (info->lo_flags & LO_FLAGS_READ_ONLY))
        fail("loop does not name the entire writable owned file");
    snprintf(path, sizeof(path), "/sys/block/%s/loop/backing_file", strrchr(loop, '/') + 1);
    read_text(path, text, sizeof(text));
    if (strcmp(text, backing))
        fail("loop backing path mismatch");
}

int main(int argc, char **argv)
{
    if (argc != 7 || (strcmp(argv[1], "enable") && strcmp(argv[1], "check") && strcmp(argv[1], "sync")))
        fail("usage: backing-probe enable|check|sync /dev/block/zramN /dev/block/loopN backing dev inode");
    unsigned index = private_zram_index(argv[2]);
    const char *prefix = "/dev/block/loop";
    if (!index || getuid() || sysconf(_SC_PAGESIZE) != 4096 ||
        strncmp(argv[3], prefix, strlen(prefix)) || !argv[3][strlen(prefix)])
        fail("root, 4KiB pages and private canonical block paths required");
    for (const char *p = argv[3] + strlen(prefix); *p; p++)
        if (*p < '0' || *p > '9')
            fail("invalid loop index");
    if (!private_backing_path(argv[4]))
        fail("only a dedicated private test backing path is accepted");
    uint64_t device = number(argv[5]), inode = number(argv[6]);
    /* O_EXCL zram claim prevents simultaneous reset/replacement during this command. */
    int zfd = open(argv[2], O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_EXCL);
    int lfd = open(argv[3], O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    int ffd = open(argv[4], (!strcmp(argv[1], "sync") ? O_RDWR : O_RDONLY) | O_CLOEXEC | O_NOFOLLOW);
    if (zfd < 0 || lfd < 0 || ffd < 0)
        fail("open only the exact private zram/loop/backing");
    struct loop_info64 info;
    int pending = !strcmp(argv[1], "enable");
    /* Enabling/changing geometry is accepted only before zram initialization/binding. */
    ownership(zfd, lfd, ffd, index, argv[3], argv[4], device, inode, pending, &info);
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (unsigned cpu = 0; cpu < 6; cpu++)
        CPU_SET(cpu, &mask);
    if (setpriority(PRIO_PROCESS, 0, 19) || sched_setaffinity(0, sizeof(mask), &mask) ||
        syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << 13))
        fail("owned-backing helper background priority");
    int logical_before = 0, logical_after = 0;
    if (ioctl(lfd, BLKSSZGET, &logical_before))
        fail("read owned loop logical block size");
    /* Default losetup uses 512; a 4KiB backing device rejects DIO at that size. */
    if (!strcmp(argv[1], "enable") && logical_before != 4096 &&
        ioctl(lfd, LOOP_SET_BLOCK_SIZE, 4096UL))
        fail("set only the unused owned loop to aligned 4KiB blocks");
    if (ioctl(lfd, BLKSSZGET, &logical_after) || logical_after != 4096)
        fail("owned loop must keep 4KiB logical blocks");
    if (!strcmp(argv[1], "enable") && ioctl(lfd, LOOP_SET_DIRECT_IO, 1UL))
        fail("LOOP_SET_DIRECT_IO unsupported; no storage cold-read conclusion");
    ownership(zfd, lfd, ffd, index, argv[3], argv[4], device, inode, pending, &info);
    char path[128], text[32];
    snprintf(path, sizeof(path), "/sys/block/%s/loop/dio", strrchr(argv[3], '/') + 1);
    read_text(path, text, sizeof(text));
    if (!(info.lo_flags & LO_FLAGS_DIRECT_IO) || strcmp(text, "1"))
        fail("loop direct I/O is not enabled in both status/sysfs");
    if (!strcmp(argv[1], "sync") && (fdatasync(ffd) || fsync(ffd)))
        fail("owned backing fdatasync/fsync failed");
    if (!strcmp(argv[1], "sync")) {
        int error = posix_fadvise(ffd, 0, BACKING_BYTES, POSIX_FADV_DONTNEED);
        if (error) {
            errno = error;
            fail("evict only the synced owned backing file cache");
        }
    }
    /* check never evicts: after-read residency must be observed as it is. */
    unsigned resident = resident_pages(ffd);
    ownership(zfd, lfd, ffd, index, argv[3], argv[4], device, inode, pending, &info);
    read_text(path, text, sizeof(text));
    if (!(info.lo_flags & LO_FLAGS_DIRECT_IO) || strcmp(text, "1"))
        fail("loop direct I/O changed while syncing");
    printf("{\"passed\":%s,\"mode\":\"%s\",\"loop_status_direct_io\":true,"
           "\"sysfs_dio\":1,\"backing_device\":%" PRIu64 ",\"backing_inode\":%" PRIu64 ","
           "\"backing_bytes\":%" PRIu64 ",\"fdatasync\":%s,\"fsync\":%s,"
           "\"loop_logical_block_bytes_before\":%d,\"loop_logical_block_bytes\":%d,"
           "\"cache_eviction_requested\":%s,\"resident_pages\":%u,"
           "\"cache_observation_faulted_data\":false,\"raw_block_data_io_performed\":false}\n",
           resident && !pending ? "false" : "true", argv[1], device, inode,
           BACKING_BYTES, !strcmp(argv[1], "sync") ? "true" : "false",
           !strcmp(argv[1], "sync") ? "true" : "false",
           logical_before, logical_after, !strcmp(argv[1], "sync") ? "true" : "false", resident);
    close(ffd); close(lfd); close(zfd);
    if (resident && !pending)
        fprintf(stderr, "backing file cache is resident; storage cold read is not proven (not a data-corruption verdict)\n");
    return resident && !pending ? 1 : 0;
}
