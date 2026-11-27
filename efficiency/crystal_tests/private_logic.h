#ifndef ACE6_PRIVATE_LOGIC_H
#define ACE6_PRIVATE_LOGIC_H

/* Pure parser, also exercised as real compiled C by the host tests. */
static inline unsigned private_zram_index(const char *path)
{
    const char prefix[] = "/dev/block/zram";
    unsigned i = 0, number = 0;
    for (; prefix[i]; i++)
        if (!path[i] || path[i] != prefix[i])
            return 0;
    if (path[i] < '1' || path[i] > '9')
        return 0;
    for (; path[i]; i++) {
        if (path[i] < '0' || path[i] > '9')
            return 0;
        number = number * 10 + (unsigned)(path[i] - '0');
        if (number >= 1024)
            return 0;
    }
    return number;
}

#endif
