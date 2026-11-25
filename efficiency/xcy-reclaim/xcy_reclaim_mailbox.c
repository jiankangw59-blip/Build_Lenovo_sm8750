#include <linux/errno.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>

#include "xcy_reclaim_mailbox.h"

int xcy_reclaim_mailbox_init(struct xcy_reclaim_mailbox *mailbox)
{
	mutex_init(&mailbox->lock);
	init_waitqueue_head(&mailbox->wait);
	mailbox->request = xcy_snapshot_calloc(XCY_RECLAIM_REQUEST_BYTES, 1);
	return mailbox->request ? 0 : -ENOMEM;
}

void xcy_reclaim_mailbox_close(struct xcy_reclaim_mailbox *mailbox)
{
	mutex_lock(&mailbox->lock);
	WRITE_ONCE(mailbox->closed, true);
	WRITE_ONCE(mailbox->length, 0);
	mutex_unlock(&mailbox->lock);
	wake_up_interruptible_all(&mailbox->wait);
}

/* Call after the publisher and the producer have both stopped. */
void xcy_reclaim_mailbox_destroy(struct xcy_reclaim_mailbox *mailbox)
{
	xcy_snapshot_free(mailbox->request);
}

static size_t xcy_format_request(char *target, size_t capacity, char operation,
			 const struct xcy_reclaim_member *members, size_t count,
				 const char *nonce, const char *epoch)
{
	int length = snprintf(target, capacity, "XCY_MEM_REQ_V4 %s %s %c %zu\n",
			      nonce, epoch, operation, count);
	size_t used, i;

	if (length < 0 || length >= capacity)
		return 0;
	used = length;
	for (i = 0; i < count; ++i) {
		const struct xcy_reclaim_member *member = &members[i];

		length = snprintf(target + used, capacity - used, "%u %u %llu %u\n",
				  member->pid, member->uid, (unsigned long long)member->starttime,
				  member->kernel_uid);
		if (length < 0 || length >= capacity - used)
			return 0;
		used += length;
	}
	length = snprintf(target + used, capacity - used, "END %s\n", nonce);
	return length < 0 || length >= capacity - used ? 0 : used + length;
}

int xcy_reclaim_mailbox_publish(struct xcy_reclaim_mailbox *mailbox, char operation,
				const struct xcy_reclaim_member *members, size_t count,
				const char *nonce, const char *epoch)
{
	size_t length;
	int error = 0;

	if (!nonce || !epoch || !*nonce || !*epoch || strnlen(nonce, 65) > 64 ||
	    strnlen(epoch, 65) > 64 || count > 512 ||
	    (operation == 'P' ? (!count || !members) : (operation != 'I' || count)))
		return -EINVAL;
	mutex_lock(&mailbox->lock);
	if (mailbox->closed) {
		error = -EPIPE;
		goto out;
	}
	length = xcy_format_request(mailbox->request, XCY_RECLAIM_REQUEST_BYTES,
				    operation, members, count, nonce, epoch);
	WRITE_ONCE(mailbox->length, length);
	if (!length)
		error = -EOVERFLOW;
out:
	mutex_unlock(&mailbox->lock);
	if (!error)
		wake_up_interruptible(&mailbox->wait);
	return error;
}

ssize_t xcy_reclaim_mailbox_read(struct xcy_reclaim_mailbox *mailbox,
				 char __user *target, size_t count, bool nonblock)
{
	ssize_t result;

	if (!count)
		return 0;
	for (;;) {
		mutex_lock(&mailbox->lock);
		if (mailbox->closed) {
			result = 0;
			break;
		}
		if (mailbox->length) {
			result = mailbox->length;
			if (count < result)
				result = -EMSGSIZE;
			else if (copy_to_user(target, mailbox->request, result))
				result = -EFAULT;
			else
				WRITE_ONCE(mailbox->length, 0);
			break;
		}
		mutex_unlock(&mailbox->lock);
		if (nonblock)
			return -EAGAIN;
		result = wait_event_interruptible(mailbox->wait,
				READ_ONCE(mailbox->closed) || READ_ONCE(mailbox->length));
		if (result)
			return result;
	}
	mutex_unlock(&mailbox->lock);
	return result;
}
