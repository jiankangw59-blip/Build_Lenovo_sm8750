#define _GNU_SOURCE
#include <inttypes.h>
#include <pthread.h>
#include <stdatomic.h>
#include <string.h>
#include <sys/timerfd.h>

#include "lifecycle_pattern.h"
#include "private_device.h"

struct concurrent {
    int fd;
    uint64_t old_seed, new_seed;
    atomic_int stop, failed;
    unsigned char versions[PROBE_PAGES];
};

struct writer {
    struct concurrent *shared;
    unsigned parity;
    uint64_t writes;
};

struct reader {
    struct concurrent *shared;
    uint64_t reads;
};

static void *write_pages(void *opaque)
{
    struct writer *writer = opaque;
    struct concurrent *state = writer->shared;
    unsigned char *data = private_buffer();
    while (!atomic_load(&state->stop))
        for (unsigned page = writer->parity; page < PROBE_PAGES && !atomic_load(&state->stop); page += 2) {
            unsigned next = state->versions[page] ^ 1U;
            probe_pattern_pairs(data, page, next ? state->new_seed : state->old_seed);
            if (!private_io(state->fd, page, data, 1)) {
                atomic_store(&state->failed, 1);
                atomic_store(&state->stop, 1);
                break;
            }
            /* Disjoint parity ownership; read only after pthread_join. */
            state->versions[page] = (unsigned char)next;
            writer->writes++;
        }
    free(data);
    return NULL;
}

static void *read_pages(void *opaque)
{
    struct reader *reader = opaque;
    struct concurrent *state = reader->shared;
    unsigned char *actual = private_buffer(), *a = private_buffer(), *b = private_buffer();
    while (!atomic_load(&state->stop))
        for (unsigned page = 0; page < PROBE_PAGES && !atomic_load(&state->stop); page++) {
            probe_pattern_pairs(a, page, state->old_seed);
            probe_pattern_pairs(b, page, state->new_seed);
            if (!private_io(state->fd, page, actual, 0) || !lifecycle_nonzero(actual) ||
                (!lifecycle_matches(actual, a) && !lifecycle_matches(actual, b))) {
                fprintf(stderr, "concurrent page is neither complete A nor B: page=%u\n", page);
                atomic_store(&state->failed, 1);
                atomic_store(&state->stop, 1);
                break;
            }
            reader->reads++;
        }
    free(actual); free(a); free(b);
    return NULL;
}

static void concurrent_test(int fd, uint64_t old_seed, uint64_t new_seed, unsigned seconds)
{
    struct concurrent state = {.fd = fd, .old_seed = old_seed, .new_seed = new_seed};
    atomic_init(&state.stop, 0);
    atomic_init(&state.failed, 0);
    struct writer writers[2] = {{.shared = &state, .parity = 0}, {.shared = &state, .parity = 1}};
    struct reader reader = {.shared = &state};
    pthread_t threads[3];
    unsigned created = 0;
    int code = pthread_create(&threads[created], NULL, write_pages, &writers[0]);
    if (!code) { created++; code = pthread_create(&threads[created], NULL, write_pages, &writers[1]); }
    if (!code) { created++; code = pthread_create(&threads[created], NULL, read_pages, &reader); }
    if (!code)
        created++;
    if (code) {
        atomic_store(&state.stop, 1);
        for (unsigned i = 0; i < created; i++)
            pthread_join(threads[i], NULL);
        errno = code;
        private_fail("create bounded private A/B worker threads");
    }
    int timer = timerfd_create(CLOCK_BOOTTIME, TFD_CLOEXEC);
    struct itimerspec spec = {.it_value.tv_sec = seconds};
    uint64_t expirations;
    if (timer < 0 || timerfd_settime(timer, 0, &spec, NULL) ||
        read(timer, &expirations, sizeof(expirations)) != sizeof(expirations))
        private_fail("bounded concurrency timer");
    close(timer);
    atomic_store(&state.stop, 1);
    for (unsigned i = 0; i < created; i++)
        if (pthread_join(threads[i], NULL))
            private_fail("join bounded workers");
    if (atomic_load(&state.failed) || writers[0].writes < 64 || writers[1].writes < 64 || reader.reads < 64)
        private_fail("concurrent data failure or insufficient overlapping observations");
    unsigned char *actual = private_buffer(), *expected = private_buffer();
    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned b_pages = 0;
    for (unsigned page = 0; page < PROBE_PAGES; page++) {
        probe_pattern_pairs(expected, page, state.versions[page] ? new_seed : old_seed);
        if (!private_io(fd, page, actual, 0) || !lifecycle_nonzero(actual) || !lifecycle_matches(actual, expected)) {
            fprintf(stderr, "final committed-version mismatch page=%u version=%u\n", page, state.versions[page]);
            private_fail("post-join final per-page version verification");
        }
        hash = probe_hash(hash, actual, PROBE_PAGE_BYTES);
        b_pages += state.versions[page] != 0;
    }
    free(actual); free(expected);
    printf("{\"passed\":true,\"mode\":\"concurrent\",\"seconds\":%u,"
           "\"writer_even_pages\":%" PRIu64 ",\"writer_odd_pages\":%" PRIu64 ","
           "\"reads_accepting_only_complete_a_or_b\":%" PRIu64 ","
           "\"bytes_verified_after_join\":%u,\"final_b_pages\":%u,"
           "\"final_fnv1a64\":\"%016" PRIx64 "\",\"discard_performed\":false,"
           "\"all_races_proven\":false}\n", seconds, writers[0].writes, writers[1].writes,
           reader.reads, PROBE_BYTES, b_pages, hash);
}

