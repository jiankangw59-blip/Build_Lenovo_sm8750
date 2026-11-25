#ifndef XCY_RECLAIM_REPLY_H
#define XCY_RECLAIM_REPLY_H

#include "xcy_reclaim_snapshot.h"

struct xcy_reclaim_reply {
	u64 request;
	u64 published_ms;
	u64 expires_ms;
	char nonce[XCY_MEMORY_TOKEN_BYTES];
	char epoch[XCY_MEMORY_TOKEN_BYTES];
	char operation;
	char diagnostic_stage;
	bool identity_changed;
};

/* Take a stable copy of the outstanding challenge before entering the parser.
 * The caller rechecks that challenge before accepting the parsed snapshot. */
int xcy_reclaim_read_reply(struct xcy_reclaim_reply *expected,
			   const char __user *body, size_t length, xcy_memory_snapshot_t *snapshot);

#endif
