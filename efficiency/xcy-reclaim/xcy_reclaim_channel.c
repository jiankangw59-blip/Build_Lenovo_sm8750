#include <linux/atomic.h>
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/errno.h>
#include <linux/fs.h>
#include <linux/ktime.h>
#include <linux/random.h>
#include <linux/security.h>
#include <linux/slab.h>
#include <linux/user_namespace.h>

#include "xcy_reclaim_channel.h"
#include "xcy_reclaim_mailbox.h"

struct xcy_reclaim_channel {
	struct xcy_reclaim_runtime runtime;
	struct xcy_reclaim_mailbox mailbox;
	struct mutex challenge_lock;
	struct mutex reply_lock;
	struct work_struct retire_work;
	atomic64_t retired_request;
	struct xcy_reclaim_reply expected;
	u64 nonce_sequence;
	u64 boot_session;
	u32 publisher_secid;
	bool root_publisher;
	void *context;
};

static const struct xcy_reclaim_channel_ops *channel_ops;
static atomic_t channel_open = ATOMIC_INIT(0);

static bool xcy_check_publisher_label(u32 *secid)
{
	static const char label[] = "u:r:system_server:s0";
	u32 length;
	char *context;
	bool allowed;

	if (__kuid_val(current_euid()) != 1000)
		return false;
	security_current_getsecid_subj(secid);
	if (security_secid_to_secctx(*secid, &context, &length))
		return false;
	allowed = (length == sizeof(label) || length == sizeof(label) - 1) &&
		memcmp(context, label, length) == 0;
	security_release_secctx(context, length);
	return allowed;
}

static bool xcy_is_owner(const struct xcy_reclaim_channel *channel)
{
	u32 secid;

	if (current_user_ns() != &init_user_ns)
		return false;
	if (channel->root_publisher)
		return uid_eq(current_euid(), GLOBAL_ROOT_UID) && capable(CAP_SYS_ADMIN);
	if (__kuid_val(current_euid()) != 1000)
		return false;
	security_current_getsecid_subj(&secid);
	return secid == channel->publisher_secid;
}

static int xcy_publish_challenge(struct xcy_reclaim_channel *channel, u64 request,
				 s64 expires_ms, char operation,
				 const struct xcy_reclaim_member *members, size_t count)
{
	struct xcy_reclaim_reply next = {
		.request = request, .expires_ms = expires_ms, .operation = operation,
		.published_ms = ktime_to_ms(ktime_get_boottime()),
	};
	int error;

	mutex_lock(&channel->challenge_lock);
	if (channel->expected.request > request || next.published_ms >= next.expires_ms) {
		error = -ESTALE;
		goto out;
	}
	snprintf(next.nonce, sizeof(next.nonce), "%016llx-%016llx",
		 (unsigned long long)channel->boot_session,
		 (unsigned long long)++channel->nonce_sequence);
	snprintf(next.epoch, sizeof(next.epoch), "%016llx", (unsigned long long)channel->boot_session);
	error = xcy_reclaim_mailbox_publish(&channel->mailbox, operation, members, count,
					   next.nonce, next.epoch);
	if (!error)
		channel->expected = next;
out:
	mutex_unlock(&channel->challenge_lock);
	return error;
}

static int xcy_begin_round(void *owner, u64 request, s64 expires_ms)
{
	return xcy_publish_challenge(owner, request, expires_ms, 'I', NULL, 0);
}

static void xcy_retire_work(struct work_struct *work)
{
	struct xcy_reclaim_channel *channel = container_of(work, struct xcy_reclaim_channel, retire_work);
	u64 retired;

	mutex_lock(&channel->reply_lock);
	retired = atomic64_read(&channel->retired_request);
	mutex_lock(&channel->challenge_lock);
	if (channel->expected.request <= retired) {
		channel->expected.operation = 0;
		xcy_reclaim_mailbox_cancel(&channel->mailbox);
	}
	mutex_unlock(&channel->challenge_lock);
	channel_ops->retire(channel->context, retired);
	mutex_unlock(&channel->reply_lock);
}

static void xcy_retire_round(void *owner, u64 request)
{
	struct xcy_reclaim_channel *channel = owner;

	atomic64_set(&channel->retired_request, request);
	queue_work(system_freezable_power_efficient_wq, &channel->retire_work);
}

struct xcy_reclaim_runtime *xcy_reclaim_channel_runtime(struct xcy_reclaim_channel *channel)
{
	return &channel->runtime;
}

int xcy_reclaim_channel_next(struct xcy_reclaim_channel *channel, u64 request, char operation,
				const struct xcy_reclaim_member *members, size_t count)
{
	s64 expires_ms;
	int error = xcy_reclaim_runtime_renew(&channel->runtime, request, &expires_ms);

	if (error)
		return error;
	return xcy_publish_challenge(channel, request, expires_ms, operation, members, count);
}

