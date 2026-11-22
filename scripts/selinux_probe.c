#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/mman.h>
#include <unistd.h>
#include <stdlib.h>
#include <grp.h>

struct selinux_status_t {
    uint32_t version;
    uint32_t sequence;
    uint32_t enforcing;
    uint32_t policyload;
    uint32_t deny_unknown;
};

static int test_status_and_abi(uint32_t *out_policyload, uint32_t *out_sequence) {
    long pg = sysconf(_SC_PAGESIZE);
    int fd = open("/sys/fs/selinux/status", O_RDONLY);
    if (fd < 0) {
        printf("[FAIL] open /sys/fs/selinux/status: %s\n", strerror(errno));
        return -1;
    }

    struct selinux_status_t st_read = {0};
    ssize_t n = pread(fd, &st_read, sizeof(st_read), 0);
    if (n != (ssize_t)sizeof(st_read)) {
        printf("[FAIL] read status returned %zd (%s)\n", n, strerror(errno));
        close(fd);
        return -1;
    }

    char dummy[16];
    ssize_t eof_n = pread(fd, dummy, sizeof(dummy), pg);
    printf("[INFO] status read(off=0): ver=%u seq=%u enf=%u pload=%u deny=%u | read(off=%ld)=%zd\n",
           st_read.version, st_read.sequence, st_read.enforcing,
           st_read.policyload, st_read.deny_unknown, pg, eof_n);

    void *base = mmap(NULL, pg, PROT_READ, MAP_PRIVATE, fd, 0);
    if (base == MAP_FAILED) {
        printf("[FAIL] baseline mmap failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    struct selinux_status_t st_mmap;
    memcpy(&st_mmap, base, sizeof(st_mmap));
    printf("[INFO] status mmap(off=0): ver=%u seq=%u enf=%u pload=%u deny=%u\n",
           st_mmap.version, st_mmap.sequence, st_mmap.enforcing,
           st_mmap.policyload, st_mmap.deny_unknown);

    int failed = 0;
    if (st_mmap.version != 1 || st_mmap.enforcing != 1 || (st_mmap.sequence & 1)) {
        printf("[FAIL] status version, enforcing state or sequence is invalid\n");
        failed = 1;
    }
    if (memcmp(&st_read, &st_mmap, sizeof(st_read)) != 0) {
        printf("[FAIL] read() vs mmap() mismatch!\n");
        failed = 1;
    } else {
        printf("[PASS] read() and mmap() status fields match\n");
    }

    /* ABI Check A: size != PAGE_SIZE -> must fail (-EIO) */
    void *p = mmap(NULL, pg * 2, PROT_READ, MAP_SHARED, fd, 0);
    if (p != MAP_FAILED) {
        printf("[FAIL] ABI-A: mmap(2*PAGE_SIZE) unexpectedly GRANTED!\n");
        munmap(p, pg * 2);
        failed = 1;
    } else if (errno == EIO) {
        printf("[PASS] ABI-A: mmap(2*PAGE_SIZE) rejected (errno=%d)\n", errno);
    } else {
        printf("[FAIL] ABI-A: unexpected errno=%d\n", errno);
        failed = 1;
    }

    /* ABI Check B: offset != 0 -> must fail (-EIO) */
    p = mmap(NULL, pg, PROT_READ, MAP_SHARED, fd, pg);
    if (p != MAP_FAILED) {
        printf("[FAIL] ABI-B: mmap(offset=PAGE_SIZE) unexpectedly GRANTED!\n");
        munmap(p, pg);
        failed = 1;
    } else if (errno == EIO) {
        printf("[PASS] ABI-B: mmap(offset=PAGE_SIZE) rejected (errno=%d)\n", errno);
    } else {
        printf("[FAIL] ABI-B: unexpected errno=%d\n", errno);
        failed = 1;
    }

    /* ABI Check C: PROT_WRITE | MAP_PRIVATE -> must fail (-EPERM) */
    p = mmap(NULL, pg, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
    if (p != MAP_FAILED) {
        printf("[FAIL] ABI-C: writable MAP_PRIVATE unexpectedly GRANTED (kernel write risk)!\n");
        munmap(p, pg);
        failed = 1;
    } else if (errno == EPERM) {
        printf("[PASS] ABI-C: writable MAP_PRIVATE rejected (errno=%d)\n", errno);
    } else {
        printf("[FAIL] ABI-C: unexpected errno=%d\n", errno);
        failed = 1;
    }

    /* ABI Check D: mprotect upgrade on baseline -> must fail (-EACCES, VM_MAYWRITE cleared) */
    if (mprotect(base, pg, PROT_READ | PROT_WRITE) == 0) {
        printf("[FAIL] ABI-D: mprotect(PROT_WRITE) unexpectedly GRANTED (VM_MAYWRITE not cleared)!\n");
        failed = 1;
    } else if (errno == EACCES) {
        printf("[PASS] ABI-D: mprotect(PROT_WRITE) rejected (errno=%d)\n", errno);
    } else {
        printf("[FAIL] ABI-D: unexpected errno=%d\n", errno);
        failed = 1;
    }

    munmap(base, pg);
    close(fd);
    *out_policyload = st_mmap.policyload;
    *out_sequence = st_mmap.sequence;
    return failed ? -2 : 0;
}

static int test_access_probe(uint32_t status_pload, uint32_t status_sequence) {
    int fd = open("/sys/fs/selinux/access", O_RDWR);
    if (fd < 0) {
        printf("[WARN] open /sys/fs/selinux/access failed: %s (check SELinux context)\n", strerror(errno));
        return -1;
    }

    const char *req = "u:r:untrusted_app:s0 u:r:untrusted_app:s0 0";
    if (write(fd, req, strlen(req)) < 0) {
        printf("[FAIL] write access failed: %s\n", strerror(errno));
        close(fd);
        return -1;
    }

    char resp[256] = {0};
    ssize_t n = read(fd, resp, sizeof(resp) - 1);
    close(fd);
    if (n <= 0) {
        printf("[FAIL] read access failed: %s\n", strerror(errno));
        return -1;
    }

    unsigned int allowed = 0, decided = 0, auditallow = 0, auditdeny = 0, seqno = 0, flags = 0;
    int cnt = sscanf(resp, "%x %x %x %x %u %x",
                     &allowed, &decided, &auditallow, &auditdeny, &seqno, &flags);
    printf("[INFO] access raw resp: \"%s\" (parsed=%d, seqno=%u)\n", resp, cnt, seqno);

    if (cnt != 6 || allowed != 0 || auditallow != 0 || flags != 0 ||
        decided != 0xffffffffU || auditdeny != 0xffffffffU) {
        printf("[FAIL] unexpected access response shape\n");
        return -1;
    }

    int native_initial = status_sequence == 0 && status_pload == 0 && seqno == 1;
    if (seqno != status_pload && !native_initial) {
        printf("[FAIL] SEQNO SPLIT DETECTED! status.policyload=%u != access.seqno=%u\n",
               status_pload, seqno);
        return -1;
    }

    if (getuid() % 100000 >= 10000 && !native_initial) {
        printf("[FAIL] expected native 6.6 hidden status (sequence=0, policyload=0), access.seqno=1\n");
        return -1;
    }

    if (native_initial)
        printf("[PASS] native 6.6 status sentinel (sequence=0, policyload=0), access.seqno=1\n");
    else
        printf("[PASS] real status.policyload == access.seqno == %u\n", seqno);
    return 0;
}

static int test_context_oracle(void) {
    const char *contexts[] = {
        "u:r:untrusted_app:s0", "u:r:app_zygote:s0",
        "u:r:ksu:s0", "u:object_r:ksu_file:s0",
        "u:r:magisk:s0", "u:object_r:magisk_file:s0",
        "u:object_r:lsposed_file:s0", "u:r:droidspacesd:s0",
        "u:r:msd_app:s0", "u:r:msd_daemon:s0",
        "u:object_r:xposed_data:s0", "u:r:adbroot:s0",
        "u:object_r:xposed_file:s0",
        "u:r:luna_context_oracle_sentinel:s0",
        "u:object_r:luna_context_oracle_sentinel_file:s0"
    };
    int hidden = getuid() % 100000 >= 10000;
    int failed = 0;
    for (unsigned int i = 0; i < sizeof(contexts) / sizeof(contexts[0]); i++) {
        int fd = open("/sys/fs/selinux/context", O_RDWR);
        if (fd < 0) {
            printf("[FAIL] context oracle open: %s\n", strerror(errno));
            return -1;
        }
        errno = 0;
        ssize_t written = write(fd, contexts[i], strlen(contexts[i]) + 1);
        int error = errno;
        int valid = i < 2 || (!hidden && i < 4);
        int optional = !hidden && i >= 4 && i < sizeof(contexts) / sizeof(contexts[0]) - 2;
        int passed = valid ? written >= 0 : (written < 0 && error == EINVAL);
        if (optional) passed = written >= 0 || (written < 0 && error == EINVAL);
        if (passed && written >= 0) {
            char canonical[256] = {0};
            ssize_t n = read(fd, canonical, sizeof(canonical) - 1);
            passed = n > 0 && strcmp(canonical, contexts[i]) == 0;
        }
        close(fd);
        printf("[%s] context oracle %s: write=%zd errno=%d\n",
               passed ? "PASS" : "FAIL", contexts[i], written, error);
        if (!passed) failed = 1;
    }
    return failed ? -1 : 0;
}

static int read_policy_number(const char *class_name, const char *permission, unsigned *out) {
    char path[256];
    if (permission)
        snprintf(path, sizeof(path), "/sys/fs/selinux/class/%s/perms/%s", class_name, permission);
    else
        snprintf(path, sizeof(path), "/sys/fs/selinux/class/%s/index", class_name);
    FILE *file = fopen(path, "r");
    int ok = file && fscanf(file, "%u", out) == 1;
    if (file) fclose(file);
    if (!ok) printf("[FAIL] policy metadata: %s\n", path);
    return ok ? 0 : -1;
}

static int test_policy_rule(const char *label, const char *source, const char *target,
                            const char *class_name, const char *permission, int expected) {
    unsigned cls = 0, perm = 0, last_allowed = 0, last_seqno = 0;
    if (read_policy_number(class_name, NULL, &cls) ||
        read_policy_number(class_name, permission, &perm) || perm < 1 || perm > 32) return -1;
    unsigned mask = UINT32_C(1) << (perm - 1);
    for (int attempt = 0; attempt < 2; attempt++) {
        char request[256], response[256] = {0};
        int length = snprintf(request, sizeof(request), "%s %s %u", source, target, cls);
        int fd = open("/sys/fs/selinux/access", O_RDWR);
        if (fd < 0 || write(fd, request, (size_t)length) != length) {
            printf("[FAIL] policy rule %s query errno=%d\n", label, errno);
            if (fd >= 0) close(fd);
            return -1;
        }
        ssize_t n = read(fd, response, sizeof(response) - 1);
        close(fd);
        unsigned allowed, decided, auditallow, auditdeny, seqno, flags;
        if (n <= 0 || sscanf(response, "%x %x %x %x %u %x", &allowed, &decided,
                             &auditallow, &auditdeny, &seqno, &flags) != 6) {
            printf("[FAIL] policy rule %s malformed response\n", label);
            return -1;
        }
        if ((expected >= 0 && ((allowed & mask) != 0) != expected) ||
            (getuid() % 100000 >= 10000 && seqno != 1) ||
            (attempt && (allowed != last_allowed || seqno != last_seqno))) {
            printf("[FAIL] policy rule %s allowed=%u mask=%08x seqno=%u expected=%d raw=%s\n",
                   label, (allowed & mask) != 0, mask, seqno, expected, response);
            return -1;
        }
        last_allowed = allowed;
        last_seqno = seqno;
    }
    printf("[PASS] policy rule %s allowed=%u mask=%08x seqno=%u expected=%d stable=1\n",
           label, (last_allowed & mask) != 0, mask, last_seqno, expected);
    return 0;
}

static int test_policy_rules(void) {
    int hidden = getuid() % 100000 >= 10000;
    int failed = 0;
    failed |= test_policy_rule("public_self_getattr", "u:r:untrusted_app:s0", "u:r:untrusted_app:s0",
                               "process", "getattr", 1);
    failed |= test_policy_rule("system_server_execmem", "u:r:system_server:s0", "u:r:system_server:s0",
                               "process", "execmem", hidden ? 0 : -1);
    failed |= test_policy_rule("zygote_adb_dir_search", "u:r:zygote:s0", "u:object_r:adb_data_file:s0",
                               "dir", "search", hidden ? 0 : -1);
    return failed ? -1 : 0;
}

static int test_rejected_context_repetitions(unsigned count) {
    if (getuid() % 100000 < 10000) return 2;
    for (unsigned i = 0; i < count; i++) {
        char context[96];
        /* Distinct MLS labels reveal unintended live SID interning. */
        snprintf(context, sizeof(context), "u:r:ksu:s0:c%u", 700 + i);
        int fd = open("/sys/fs/selinux/context", O_RDWR);
        if (fd < 0) return 2;
        errno = 0;
        ssize_t written = write(fd, context, strlen(context) + 1);
        int error = errno;
        close(fd);
        if (written >= 0 || error != EINVAL) {
            printf("[FAIL] rejected context %s write=%zd errno=%d\n", context, written, error);
            return 1;
        }
    }
    printf("[PASS] rejected_context_repetitions=%u all_EINVAL=1\n", count);
    return 0;
}

int main(int argc, char **argv) {
    unsigned repetitions = 0;
    if (argc >= 3 && strcmp(argv[1], "--repeat-context") == 0) {
        char *end = NULL;
        repetitions = (unsigned)strtoul(argv[2], &end, 10);
        if (!end || *end || repetitions < 1 || repetitions > 64) return 2;
        argc -= 2;
        argv += 2;
    }
    if (argc == 3) {
        char *end = NULL;
        unsigned long uid = strtoul(argv[1], &end, 10);
        if (!end || *end || uid > UINT32_MAX || uid % 100000 < 10000 ||
            setgroups(0, NULL) || setgid(uid) || setuid(uid)) {
            fprintf(stderr, "[FAIL] invalid app UID or privilege drop: %s\n", strerror(errno));
            return 2;
        }
        int fd = open("/proc/self/attr/current", O_WRONLY);
        if (fd < 0 || write(fd, argv[2], strlen(argv[2])) != (ssize_t)strlen(argv[2])) {
            fprintf(stderr, "[FAIL] context transition failed: %s\n", strerror(errno));
            if (fd >= 0) close(fd);
            return 2;
        }
        close(fd);
    } else if (argc != 1) {
        fprintf(stderr, "Usage: %s [APP_UID SELINUX_CONTEXT]\n", argv[0]);
        return 2;
    }
    char ctx[128] = "unknown";
    int cfd = open("/proc/self/attr/current", O_RDONLY);
    if (cfd >= 0) {
        ssize_t n = read(cfd, ctx, sizeof(ctx) - 1);
        if (n > 0) ctx[n] = '\0';
        close(cfd);
    }
    ctx[strcspn(ctx, "\n")] = '\0';
    printf("=== SELinux Probe | uid=%u euid=%u ctx=%s ===\n", getuid(), geteuid(), ctx);
    if (argc == 3 && strcmp(ctx, argv[2]) != 0) {
        fprintf(stderr, "[FAIL] process did not enter the requested context\n");
        return 2;
    }
    if (repetitions) return test_rejected_context_repetitions(repetitions);

    uint32_t pload = 0, sequence = 0;
    int st_rc = test_status_and_abi(&pload, &sequence);
    int acc_rc = 0;
    if (st_rc != -1) {
        acc_rc = test_access_probe(pload, sequence);
    }
    int oracle_rc = test_context_oracle();
    int rules_rc = test_policy_rules();
    return (st_rc != 0 || acc_rc < 0 || oracle_rc < 0 || rules_rc < 0) ? 1 : 0;
}
