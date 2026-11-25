/* Only fsync: never read or write raw block contents. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define STATE "/data/adb/ace6-kernel-efficiency"

static int flush_fd(int fd, const char *name)
{
    int ret;
    do { ret = fsync(fd); } while (ret && errno == EINTR);
    if (ret)
        perror(name);
    return ret;
}

static int state_file_allowed(const char *name)
{
    static const char *const names[] = {
        STATE "/native-writeback.daily",
        STATE "/crystal-writeback.physical",
        STATE "/crystal-writeback.pending",
        STATE "/crystal-writeback.last-round",
        STATE "/crystal-writeback.blocked",
    };
    for (unsigned int i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        if (!strcmp(name, names[i]))
            return 1;
    return 0;
}

int main(int argc, char **argv)
{
    struct stat info;
    int state = argc == 3 && !strcmp(argv[1], "--state") &&
                state_file_allowed(argv[2]);
    if (!state && (argc != 2 || strcmp(argv[1], "--block"))) {
        fprintf(stderr, "usage: %s --block | --state OWNED_STATE_FILE\n", argv[0]);
        return 2;
    }
    const char *path = state ? argv[2] : "/dev/block/zram0";
    int directory = -1;
    if (state) {
        directory = open(STATE, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
        if (directory < 0 || fstat(directory, &info) || info.st_uid ||
            (info.st_mode & 0022)) {
            fprintf(stderr, "unsafe controller state directory\n");
            if (directory >= 0)
                close(directory);
            return 2;
        }
    }
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) {
        perror(path);
        if (directory >= 0)
            close(directory);
        return 2;
    }
    int ret = 2;
    if (fstat(fd, &info) ||
        (state ? !S_ISREG(info.st_mode) || info.st_uid || (info.st_mode & 0077)
               : !S_ISBLK(info.st_mode))) {
        fprintf(stderr, "unexpected fsync target type/ownership\n");
    } else if (!flush_fd(fd, path) &&
               (!state || !flush_fd(directory, STATE))) {
        ret = 0;
    }
    if (close(fd) && !ret)
        ret = 2;
    if (directory >= 0 && close(directory) && !ret)
        ret = 2;
    return ret;
}
