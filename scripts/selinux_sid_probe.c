#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <grp.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

/* Parent stays Root to observe the live SID table; only the carrier submits inputs. */
static const char *carrier = "u:r:app_zygote:s0:c512,c768";
static unsigned file_class, process_class;
static char low[32][256], high[32][256];
static const char *transaction_phase;

static int entries(void) {
    char buf[512] = {0};
    int fd = open("/sys/fs/selinux/ss/sidtab_hash_stats", O_RDONLY);
    if (fd < 0) return -1;
    ssize_t n = read(fd, buf, sizeof(buf)-1);
    close(fd);
    char *at = n > 0 ? strstr(buf, "entries:") : NULL;
    return at ? (int)strtol(at+8, NULL, 10) : -1;
}

static unsigned class_index(const char *name) {
    char path[128]; unsigned value = 0;
    snprintf(path, sizeof(path), "/sys/fs/selinux/class/%s/index", name);
    FILE *f = fopen(path, "r");
    if (f) { if (fscanf(f, "%u", &value) != 1) value = 0; fclose(f); }
    return value;
}

static int transaction(const char *node, const char *input) {
    char path[128], reply[4096];
    snprintf(path, sizeof(path), "/sys/fs/selinux/%s", node);
    transaction_phase = "open";
    int fd = open(path, O_RDWR | O_CLOEXEC);
    if (fd < 0) return errno;
    transaction_phase = "write";
    ssize_t n = write(fd, input, strlen(input)+1);
    int error = n < 0 ? errno : 0;
    if (!error) {
        transaction_phase = "read"; errno = 0;
        if (read(fd, reply, sizeof(reply)) <= 0) error = errno ? errno : EIO;
        else transaction_phase = "complete";
    }
    close(fd);
    return error;
}

