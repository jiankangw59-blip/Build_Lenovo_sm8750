/* Only this process's own 64 swap slots; no raw block reads or writes. */
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
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/sysmacros.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "owned_logic.h"
#include "pattern.h"

#define MAIN "/sys/block/zram0/"
#define NOTES_MAX 512U

struct snapshot {
    uint64_t bd[3], failed_read, failed_write, limit;
    int limit_enabled;
    uint64_t zms_read, zms_write, zms_failed_read, zms_failed_write, dirty;
};

static const char *backend, *codec, *backing_node, *backing_file;
static int crystal, expected_sddc;
static unsigned char expected_notes[NOTES_MAX];
static size_t expected_notes_bytes;
static struct stat original_backing;
static int have_original_backing;

static void reject(const char *message)
{
    fprintf(stderr, "%s (errno=%d: %s)\n", message, errno, strerror(errno));
    exit(1);
}

static void read_line(const char *path, char *buffer, size_t size)
{
    FILE *file = fopen(path, "re");
    if (!file || !fgets(buffer, size, file))
        reject(path);
    fclose(file);
    buffer[strcspn(buffer, "\r\n")] = '\0';
}

static uint64_t number_node(const char *path)
{
    char buffer[128], *end;
    read_line(path, buffer, sizeof(buffer));
    errno = 0;
    uint64_t value = strtoull(buffer, &end, 10);
    if (errno || !buffer[0] || *end)
        reject("malformed unsigned sysfs number");
    return value;
}

static void property_value(void *cookie, const char *name, const char *value, uint32_t serial)
{
    (void)name;
    (void)serial;
    snprintf(cookie, PROP_VALUE_MAX, "%s", value);
}

static void sole_zram(void)
{
    char line[512], device[256];
    unsigned entries = 0;
    FILE *file = fopen("/proc/swaps", "re");
    if (!file || !fgets(line, sizeof(line), file))
        reject("read swaps");
    while (fgets(line, sizeof(line), file)) {
        if (sscanf(line, "%255s", device) != 1 ||
            (strcmp(device, "/dev/block/zram0") && strcmp(device, "/dev/zram0")))
            reject("expected sole active main zram0");
        entries++;
    }
    if (ferror(file))
        reject("read active swaps");
    fclose(file);
    if (entries != 1)
        reject("expected exactly one active zram0");
}

static void check_identity(void)
{
    char buffer[1024], selected[32], path[128];
    char model[PROP_VALUE_MAX] = {0};
    const prop_info *property = __system_property_find("ro.product.model");
    if (!property)
        reject("model property unavailable");
    __system_property_read_callback(property, property_value, model);
    if (strcmp(model, "PLQ110"))
        reject("device is not PLQ110");
    unsigned char notes[NOTES_MAX];
    FILE *file = fopen("/sys/kernel/notes", "re");
    if (!file)
        reject("read kernel notes");
    size_t count = fread(notes, 1, sizeof(notes), file);
    int extra = fgetc(file);
    if (ferror(file) || count != expected_notes_bytes || extra != EOF ||
        memcmp(notes, expected_notes, count))
        reject("running kernel notes differ from the pinned bytes");
    fclose(file);
    sole_zram();
    if ((!access(MAIN "zms_stat", F_OK)) != crystal)
        reject("ZRAM backend marker differs");
    read_line(MAIN "comp_algorithm", buffer, sizeof(buffer));
    snprintf(selected, sizeof(selected), "[%s]", codec);
    if (!strstr(buffer, selected))
        reject("main codec differs");
    read_line(MAIN "backing_dev", buffer, sizeof(buffer));
    if (strcmp(buffer, backing_node))
        reject("main backing device changed");
    const char *name = strrchr(backing_node, '/');
    if (!name)
        reject("malformed backing node");
    snprintf(path, sizeof(path), "/sys/block/%s/loop/backing_file", name + 1);
    read_line(path, buffer, sizeof(buffer));
    if (strcmp(buffer, backing_file))
        reject("loop backing owner changed");
    struct stat current;
    if (lstat(backing_file, &current) || !S_ISREG(current.st_mode) ||
        current.st_uid != 0 || current.st_gid != 0 ||
        (current.st_mode & 0777) != 0600 || current.st_size < 4096)
        reject("backing file metadata is not the reviewed private file");
    if (!have_original_backing) {
        original_backing = current;
        have_original_backing = 1;
    } else if (current.st_dev != original_backing.st_dev || current.st_ino != original_backing.st_ino ||
               current.st_size != original_backing.st_size)
        reject("backing file identity changed");
    if (crystal) {
        read_line(MAIN "sddc_stat", buffer, sizeof(buffer));
        unsigned enabled;
        if (sscanf(buffer, "enabled: %u", &enabled) != 1 || enabled != (unsigned)expected_sddc)
            reject("SDDC state differs");
    }
}

