/* Blocking screen/PSI events and finite non-wakeup cold-page grace period. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/ioprio.h>
#include <poll.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/inotify.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

#define BACKLIGHT "/sys/class/backlight/panel0-backlight/"

static void fail(const char *what)
{
    perror(what);
    exit(2);
}

static void background_priority(void)
{
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (unsigned i = 0; i < 6; i++)
        CPU_SET(i, &mask);
    if (getuid() || setpriority(PRIO_PROCESS, 0, 19) ||
        sched_setaffinity(0, sizeof(mask), &mask) ||
        syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << 13))
        fail("root/nice19/CPU0-5/I/O-idle policy");
}

static void parent_guard(void)
{
    pid_t parent = getppid();
    if (prctl(PR_SET_PDEATHSIG, SIGTERM) || parent != getppid())
        fail("parent lifetime guard");
}

static int node(int fd)
{
    char data[64], *end;
    if (lseek(fd, 0, SEEK_SET) < 0)
        fail("rewind backlight node");
    ssize_t n = read(fd, data, sizeof(data) - 1);
    if (n <= 0)
        fail("read backlight node");
    data[n] = 0;
    long value = strtol(data, &end, 10);
    if (end == data || (*end && *end != '\n') || value < 0)
        fail("backlight value");
    return (int)value;
}

static void event(const char *name)
{
    if (printf("%s\n", name) < 0 || fflush(stdout))
        fail("event receiver disappeared");
}

static int watch(void)
{
    /* Use the SAME brightness node/inotify mechanism as the installed AppOpt.
     * This vendor driver's actual_brightness incorrectly remains zero awake.
     */
    const char *path = BACKLIGHT "brightness";
    unsigned off_max = 1; /* AppOpt's default also treats dim AOD level 1 as off. */
    const char *configured = getenv("NATIVE_WB_SCREEN_OFF_MAX");
    if (configured) {
        char *end;
        unsigned long value = strtoul(configured, &end, 10);
        if (!configured[0] || *end || value > 7)
            fail("invalid screen-off brightness threshold");
        off_max = (unsigned)value;
    }
    int brightness = open(path, O_RDONLY | O_CLOEXEC);
    if (brightness < 0)
        fail(path);
    struct pollfd fds[2] = {0};
    fds[0].fd = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    fds[0].events = POLLIN;
    if (fds[0].fd < 0 || inotify_add_watch(fds[0].fd, path,
        IN_MODIFY | IN_ATTRIB | IN_CLOSE_WRITE | IN_DELETE_SELF | IN_MOVE_SELF) < 0)
        fail("AppOpt-compatible brightness inotify");
    fds[1].fd = open("/proc/pressure/memory", O_RDWR | O_NONBLOCK | O_CLOEXEC);
    const char trigger[] = "some 200000 2000000";
    if (fds[1].fd < 0 || write(fds[1].fd, trigger, sizeof(trigger)) != sizeof(trigger))
        fail("register blocking memory PSI trigger");
    fds[1].events = POLLPRI;
    int was_off = (unsigned)node(brightness) <= off_max;
    if (was_off)
        event("screen_off");
    for (;;) {
        int rc = poll(fds, 2, -1);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            fail("wait screen/PSI event");
        if (fds[0].revents & (POLLERR | POLLHUP | POLLNVAL))
            fail("brightness inotify source disappeared");
        if (fds[0].revents & POLLIN) {
            unsigned char events[4096];
            ssize_t n = read(fds[0].fd, events, sizeof(events));
            if (n < 0 && errno == EAGAIN)
                continue;
            if (n <= 0)
                fail("read brightness events");
            for (size_t offset = 0; offset + sizeof(struct inotify_event) <= (size_t)n;) {
                struct inotify_event event;
                memcpy(&event, events + offset, sizeof(event));
                if (event.mask & (IN_IGNORED | IN_DELETE_SELF | IN_MOVE_SELF | IN_Q_OVERFLOW))
                    fail("brightness watch invalidated");
                offset += sizeof(event) + event.len;
            }
            int is_off = (unsigned)node(brightness) <= off_max;
            if (is_off != was_off)
                event(is_off ? "screen_off" : "screen_on");
            was_off = is_off;
        }
        if (fds[1].revents & (POLLERR | POLLHUP | POLLNVAL))
            fail("PSI event source disappeared");
        if (fds[1].revents & POLLPRI)
            event("pressure");
    }
}

static int grace(const char *value)
{
    struct stat pipe_stat;
    if (fstat(3, &pipe_stat) || !S_ISFIFO(pipe_stat.st_mode))
        fail("missing inherited controller FIFO on fd3");
    char *end;
    errno = 0;
    int64_t deadline = strtoll(value, &end, 10);
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now))
        fail("boottime");
    if (errno || *end || deadline < now.tv_sec || deadline > now.tv_sec + 3601)
        return 2;
    int timer = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC);
    if (timer < 0)
        fail("finite non-wakeup cold timer");
    struct itimerspec spec = {.it_value.tv_sec = deadline};
    if (timerfd_settime(timer, TFD_TIMER_ABSTIME, &spec, NULL))
        fail("arm cold timer");
    struct pollfd fds[] = {{.fd = 3, .events = POLLIN}, {.fd = timer, .events = POLLIN}};
    char message[64];
    size_t used = 0;
    for (;;) {
        int rc = poll(fds, 2, -1);
        if (rc < 0 && errno == EINTR)
            continue;
        if (rc < 0)
            fail("cold grace wait");
        if (fds[0].revents & (POLLERR | POLLNVAL))
            fail("event pipe failed");
        if (fds[0].revents & POLLIN) {
            char ch;
            ssize_t n = read(3, &ch, 1);
            if (n != 1)
                fail("event pipe closed");
            if (ch == '\n') {
                message[used] = 0;
                if (!strcmp(message, "screen_on")) {
                    close(timer);
                    return 1;
                }
                used = 0;
            } else if (used + 1 < sizeof(message)) {
                message[used++] = ch;
            } else {
                fail("oversized event");
            }
            /* Drain queued screen transitions before accepting the timer. */
            continue;
        }
        if (fds[0].revents & POLLHUP)
            fail("event watcher exited");
        if (fds[1].revents & POLLIN) {
            close(timer);
            return 0;
        }
    }
}

int main(int argc, char **argv)
{
    background_priority();
    if (argc == 3 && !strcmp(argv[1], "--exec")) {
        execl("/system/bin/sh", "sh", argv[2], (char *)NULL);
        fail("exec Scene writeback controller");
    }
    parent_guard();
    if (argc == 2 && !strcmp(argv[1], "--watch")) {
        prctl(PR_SET_NAME, "SceneZramEvent");
        return watch();
    }
    if (argc == 3 && !strcmp(argv[1], "--grace")) {
        prctl(PR_SET_NAME, "SceneZramGrace");
        return grace(argv[2]);
    }
    fprintf(stderr, "usage: %s --watch | --grace BOOTTIME_DEADLINE | --exec SCRIPT\n", argv[0]);
    return 2;
}