static int xcy_channel_open(struct inode *inode, struct file *file)
{
	struct xcy_reclaim_channel *channel;
	u32 secid = 0;
	bool root_publisher = uid_eq(current_euid(), GLOBAL_ROOT_UID);
	int error;

	if (!(file->f_mode & FMODE_READ) || !(file->f_mode & FMODE_WRITE) ||
	    current_user_ns() != &init_user_ns ||
	    !(root_publisher ? capable(CAP_SYS_ADMIN) : xcy_check_publisher_label(&secid)))
		return -EACCES;
	if (atomic_cmpxchg(&channel_open, 0, 1))
		return -EBUSY;
	channel = kzalloc(sizeof(*channel), GFP_KERNEL);
	if (!channel) {
		error = -ENOMEM;
		goto unclaim;
	}
	channel->boot_session = get_random_u64();
	channel->publisher_secid = secid;
	channel->root_publisher = root_publisher;
	mutex_init(&channel->challenge_lock);
	mutex_init(&channel->reply_lock);
	INIT_WORK(&channel->retire_work, xcy_retire_work);
	error = xcy_reclaim_mailbox_init(&channel->mailbox);
	if (error)
		goto free;
	error = channel_ops->open(channel, &channel->context);
	if (error)
		goto destroy;
	error = xcy_reclaim_runtime_start(&channel->runtime, file, xcy_begin_round, xcy_retire_round, channel);
	if (error)
		goto close;
	file->private_data = channel;
	return nonseekable_open(inode, file);
close:
	channel_ops->close(channel->context);
destroy:
	xcy_reclaim_mailbox_destroy(&channel->mailbox);
free:
	kfree(channel);
unclaim:
	atomic_set(&channel_open, 0);
	return error;
}

static int xcy_channel_release(struct inode *inode, struct file *file)
{
	struct xcy_reclaim_channel *channel = file->private_data;

	xcy_reclaim_runtime_stop(&channel->runtime);
	cancel_work_sync(&channel->retire_work);
	xcy_reclaim_mailbox_close(&channel->mailbox);
	channel_ops->close(channel->context);
	xcy_reclaim_mailbox_destroy(&channel->mailbox);
	kfree(channel);
	atomic_set(&channel_open, 0);
	return 0;
}

static ssize_t xcy_channel_read(struct file *file, char __user *buffer, size_t length, loff_t *pos)
{
	struct xcy_reclaim_channel *channel = file->private_data;

	if (!xcy_is_owner(channel))
		return -EACCES;
	return xcy_reclaim_mailbox_read(&channel->mailbox, buffer, length, file->f_flags & O_NONBLOCK);
}

static ssize_t xcy_channel_write(struct file *file, const char __user *buffer, size_t length, loff_t *pos)
{
	struct xcy_reclaim_channel *channel = file->private_data;
	struct xcy_reclaim_reply challenge;
	xcy_memory_snapshot_t snapshot = {0};
	int error;

	if (!xcy_is_owner(channel))
		return -EACCES;
	if (mutex_lock_interruptible(&channel->reply_lock))
		return -EINTR;
	mutex_lock(&channel->challenge_lock);
	challenge = channel->expected;
	mutex_unlock(&channel->challenge_lock);
	error = xcy_reclaim_read_reply(&challenge, buffer, length, &snapshot);
	if (error)
		goto out;
	mutex_lock(&channel->challenge_lock);
	if (strcmp(challenge.nonce, channel->expected.nonce) || !channel->expected.operation)
		error = -ESTALE;
	else
		channel->expected.operation = 0;
	mutex_unlock(&channel->challenge_lock);
	if (!error)
		error = channel_ops->receive(channel, channel->context, &challenge, &snapshot);
out:
	xcy_memory_snapshot_free(&snapshot);
	mutex_unlock(&channel->reply_lock);
	/* A stale pressure round is ordinary. The publisher keeps waiting. */
	if (error == -EAGAIN)
		return -ESTALE;
	return error ? error : length;
}

static const struct file_operations xcy_channel_fops = {
	.open = xcy_channel_open,
	.release = xcy_channel_release,
	.read = xcy_channel_read,
	.write = xcy_channel_write,
	.llseek = no_llseek,
};

static struct miscdevice xcy_channel_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xcy_reclaim",
	.fops = &xcy_channel_fops,
	.mode = 0660,
};

int xcy_reclaim_channel_register(const struct xcy_reclaim_channel_ops *ops)
{
	int error;

	if (!ops || !ops->open || !ops->close || !ops->retire || !ops->receive || channel_ops)
		return -EINVAL;
	channel_ops = ops;
	error = misc_register(&xcy_channel_device);
	if (error)
		channel_ops = NULL;
	return error;
}
