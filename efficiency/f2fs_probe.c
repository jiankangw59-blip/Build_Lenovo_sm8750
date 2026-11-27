/* Read-only compression and GC scheduler observations for the loop-FS probe. */
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/f2fs.h>
#include <linux/ioprio.h>
#include <sched.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <unistd.h>

int main(int argc, char **argv)
{
    if (argc != 3)
        return 2;
    if (!strcmp(argv[1], "compression")) {
        uint64_t counter = 0;
        struct stat status;
        int fd = open(argv[2], O_RDONLY | O_CLOEXEC);
        if (fd < 0 || fstat(fd, &status) || ioctl(fd, F2FS_IOC_GET_COMPRESS_BLOCKS, &counter)) {
            perror("F2FS compression observation");
            return 1;
        }
        close(fd);
        printf("{\"size\":%" PRIu64 ",\"allocated_blocks\":%" PRIu64
               ",\"compression_counter\":%" PRIu64 "}\n",
               (uint64_t)status.st_size, (uint64_t)status.st_blocks, counter);
        return counter ? 0 : 1;
    }
    if (!strcmp(argv[1], "scheduler")) {
        char *end;
        long pid = strtol(argv[2], &end, 10);
        if (!*argv[2] || *end || pid <= 0)
            return 2;
        int policy = sched_getscheduler((pid_t)pid);
        if (policy < 0) {
            perror("GC scheduling policy");
            return 1;
        }
        int ioprio = (int)syscall(__NR_ioprio_get, 1, (int)pid);
        if (ioprio < 0) {
            perror("GC I/O priority");
            return 1;
        }
        errno = 0;
        int nice = getpriority(PRIO_PROCESS, (id_t)pid);
        if (errno) {
            perror("GC scheduler observation");
            return 1;
        }
        printf("{\"pid\":%ld,\"policy\":%d,\"nice\":%d,\"io_class\":%d,\"io_level\":%d}\n",
               pid, policy, nice, ioprio >> 13, ioprio & 8191);
        return 0;
    }
    return 2;
}