int main(int argc, char **argv)
{
    if (argc != 5 && argc != 6)
        return 2;
    const char *mode = argv[2];
    char *end;
    uint64_t old_seed = strtoull(argv[3], &end, 0);
    if (!argv[3][0] || *end)
        return 2;
    uint64_t new_seed = strtoull(argv[4], &end, 0);
    if (!argv[4][0] || *end || old_seed == new_seed ||
        !lifecycle_seed_safe(old_seed) || !lifecycle_seed_safe(new_seed))
        return 2;
    int fill = !strcmp(mode, "fill");
    int overwrite = !strcmp(mode, "overwrite-sources");
    int discard = !strcmp(mode, "discard-sources");
    int flush = !strcmp(mode, "flush");
    int concurrent = !strcmp(mode, "concurrent");
    int state = !strcmp(mode, "verify-filled") ? 0 : !strcmp(mode, "verify-overwrite") ? 1 :
                !strcmp(mode, "verify-discard") ? 2 : -1;
    if (!fill && !overwrite && !discard && !flush && !concurrent && state < 0)
        return 2;
    unsigned seconds = 0;
    if (concurrent) {
        if (argc != 6)
            return 2;
        seconds = (unsigned)strtoul(argv[5], &end, 10);
        if (*end || seconds < 2 || seconds > 5)
            return 2;
    } else if (argc != 5)
        return 2;
    int fd = private_open(argv[1], fill || overwrite || discard || flush || concurrent);
    if (concurrent) {
        concurrent_test(fd, old_seed, new_seed, seconds);
        close(fd);
        return 0;
    }
    if (flush) {
        if (fdatasync(fd) || fsync(fd))
            private_fail("explicit post-writeback private-device sync");
        printf("{\"passed\":true,\"mode\":\"flush\",\"fdatasync\":true,\"fsync\":true}\n");
        close(fd);
        return 0;
    }
    if (discard) {
        for (unsigned page = 0; page < PROBE_PAGES; page += LIFECYCLE_GROUP_PAGES) {
            uint64_t range[2] = {(uint64_t)page * PROBE_PAGE_BYTES,
                                  LIFECYCLE_SOURCE_PAGES * PROBE_PAGE_BYTES};
            if (ioctl(fd, BLKDISCARD, range))
                private_fail("discard only each private family's first pair");
        }
        printf("{\"passed\":true,\"mode\":\"discard-sources\",\"pages_discarded\":%u}\n",
               PROBE_PAGES / LIFECYCLE_GROUP_PAGES * LIFECYCLE_SOURCE_PAGES);
        close(fd);
        return 0;
    }
    unsigned char *expected = private_buffer(), *actual = private_buffer();
    uint64_t hash = UINT64_C(14695981039346656037);
    unsigned processed = 0;
    for (unsigned page = 0; page < PROBE_PAGES; page++) {
        if (overwrite && page % LIFECYCLE_GROUP_PAGES >= LIFECYCLE_SOURCE_PAGES)
            continue;
        lifecycle_expected(expected, page, old_seed, new_seed, overwrite ? 1U : state >= 0 ? (unsigned)state : 0U);
        if (!private_io(fd, page, fill || overwrite ? expected : actual, fill || overwrite))
            private_fail("private lifecycle 4 KiB I/O");
        if (!fill && !overwrite && !lifecycle_matches(actual, expected)) {
            fprintf(stderr, "lifecycle known-pattern mismatch mode=%s page=%u\n", mode, page);
            private_fail("sequential reference-lifetime data verification");
        }
        hash = probe_hash(hash, fill || overwrite ? expected : actual, PROBE_PAGE_BYTES);
        processed++;
    }
    if ((fill || overwrite) && fdatasync(fd))
        private_fail("private fill/overwrite fdatasync");
    free(expected); free(actual); close(fd);
    printf("{\"passed\":true,\"mode\":\"%s\",\"schema\":\"%s\","
           "\"old_seed\":%" PRIu64 ",\"new_seed\":%" PRIu64 ","
           "\"pages_processed\":%u,\"bytes_processed\":%u,\"fnv1a64\":\"%016" PRIx64 "\"}\n",
           mode, LIFECYCLE_SCHEMA, old_seed, new_seed, processed, processed * PROBE_PAGE_BYTES, hash);
    return 0;
}
