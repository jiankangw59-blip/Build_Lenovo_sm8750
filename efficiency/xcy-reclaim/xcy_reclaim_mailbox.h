#ifndef XCY_RECLAIM_MAILBOX_H
#define XCY_RECLAIM_MAILBOX_H

#include <linux/mutex.h>
#include <linux/wait.h>

#include "xcy_reclaim_plan.h"

#define XCY_RECLAIM_REQUEST_BYTES 32768U

struct xcy_reclaim_mailbox {
	struct mutex lock;
	wait_queue_head_t wait;
	char *request;
	size_t length;
	bool closed;
};

int xcy_reclaim_mailbox_init(struct xcy_reclaim_mailbox *mailbox);
void xcy_reclaim_mailbox_close(struct xcy_reclaim_mailbox *mailbox);
void xcy_reclaim_mailbox_cancel(struct xcy_reclaim_mailbox *mailbox);
void xcy_reclaim_mailbox_destroy(struct xcy_reclaim_mailbox *mailbox);
int xcy_reclaim_mailbox_publish(struct xcy_reclaim_mailbox *mailbox, char operation,
				const struct xcy_reclaim_member *members, size_t count,
				const char *nonce, const char *epoch);
ssize_t xcy_reclaim_mailbox_read(struct xcy_reclaim_mailbox *mailbox,
				 char __user *target, size_t count, bool nonblock);

#endif
