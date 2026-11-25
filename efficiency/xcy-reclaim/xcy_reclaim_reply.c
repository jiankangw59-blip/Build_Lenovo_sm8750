#include <linux/errno.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>

#include "xcy_reclaim_identity.h"
#include "xcy_reclaim_reply.h"

int xcy_reclaim_read_reply(const struct xcy_reclaim_reply *expected,
			   const char __user *body, size_t length, xcy_memory_snapshot_t *snapshot)
{
	u64 now = ktime_to_ms(ktime_get_boottime());
	xcy_memory_snapshot_t accepted = {0};
	char *copy;
	int error = -ESTALE;

	if (!expected->request || now >= expected->expires_ms || now < expected->published_ms ||
	    (expected->operation != 'I' && expected->operation != 'P'))
		return -ESTALE;
	if (!length || length > XCY_MEMORY_MAX_WIRE_BYTES)
		return -EMSGSIZE;
	copy = vmemdup_user(body, length);
	if (IS_ERR(copy))
		return PTR_ERR(copy);

	if (!xcy_memory_snapshot_parse(copy, length, expected->nonce, expected->epoch,
				       now, now - expected->published_ms,
				       xcy_reclaim_validate_identity, NULL, &accepted))
		goto out;
	if (accepted.wire_version != 4 || ktime_to_ms(ktime_get_boottime()) >= expected->expires_ms)
		goto out;
	/* An initial or final AMS inventory is metadata, not a PSS measurement. */
	if (expected->operation == 'I') {
		size_t i;

		for (i = 0; i < accepted.count; ++i)
			if (accepted.items[i].flags & XCY_MEMORY_PSS_VALID)
				goto out;
	}
	*snapshot = accepted;
	xcy_snapshot_free(copy);
	return 0;
out:
	xcy_memory_snapshot_free(&accepted);
	xcy_snapshot_free(copy);
	return error;
}
