/* SPDX-License-Identifier: GPL-2.0-or-later */
#include "lab_compat.h"

#ifdef _WIN32
__declspec(dllimport) void *GetProcessHeap(void);
__declspec(dllimport) void *HeapAlloc(void *, unsigned long, size_t);
__declspec(dllimport) int HeapFree(void *, unsigned long, void *);
static void *system_alloc(size_t n) { return HeapAlloc(GetProcessHeap(), 8, n); }
static void system_free(void *p) { HeapFree(GetProcessHeap(), 0, p); }
#else
void *calloc(size_t, size_t);
void free(void *);
static void *system_alloc(size_t n) { return calloc(1, n); }
static void system_free(void *p) { free(p); }
#endif

void *memcpy(void *to, const void *from, size_t n)
{
    unsigned char *d = to;
    const unsigned char *s = from;
    for (size_t i = 0; i < n; i++) d[i] = s[i];
    return to;
}

void *memset(void *to, int byte, size_t n)
{
    unsigned char *d = to;
    for (size_t i = 0; i < n; i++) d[i] = byte;
    return to;
}

struct allocation {
    struct allocation *next;
    size_t bytes;
    unsigned char guard[48];
};
_Static_assert(sizeof(struct allocation) == 64, "guard alignment");
static struct allocation *allocations;
static unsigned long failures;
static int fail_after = -1;

void lab_set_fail_after(int count) { fail_after = count; }

unsigned long lab_check_allocation_guards(void)
{
    for (struct allocation *a = allocations; a; a = a->next) {
        unsigned char *tail = (unsigned char *)(a + 1) + a->bytes;
        for (unsigned i = 0; i < sizeof(a->guard); i++)
            if (a->guard[i] != 0xa7) failures++;
        for (unsigned i = 0; i < 64; i++)
            if (tail[i] != 0x5d) failures++;
    }
    return failures;
}

unsigned lab_live_allocations(void)
{
    unsigned count = 0;
    for (struct allocation *a = allocations; a; a = a->next) count++;
    return count;
}

void *lab_zalloc(size_t n)
{
    if (fail_after == 0) return NULL;
    if (fail_after > 0) fail_after--;
    if (n > (size_t)-1 - 128) return NULL;
    struct allocation *a = system_alloc(n + 128);
    if (!a) return NULL;
    a->bytes = n;
    a->next = allocations;
    allocations = a;
    memset(a->guard, 0xa7, sizeof(a->guard));
    memset((unsigned char *)(a + 1) + n, 0x5d, 64);
    return a + 1;
}

void lab_free(void *ptr)
{
    if (!ptr) return;
    lab_check_allocation_guards();
    struct allocation *target = (struct allocation *)ptr - 1;
    struct allocation **at = &allocations;
    while (*at && *at != target) at = &(*at)->next;
    if (!*at) { failures++; return; }
    *at = target->next;
    system_free(target);
}
