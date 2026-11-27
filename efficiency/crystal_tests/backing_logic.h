#ifndef ACE6_BACKING_LOGIC_H
#define ACE6_BACKING_LOGIC_H

/* This actual admission parser is also compiled and called by host tests. */
static inline int private_backing_path(const char *path)
{
    const char prefix[] = "/data/adb/ace6";
    unsigned i = 0;
    for (; prefix[i]; i++)
        if (!path[i] || path[i] != prefix[i])
            return 0;
    if (!((path[i] == 'c' && path[i + 1] == 't') ||
          (path[i] == 'l' && path[i + 1] == 'c')) || path[i + 2] != '-')
        return 0;
    i += 3;
    for (unsigned n = 0; n < 20; n++, i++)
        if (!((path[i] >= '0' && path[i] <= '9') || (path[i] >= 'a' && path[i] <= 'f')))
            return 0;
    const char suffix[] = "/round-";
    for (unsigned n = 0; suffix[n]; n++, i++)
        if (!path[i] || path[i] != suffix[n])
            return 0;
    if (path[i] < '0' || path[i] > '9')
        return 0;
    i++;
    const char extension[] = ".img";
    for (unsigned n = 0; extension[n]; n++, i++)
        if (!path[i] || path[i] != extension[n])
            return 0;
    return !path[i];
}

#endif
