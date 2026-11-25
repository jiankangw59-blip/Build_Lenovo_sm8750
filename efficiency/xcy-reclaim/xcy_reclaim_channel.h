#ifndef XCY_RECLAIM_CHANNEL_H
#define XCY_RECLAIM_CHANNEL_H

#include <linux/miscdevice.h>

#include "xcy_reclaim_reply.h"
#include "xcy_reclaim_runtime.h"

struct xcy_reclaim_channel;
struct xcy_reclaim_member;

enum xcy_reclaim_notice {
	XCY_RECLAIM_NOTICE_TIMEOUT,
	XCY_RECLAIM_NOTICE_IDENTITY,
};

void xcy_reclaim_notify(enum xcy_reclaim_notice notice, char operation);

struct xcy_reclaim_channel_ops {
	int (*open)(struct xcy_reclaim_channel *channel, void **context);
	void (*close)(void *context);
	void (*retire)(void *context, u64 request);
	int (*receive)(struct xcy_reclaim_channel *channel, void *context,
		       const struct xcy_reclaim_reply *reply, xcy_memory_snapshot_t *snapshot);
};

/* The finished policy registers one device; opening it attaches one publisher. */
int xcy_reclaim_channel_register(const struct xcy_reclaim_channel_ops *ops);
struct xcy_reclaim_runtime *xcy_reclaim_channel_runtime(struct xcy_reclaim_channel *channel);
int xcy_reclaim_channel_next(struct xcy_reclaim_channel *channel, u64 request, char operation,
				const struct xcy_reclaim_member *members, size_t count);

#endif