static int set_current(const char *context) {
    int fd = open("/proc/self/attr/current", O_WRONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    int error = write(fd, context, strlen(context)+1) < 0 ? errno : 0;
    close(fd);
    return error;
}

static int enter_carrier(unsigned uid) {
    char context[256] = {0}, status[4096] = {0};
    if (setgroups(0, NULL) || setgid(uid) || setuid(uid)) return errno;
    int error = set_current(carrier);
    if (error) return error;
    int fd = open("/proc/self/attr/current", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    ssize_t n = read(fd, context, sizeof(context)-1); close(fd);
    context[strcspn(context, "\n")] = 0;
    if (n <= 0 || strcmp(context, carrier) || getuid() != uid || geteuid() != uid) return EPROTO;
    fd = open("/proc/self/status", O_RDONLY | O_CLOEXEC);
    if (fd < 0) return errno;
    n = read(fd, status, sizeof(status)-1); close(fd);
    const char *keys[] = {"CapPrm:", "CapEff:", "CapAmb:"};
    for (unsigned i = 0; i < sizeof(keys)/sizeof(keys[0]); i++) {
        const char *at = n > 0 ? strstr(status, keys[i]) : NULL;
        if (!at || strtoull(at+strlen(keys[i]), NULL, 16)) return EPERM;
    }
    printf("[INFO] carrier uid=%u euid=%u context=%s capabilities=0\n", getuid(), geteuid(), context);
    fflush(stdout);
    return 0;
}

static uint32_t random_step(uint32_t *seed) {
    *seed ^= *seed << 13; *seed ^= *seed >> 17; *seed ^= *seed << 5;
    return *seed;
}

static int compare(const void *a, const void *b) {
    unsigned x = *(const unsigned *)a, y = *(const unsigned *)b;
    return (x > y) - (x < y);
}

static void labels(char output[32][256], unsigned base, uint32_t *seed) {
    for (unsigned i = 0; i < 32; i++) {
        unsigned cats[8];
        for (unsigned j = 0; j < 8; j++) {
            int duplicate;
            do {
                cats[j] = base + random_step(seed) % 512; duplicate = 0;
                for (unsigned k = 0; k < j; k++) duplicate |= cats[k] == cats[j];
            } while (duplicate);
        }
        qsort(cats, 8, sizeof(cats[0]), compare);
        int used = snprintf(output[i], 256, "u:r:app_zygote:s0:");
        for (unsigned j = 0; j < 8; j++)
            used += snprintf(output[i]+used, 256-(size_t)used, "%sc%u", j ? "," : "", cats[j]);
    }
}

static int group(unsigned index) {
    for (unsigned i = 0; i < 32; i++) {
        int error;
        if (index <= 3 || index == 5) {
            const char *label = index == 0 ? carrier : index == 1 ?
                "u:r:single_sidtab_missing_control:s0" : index == 5 ? high[i] : low[i];
            error = transaction("context", label);
            if (index == 1 ? error != EINVAL : error != 0) return error ? error : EPROTO;
        } else if (index == 4 || index == 6) {
            char query[600];
            snprintf(query, sizeof(query), "%s u:object_r:tmpfs:s0 %u", high[i], file_class);
            if ((error = transaction("access", query))) return error;
        } else {
            pid_t child = fork(); int status;
            if (child < 0) return errno;
            if (!child) {
                error = set_current(high[i]);
                _exit(error == 0 || error == EACCES || error == EPERM ? 0 : 1);
            }
            if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) return EPROTO;
        }
    }
    return 0;
}

static int reachability(void) {
    static const char *nodes[] = {"create", "relabel", "member", "user", "validatetrans"};
    int failed = 0;
    for (unsigned i = 0; i < sizeof(nodes)/sizeof(nodes[0]); i++) {
        char query[1024];
        if (i == 3) snprintf(query, sizeof(query), "%s u", carrier);
        else if (i == 4) snprintf(query, sizeof(query), "%s %s %u %s", carrier, carrier, process_class, carrier);
        else snprintf(query, sizeof(query), "%s %s %u", carrier, carrier, process_class);
        int error = transaction(nodes[i], query);
        /* An open denial also prevents reaching SS; retain the exact syscall. */
        int passed = error == EACCES && (!strcmp(transaction_phase, "write") || !strcmp(transaction_phase, "open"));
        printf("%s domain=%s node=%s phase=%s errno=%d (%s)\n", passed ? "[PASS]" : "[FAIL]",
               carrier, nodes[i], transaction_phase, error, error ? strerror(error) : "reachable");
        failed |= !passed;
    }
    return failed;
}

int main(int argc, char **argv) {
    if (argc < 2) return 2;
    unsigned uid = (unsigned)strtoul(argv[1], NULL, 10);
    int reach = argc > 2 && !strcmp(argv[2], "--reachability");
    if (argc > 3) carrier = argv[3];
    file_class = class_index("file"); process_class = class_index("process");
    if (!file_class || !process_class || uid < 10000) return 2;
    if (reach) {
        if (enter_carrier(uid)) return 3;
        return reachability();
    }
    /* Warm fixed labels before measuring any SID growth. */
    if (transaction("context", carrier) || transaction("context", "u:object_r:tmpfs:s0")) return 4;
    uint32_t seed = (uint32_t)time(NULL) ^ ((uint32_t)getpid()<<11) ^ 0x793abca1;
    labels(low, 0, &seed); labels(high, 512, &seed);
    int start[2], done[2];
    if (pipe(start) || pipe(done)) return 5;
    pid_t child = fork();
    if (child < 0) return 5;
    if (!child) {
        close(start[1]); close(done[0]);
        int preparation = enter_carrier(uid);
        if (write(done[1], &preparation, sizeof(preparation)) != sizeof(preparation) || preparation) _exit(3);
        for (unsigned i = 0; i < 9; i++) {
            char command; int error;
            if (read(start[0], &command, 1) != 1) _exit(6);
            error = group(i);
            if (write(done[1], &error, sizeof(error)) != sizeof(error)) _exit(7);
        }
        _exit(0);
    }
    close(start[0]); close(done[1]);
    const int expected[] = {0, 0, 32, 0, 32, 0, 0, 0, 0};
    int failed = 0;
    int preparation = EIO;
    if (read(done[0], &preparation, sizeof(preparation)) != sizeof(preparation) || preparation) {
        fprintf(stderr, "[FAIL] carrier preparation errno=%d\n", preparation);
        close(start[1]); close(done[0]); waitpid(child, NULL, 0); return 3;
    }
    for (unsigned i = 0; i < 9; i++) {
        int before = entries(), after, error = EIO; char command = 'G';
        if (write(start[1], &command, 1) != 1 || read(done[0], &error, sizeof(error)) != sizeof(error)) { failed = 1; break; }
        after = entries(); int passed = !error && before >= 0 && after-before == expected[i];
        printf("%s SID group=%u before=%d after=%d delta=%d expected=%d errno=%d\n",
               passed ? "[PASS]" : "[FAIL]", i, before, after, after-before, expected[i], error);
        fflush(stdout); failed |= !passed;
    }
    close(start[1]); close(done[0]); int status;
    if (waitpid(child, &status, 0) != child || !WIFEXITED(status) || WEXITSTATUS(status)) failed = 1;
    return failed;
}