static void snapshot(struct snapshot *out)
{
    char buffer[256];
    memset(out, 0, sizeof(*out));
    read_line(MAIN "bd_stat", buffer, sizeof(buffer));
    if (sscanf(buffer, "%" SCNu64 " %" SCNu64 " %" SCNu64,
               &out->bd[0], &out->bd[1], &out->bd[2]) != 3)
        reject("malformed bd_stat");
    read_line(MAIN "io_stat", buffer, sizeof(buffer));
    if (sscanf(buffer, "%" SCNu64 " %" SCNu64, &out->failed_read, &out->failed_write) != 2)
        reject("malformed io_stat");
    out->limit_enabled = number_node(MAIN "writeback_limit_enable") != 0;
    out->limit = number_node(MAIN "writeback_limit");
    if (crystal) {
        FILE *file = fopen(MAIN "zms_stat", "re");
        unsigned fields = 0;
        int enabled = 0, ret = -1;
        if (!file)
            reject("read ZMS counters");
        while (fgets(buffer, sizeof(buffer), file)) {
            char name[64];
            int64_t value;
            if (sscanf(buffer, "%63[^:]: %" SCNi64, name, &value) != 2)
                continue;
            if (!strcmp(name, "enabled")) { enabled = (int)value; fields |= 1U; }
            else if (!strcmp(name, "ret")) { ret = (int)value; fields |= 2U; }
            else if (!strcmp(name, "physical_read_pages")) { out->zms_read = value; fields |= 4U; }
            else if (!strcmp(name, "physical_write_pages")) { out->zms_write = value; fields |= 8U; }
            else if (!strcmp(name, "physical_read_failed_pages")) { out->zms_failed_read = value; fields |= 16U; }
            else if (!strcmp(name, "physical_write_failed_pages")) { out->zms_failed_write = value; fields |= 32U; }
            else if (!strcmp(name, "dirty_pages")) { out->dirty = value; fields |= 64U; }
        }
        if (ferror(file) || fields != 127U || enabled != 1 || ret != 0)
            reject("required live ZMS statistics missing or failed");
        fclose(file);
    }
}

static uint64_t own_pagemap(int fd, const void *address)
{
    uint64_t entry;
    off_t offset = (off_t)((uintptr_t)address / PROBE_PAGE_BYTES * sizeof(entry));
    if (pread(fd, &entry, sizeof(entry), offset) != sizeof(entry))
        reject("read this process's pagemap");
    return entry;
}

