#include <linux/errno.h>
#include <linux/err.h>
#include <linux/ktime.h>
#include <linux/uaccess.h>

#include "xcy_reclaim_identity.h"
#include "xcy_reclaim_reply.h"

static int xcy_reply_accept_metadata(const xcy_memory_process_t *process, void *context)
{
	/* Check live identities after parsing so a departed member does not
	 * discard results for unrelated app groups. */
	return 1;
}

int xcy_reclaim_read_reply(struct xcy_reclaim_reply *expected,
			   const char __user *body, size_t length, xcy_memory_snapshot_t *snapshot)
{
	u64 now = ktime_to_ms(ktime_get_boottime());
	xcy_memory_snapshot_t accepted = {0};
	char *copy;
	size_t i;
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
				       xcy_reply_accept_metadata, NULL, &accepted) ||
	    accepted.wire_version != 4)
		goto out;
	for (i = 0; i < accepted.count; ++i) {
		xcy_memory_process_t *row = &accepted.items[i];

		if (expected->operation == 'I' && (row->flags & XCY_MEMORY_PSS_VALID))
			goto out;
		if ((row->flags & XCY_MEMORY_IDENTITY_VALID) && !xcy_reclaim_validate_identity(row, NULL)) {
			xcy_memory_discard_identity(row);
			expected->identity_changed = true;
		}
	}
	if (ktime_to_ms(ktime_get_boottime()) >= expected->expires_ms)
		goto out;
	*snapshot = accepted;
	xcy_snapshot_free(copy);
	return 0;
out:
	xcy_memory_snapshot_free(&accepted);
	xcy_snapshot_free(copy);
	return error;
}
