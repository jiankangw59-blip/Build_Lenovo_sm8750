#ifndef XCY_RECLAIM_PORT_H
#define XCY_RECLAIM_PORT_H

#ifdef __KERNEL__
#include <linux/gfp.h>
#include <linux/limits.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/types.h>

#define XCY_RECLAIM_UINT32_MAX U32_MAX
#define XCY_RECLAIM_UINT64_MAX U64_MAX
#define XCY_RECLAIM_INT32_MAX S32_MAX
#define XCY_RECLAIM_INT32_MIN S32_MIN
#define XCY_RECLAIM_INT64_MAX S64_MAX
#define XCY_RECLAIM_U64_C(value) value##ULL

/* Call in worker context. Failed allocation leaves the old snapshot intact. */
static inline void *xcy_snapshot_calloc(size_t count, size_t size)
{
	return kvcalloc(count, size, GFP_KERNEL | __GFP_NOWARN);
}

static inline void xcy_snapshot_free(void *ptr)
{
	kvfree(ptr);
}
#else
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define XCY_RECLAIM_UINT32_MAX UINT32_MAX
#define XCY_RECLAIM_UINT64_MAX UINT64_MAX
#define XCY_RECLAIM_INT32_MAX INT32_MAX
#define XCY_RECLAIM_INT32_MIN INT32_MIN
#define XCY_RECLAIM_INT64_MAX INT64_MAX
#define XCY_RECLAIM_U64_C(value) UINT64_C(value)

static inline void *xcy_snapshot_calloc(size_t count, size_t size)
{
	return calloc(count, size);
}

static inline void xcy_snapshot_free(void *ptr)
{
	free(ptr);
}
#endif

#endif