static void flush_main(void)
{
    check_identity();
    int fd = open("/dev/block/zram0", O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        reject("open main block read-only for sync only");
    struct stat st;
    unsigned ma, mi;
    char identity[64];
    read_line(MAIN "dev", identity, sizeof(identity));
    if (fstat(fd, &st) || !S_ISBLK(st.st_mode) || sscanf(identity, "%u:%u", &ma, &mi) != 2 ||
        major(st.st_rdev) != ma || minor(st.st_rdev) != mi)
        reject("main sync fd identity differs");
    /* No raw read/write, no O_EXCL and no reset on the live swap device. */
    if (fdatasync(fd) || fsync(fd))
        reject("main block fdatasync/fsync after page-index requests");
    close(fd);
}

static void print_snapshot(const struct snapshot *s)
{
    printf("{\"bd_stat\":[%" PRIu64 ",%" PRIu64 ",%" PRIu64 "],"
           "\"failed_read\":%" PRIu64 ",\"failed_write\":%" PRIu64 ","
           "\"writeback_limit_enabled\":%s,\"writeback_limit_logical_slots\":%" PRIu64,
           s->bd[0], s->bd[1], s->bd[2], s->failed_read, s->failed_write,
           s->limit_enabled ? "true" : "false", s->limit);
    if (crystal)
        printf(",\"zms_physical_read_pages\":%" PRIu64 ",\"zms_physical_write_pages\":%" PRIu64
               ",\"zms_failed_read_pages\":%" PRIu64 ",\"zms_failed_write_pages\":%" PRIu64
               ",\"zms_dirty_pages\":%" PRIu64, s->zms_read, s->zms_write,
               s->zms_failed_read, s->zms_failed_write, s->dirty);
    printf("}");
}

int main(int argc, char **argv)
{
    if (argc != 8 || getuid() || sysconf(_SC_PAGESIZE) != PROBE_PAGE_BYTES)
        return 2;
    backend = argv[1]; codec = argv[2]; backing_node = argv[3]; backing_file = argv[4];
    crystal = !strcmp(backend, "crystal");
    if ((!crystal && strcmp(backend, "native")) ||
        (strcmp(codec, "lz4kd") && strcmp(codec, "lz4kds")) ||
        strncmp(backing_node, "/dev/block/loop", 15) ||
        strspn(backing_node + 15, "0123456789") != strlen(backing_node + 15) ||
        !backing_node[15] || strncmp(backing_file, "/data/adb/", 10))
        return 2;
    char *end;
    uint64_t seed = strtoull(argv[6], &end, 0);
    if (!*argv[6] || *end || (strcmp(argv[7], "0") && strcmp(argv[7], "1")))
        return 2;
    expected_sddc = atoi(argv[7]);
    if ((!crystal && expected_sddc) || (!strcmp(codec, "lz4kds") && !expected_sddc))
        return 2;
    size_t hex = strlen(argv[5]);
    if (!hex || hex % 2 || hex / 2 > NOTES_MAX || strspn(argv[5], "0123456789abcdef") != hex)
        return 2;
    expected_notes_bytes = hex / 2;
    for (size_t i = 0; i < expected_notes_bytes; i++) {
        unsigned byte;
        if (sscanf(argv[5] + i * 2, "%2x", &byte) != 1)
            return 2;
        expected_notes[i] = (unsigned char)byte;
    }
    check_identity();
    struct snapshot initial, before_requests, flushed, readback;
    snapshot(&initial);
    if (initial.failed_read || initial.failed_write ||
        (crystal && (initial.zms_failed_read || initial.zms_failed_write)))
        reject("existing failed data I/O counters are not a clean baseline");
    if (initial.limit_enabled && initial.limit < OWNED_SAMPLES)
        reject("existing writeback budget is insufficient; never replenish it");
    uint64_t slots = number_node(MAIN "disksize") / PROBE_PAGE_BYTES;
    cpu_set_t mask;
    CPU_ZERO(&mask);
    for (unsigned cpu = 0; cpu < 6; cpu++)
        CPU_SET(cpu, &mask);
    if (setpriority(PRIO_PROCESS, 0, 19) || sched_setaffinity(0, sizeof(mask), &mask) ||
        syscall(__NR_ioprio_set, IOPRIO_WHO_PROCESS, 0, IOPRIO_CLASS_IDLE << 13))
        reject("own-process background priority");
    unsigned char *data = mmap(NULL, OWNED_BYTES, PROT_READ | PROT_WRITE,
                              MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (data == MAP_FAILED || madvise(data, OWNED_BYTES, MADV_NOHUGEPAGE))
        reject("allocate only this process's 4 MiB payload");
    uint64_t expected_hash = UINT64_C(14695981039346656037);
    for (unsigned page = 0; page < OWNED_PAGES; page++) {
        probe_pattern_pairs(data + page * PROBE_PAGE_BYTES, page, seed);
        expected_hash = probe_hash(expected_hash, data + page * PROBE_PAGE_BYTES, PROBE_PAGE_BYTES);
    }
    int pm = open("/proc/self/pagemap", O_RDONLY | O_CLOEXEC);
    check_identity();
    if (pm < 0 || madvise(data, OWNED_PAGEOUT_BYTES, MADV_PAGEOUT))
        reject("page out only this process's first 64 pages");
    uint64_t entries[OWNED_SAMPLES], offsets[OWNED_SAMPLES];
    unsigned swapped = 0;
    for (unsigned attempt = 0; attempt < 20; attempt++) {
        swapped = 0;
        for (unsigned page = 0; page < OWNED_SAMPLES; page++) {
            entries[page] = own_pagemap(pm, data + page * PROBE_PAGE_BYTES);
            if (owned_valid_swap_entry(entries[page], slots))
                swapped++;
        }
        if (swapped == OWNED_SAMPLES)
            break;
        usleep(20000);
    }
    if (swapped != OWNED_SAMPLES)
        reject("all 64 selected owned pages must enter swap");
    for (unsigned page = 0; page < OWNED_SAMPLES; page++) {
        offsets[page] = owned_swap_offset(entries[page]);
        if ((entries[page] & 31) != (entries[0] & 31))
            reject("selected pages belong to different swap types");
        for (unsigned prior = 0; prior < page; prior++)
            if (offsets[prior] == offsets[page])
                reject("unexpected shared swap slot");
    }
    snapshot(&before_requests);
    if (before_requests.limit_enabled && before_requests.limit < OWNED_SAMPLES)
        reject("available budget fell below this bounded request");
    for (unsigned page = 0; page < OWNED_SAMPLES; page++) {
        check_identity();
        if (own_pagemap(pm, data + page * PROBE_PAGE_BYTES) != entries[page])
            reject("owned mapping changed before page-index request");
        if (number_node(MAIN "writeback_limit_enable") && !number_node(MAIN "writeback_limit"))
            reject("writeback budget exhausted; never refill it");
        char request[128];
        int bytes = snprintf(request, sizeof(request), "page_index=%" PRIu64 "\n", offsets[page]);
        int wb = open(MAIN "writeback", O_WRONLY | O_CLOEXEC);
        if (wb < 0 || write(wb, request, (size_t)bytes) != bytes)
            reject("write back this process's verified swap slot");
        close(wb);
    }
    for (unsigned attempt = 0; attempt < 3; attempt++) {
        flush_main();
        snapshot(&flushed);
        if (!crystal || flushed.dirty == 0)
            break;
        usleep(10000);
    }
    if (crystal && flushed.dirty)
        reject("ZMS remained dirty after the bounded explicit flush attempts");
    unsigned char expected[PROBE_PAGE_BYTES];
    uint64_t actual_hash = UINT64_C(14695981039346656037);
    for (unsigned page = 0; page < OWNED_PAGES; page++) {
        probe_pattern_pairs(expected, page, seed);
        volatile unsigned char *actual = data + page * PROBE_PAGE_BYTES;
        for (unsigned byte = 0; byte < PROBE_PAGE_BYTES; byte++) {
            unsigned char value = actual[byte];
            if (value != expected[byte]) {
                fprintf(stderr, "owned readback mismatch page=%u byte=%u\n", page, byte);
                return 1;
            }
            actual_hash ^= value;
            actual_hash *= UINT64_C(1099511628211);
        }
    }
    snapshot(&readback);
    check_identity();
    if (flushed.bd[2] <= before_requests.bd[2] || readback.bd[1] <= flushed.bd[1] ||
        readback.failed_read || readback.failed_write || expected_hash != actual_hash ||
        flushed.limit_enabled != before_requests.limit_enabled ||
        (flushed.limit_enabled && flushed.limit > before_requests.limit) ||
        (crystal && (flushed.zms_write <= before_requests.zms_write ||
         readback.zms_read <= flushed.zms_read || readback.zms_failed_read || readback.zms_failed_write)))
        reject("data or actual backend I/O criteria failed; no exact 64-page physical assumption");
    close(pm);
    munmap(data, OWNED_BYTES);
    printf("{\"passed\":true,\"backend\":\"%s\",\"codec\":\"%s\","
           "\"pattern_schema\":\"%s\",\"payload_bytes\":%u,\"bytes_verified\":%u,"
           "\"madv_pageout_bytes\":%u,\"own_swap_slots_verified\":%u,"
           "\"logical_page_index_requests_accepted\":%u,\"seed\":%" PRIu64 ","
           "\"fnv1a64\":\"%016" PRIx64 "\",\"explicit_post_writeback_block_sync\":true,"
           "\"raw_main_block_read_or_write_performed\":false,\"budget_modified_by_probe\":false,"
           "\"kcompressd_path_proven\":false,\"all_requested_slots_backed_proven\":false,"
           "\"global_counters_may_include_concurrent_io\":true,\"initial\":",
           backend, codec, PROBE_PAIRS_SCHEMA, OWNED_BYTES, OWNED_BYTES, OWNED_PAGEOUT_BYTES,
           OWNED_SAMPLES, OWNED_SAMPLES, seed, actual_hash);
    print_snapshot(&initial);
    printf(",\"before_requests\":"); print_snapshot(&before_requests);
    printf(",\"after_flush_before_faultin\":"); print_snapshot(&flushed);
    printf(",\"after_faultin\":"); print_snapshot(&readback);
    printf(",\"bd_write_pages_delta\":%" PRIu64 ",\"bd_read_pages_delta\":%" PRIu64,
           flushed.bd[2] - before_requests.bd[2], readback.bd[1] - flushed.bd[1]);
    if (before_requests.limit_enabled)
        printf(",\"budget_logical_slots_consumed_observed\":%" PRIu64,
               before_requests.limit - flushed.limit);
    else
        printf(",\"budget_logical_slots_consumed_observed\":null");
    printf("}\n");
    return 0;
}
