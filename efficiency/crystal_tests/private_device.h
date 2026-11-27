#ifndef ACE6_PRIVATE_DEVICE_H
#define ACE6_PRIVATE_DEVICE_H

#include <errno.h>
#include <fcntl.h>
#include <linux/fs.h>
#include <linux/ioprio.h>
#include <sched.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include "pattern.h"
#include "private_logic.h"

static void private_fail(const char *why)
{
    fprintf(stderr, "%s (errno=%d)\n", why, errno);
    exit(1);
}

static void private_not_swap(dev_t device)
{
    char line[2048], path[1024];
    FILE *file = fopen("/proc/swaps", "re");
    if (!file)
        private_fail("read active swaps");
    while (fgets(line, sizeof(line), file)) {
        struct stat st;
        if (sscanf(line, "%1023s", path) == 1 && !stat(path, &st) &&
            S_ISBLK(st.st_mode) && st.st_rdev == device)
            private_fail("never open an active swap device, including aliases");
    }
    if (ferror(file))
        private_fail("active swap list read failed");
    fclose(file);
}

static int private_open(const char *path, int writing)
{
    unsigned index = private_zram_index(path);
    if (!index || getuid() || sysconf(_SC_PAGESIZE) != PROBE_PAGE_BYTES)
        private_fail("require root and a private zram index greater than zero");
    struct stat before, opened;
    if (lstat(path, &before) || !S_ISBLK(before.st_mode))
        private_fail("require a direct block node, not a symlink");
    private_not_swap(before.st_rdev);
    int fd = open(path, (writing ? O_RDWR : O_RDONLY) |
                  O_DIRECT | O_CLOEXEC | O_NOFOLLOW | O_EXCL);
    if (fd < 0 || fstat(fd, &opened) || opened.st_rdev != before.st_rdev || !S_ISBLK(opened.st_mode))
        private_fail("open exclusive private block device");
    private_not_swap(opened.st_rdev);
    char sysfs[128];
    snprintf(sysfs, sizeof(sysfs), "/sys/block/zram%u/dev", index);
    FILE *file = fopen(sysfs, "re");
    unsigned ma, mi;
    if (!file || fscanf(file, "%u:%u", &ma, &mi) != 2 ||
        major(opened.st_rdev) != ma || minor(opened.st_rdev) != mi)
        private_fail("private sysfs/block identity mismatch");
    fclose(file);
    uint64_t size;
    if (ioctl(fd, BLKGETSIZE64, &size) || size != PROBE_BYTES)
        private_fail("require exactly 32 MiB private capacity");
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (unsigned cpu = 0; cpu < 6; cpu++)
        CPU_SET(cpu, &mask);
    if (setpriority(PRIO_PROCESS, 0, 19) || sched_setaffinity(0, sizeof(mask), &mask) ||
        syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << 13))
        private_fail("private probe background priority");
    return fd;
}

static unsigned char *private_buffer(void)
{
    unsigned char *out = NULL;
    if (posix_memalign((void **)&out, PROBE_PAGE_BYTES, PROBE_PAGE_BYTES))
        private_fail("allocate aligned 4 KiB buffer");
    return out;
}

static int private_io(int fd, unsigned page, unsigned char *buffer, int write_page)
{
    ssize_t done;
    do {
        done = write_page ? pwrite(fd, buffer, PROBE_PAGE_BYTES, (off_t)page * PROBE_PAGE_BYTES)
                          : pread(fd, buffer, PROBE_PAGE_BYTES, (off_t)page * PROBE_PAGE_BYTES);
    } while (done < 0 && errno == EINTR);
    return done == PROBE_PAGE_BYTES;
}

#endif
