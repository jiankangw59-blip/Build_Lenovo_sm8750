/* Finite, offline idle measurement: no polling during cooldown/measurement. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <linux/netlink.h>
#include <sched.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <time.h>
#include <unistd.h>

struct sample {
    int64_t charge_uah, voltage_raw, current_raw, temp_deci_c;
    int64_t usb_online, brightness;
    double boottime;
    char status[32];
};

static void fail(const char *message)
{
    perror(message);
    exit(1);
}

static void text_node(const char *path, char *out, size_t size)
{
    FILE *f = fopen(path, "re");
    if (!f || !fgets(out, size, f))
        fail(path);
    fclose(f);
    out[strcspn(out, "\r\n")] = 0;
}

static int64_t number_node(const char *path)
{
    char text[96], *end;
    text_node(path, text, sizeof(text));
    errno = 0;
    int64_t value = strtoll(text, &end, 10);
    if (errno || !text[0] || *end) {
        errno = EINVAL;
        fail(path);
    }
    return value;
}

static struct sample snapshot(void)
{
    struct sample s;
    struct timespec now;
    if (clock_gettime(CLOCK_BOOTTIME, &now))
        fail("boottime");
    s.boottime = now.tv_sec + now.tv_nsec / 1e9;
    s.charge_uah = number_node("/sys/class/power_supply/battery/charge_counter");
    s.voltage_raw = number_node("/sys/class/power_supply/battery/voltage_now");
    s.current_raw = number_node("/sys/class/power_supply/battery/current_now");
    s.temp_deci_c = number_node("/sys/class/power_supply/battery/temp");
    s.usb_online = number_node("/sys/class/power_supply/usb/online");
    s.brightness = number_node("/sys/class/backlight/panel0-backlight/brightness");
    text_node("/sys/class/power_supply/battery/status", s.status, sizeof(s.status));
    return s;
}

static bool unplugged(const struct sample *s)
{
    return !s->usb_online && !strcmp(s->status, "Discharging");
}

static void wait_alarm(int fd, unsigned seconds)
{
    struct itimerspec timer = {.it_value.tv_sec = seconds};
    uint64_t count;
    if (timerfd_settime(fd, 0, &timer, NULL))
        fail("arm finite alarm");
    ssize_t n;
    do {
        n = read(fd, &count, sizeof(count));
    } while (n < 0 && errno == EINTR);
    if (n != sizeof(count))
        fail("wait finite alarm");
}

static void state(int fd, const char *phase)
{
    if (lseek(fd, 0, SEEK_SET) < 0 || ftruncate(fd, 0) ||
        dprintf(fd, "{\"phase\":\"%s\",\"measurement_complete\":false}\n", phase) < 0 || fsync(fd))
        fail("write measurement state");
}

static void print_sample(int fd, const char *name, const struct sample *s)
{
    if (dprintf(fd,
        "\"%s\":{\"boottime_seconds\":%.6f,\"charge_counter_raw\":%" PRId64
        ",\"voltage_raw\":%" PRId64 ",\"current_raw\":%" PRId64
        ",\"temperature_deci_c\":%" PRId64 ",\"usb_online\":%" PRId64
        ",\"backlight_brightness\":%" PRId64 ",\"battery_status\":\"%s\"}",
        name, s->boottime, s->charge_uah, s->voltage_raw, s->current_raw,
        s->temp_deci_c, s->usb_online, s->brightness, s->status) < 0)
        fail("write sample");
}

int main(int argc, char **argv)
{
    if (getuid() != 0) {
        errno = EPERM;
        fail("requires root");
    }
    int alarm = timerfd_create(CLOCK_BOOTTIME_ALARM, TFD_CLOEXEC);
    if (alarm < 0)
        fail("CLOCK_BOOTTIME_ALARM is required; do not use polling instead");
    struct sample initial = snapshot();
    if (argc == 2 && !strcmp(argv[1], "--check")) {
        dprintf(STDOUT_FILENO, "{\"alarm_timer_available\":true,");
        print_sample(STDOUT_FILENO, "current", &initial);
        dprintf(STDOUT_FILENO, "}\n");
        close(alarm);
        return 0;
    }
    if (argc != 5 || strcmp(argv[1], "--arm")) {
        fprintf(stderr, "usage: %s --check | --arm COOLDOWN_SECONDS MEASURE_SECONDS NEW_RESULT_PATH\n", argv[0]);
        return 2;
    }
    char *end;
    unsigned long cooldown = strtoul(argv[2], &end, 10);
    if (*end || cooldown < 60 || cooldown > 1800)
        return 2;
    unsigned long duration = strtoul(argv[3], &end, 10);
    if (*end || duration < 600 || duration > 3600)
        return 2;
    if (setpriority(PRIO_PROCESS, 0, 19))
        fail("nice 19");
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    for (unsigned i = 0; i < 6; i++)
        CPU_SET(i, &cpus);
    if (sched_setaffinity(0, sizeof(cpus), &cpus))
        fail("CPU0-5 affinity");
    int result = open(argv[4], O_RDWR | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC, 0600);
    if (result < 0)
        fail("create a fresh private result");
    state(result, "waiting_for_physical_unplug");
    /* Subscribe only until unplug. The socket is CLOSED before idle begins. */
    int events = socket(AF_NETLINK, SOCK_DGRAM | SOCK_CLOEXEC, NETLINK_KOBJECT_UEVENT);
    if (events < 0)
        fail("power uevent socket");
    struct sockaddr_nl address = {.nl_family = AF_NETLINK, .nl_groups = 1};
    if (bind(events, (struct sockaddr *)&address, sizeof(address)))
        fail("power uevent bind");
    for (;;) {
        struct sample s = snapshot();
        if (unplugged(&s))
            break;
        char event[8192];
        ssize_t n = recv(events, event, sizeof(event), 0);
        if (n < 0 && errno != EINTR)
            fail("wait for unplug");
    }
    close(events);
    state(result, "unplugged_cooldown");
    wait_alarm(alarm, (unsigned)cooldown);
    struct sample start = snapshot();
    if (!unplugged(&start) || start.brightness) {
        state(result, "invalid_start_requires_unplugged_screen_off");
        close(result);
        return 1;
    }
    state(result, "measuring_offline_no_polling");
    wait_alarm(alarm, (unsigned)duration);
    struct sample finish = snapshot();
    bool endpoints = unplugged(&finish) && !finish.brightness;
    double elapsed = finish.boottime - start.boottime;
    int64_t delta = start.charge_uah - finish.charge_uah;
    bool estimate = endpoints && elapsed >= duration && delta > 0;
    char boot[64];
    text_node("/proc/sys/kernel/random/boot_id", boot, sizeof(boot));
    if (lseek(result, 0, SEEK_SET) < 0 || ftruncate(result, 0))
        fail("rewind final result");
    dprintf(result, "{\"phase\":\"complete\",\"measurement_complete\":true,"
        "\"boot_id\":\"%s\",\"valid_endpoints\":%s,\"positive_charge_delta\":%s,"
        "\"interval_seconds\":%.6f,\"charge_counter_drop_raw\":%" PRId64 ","
        "\"intermediate_screen_or_charger_changes_monitored\":false,"
        "\"current_now_units\":\"OEM raw; not used to calculate average\","
        "\"charge_counter_units_assumption\":\"Linux uAh; OEM calibration not independently verified\",",
        boot, endpoints ? "true" : "false", delta > 0 ? "true" : "false", elapsed, delta);
    if (estimate)
        dprintf(result, "\"gauge_average_current_ma\":%.6f,", delta * 3.6 / elapsed);
    else
        dprintf(result, "\"gauge_average_current_ma\":null,");
    print_sample(result, "start", &start);
    dprintf(result, ",");
    print_sample(result, "finish", &finish);
    dprintf(result, "}\n");
    if (fsync(result))
        fail("sync final result");
    close(result);
    close(alarm);
    return endpoints ? 0 : 1;
}
