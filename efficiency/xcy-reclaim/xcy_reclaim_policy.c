#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/fs_struct.h>
#include <linux/fsnotify_backend.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched.h>
#include <linux/sched/task.h>
#include <linux/sort.h>
#include <linux/uaccess.h>
#include <linux/user_namespace.h>
#include <linux/workqueue.h>

#include "xcy_reclaim_identity.h"
#include "xcy_reclaim_lease.h"
#include "xcy_reclaim_plan.h"
#include "xcy_reclaim_policy.h"
#include "xcy_reclaim_protection.h"

#define XCY_CONFIG_DIR "/data/adb/XiaoCaiYe/proc_guard/"
#define XCY_SYNC_DIR "/data/adb/feedmeboost-sync"
#define XCY_SCENE_ROOT "/data/user/0/com.omarea.vtools"
#define XCY_SCENE_DIR XCY_SCENE_ROOT "/files"
#define XCY_SCENE_NAME "xiaocaiye_a11y_state"
#define XCY_SCENE_AGE_MS 180000U
#define XCY_LIST_LIMIT (1024U * 1024U)
#define XCY_LIST_ROWS 16384U

enum xcy_policy_input {
	XCY_CONFIG, XCY_NORMAL, XCY_FULL, XCY_EXACT, XCY_GAME, XCY_SYNC, XCY_SCENE, XCY_INPUTS,
};

struct xcy_policy_file {
	const char *path;
	struct kstat stamp;
	char *data;
	const char **names;
	bool missing;
};

struct xcy_reclaim_policy {
	struct xcy_policy_file files[XCY_INPUTS];
	struct xcy_reclaim_protection_frame scene;
	u64 authority;
};

static DEFINE_MUTEX(policy_lock);
static const struct cred *policy_cred;
static struct path policy_root;
static u64 policy_generation;
static struct fsnotify_group *scene_watch;
static void (*scene_changed)(const struct xcy_reclaim_protection_frame *frame);

static struct file *xcy_policy_open(const char *path, int flags)
{
	return file_open_root(&policy_root, path + 1, flags | O_NOFOLLOW | O_NONBLOCK, 0);
}

static bool xcy_same_stamp(const struct kstat *a, const struct kstat *b)
{
	return a->dev == b->dev && a->ino == b->ino && a->size == b->size &&
		a->mode == b->mode && uid_eq(a->uid, b->uid) &&
		timespec64_equal(&a->mtime, &b->mtime) && timespec64_equal(&a->ctime, &b->ctime);
}

static int xcy_safe_dir(const char *path, kuid_t uid, umode_t forbidden)
{
	struct file *file = xcy_policy_open(path, O_PATH | O_DIRECTORY);
	struct inode *inode;
	int error;

	if (IS_ERR(file))
		return PTR_ERR(file);
	inode = file_inode(file);
	error = S_ISDIR(inode->i_mode) && uid_eq(inode->i_uid, uid) &&
		!(inode->i_mode & forbidden) ? 0 : -EACCES;
	fput(file);
	return error;
}

static int xcy_read_policy_file(struct xcy_policy_file *input, size_t limit,
				kuid_t owner, bool optional, umode_t forbidden)
{
	struct file *file = xcy_policy_open(input->path, O_RDONLY);
	struct kstat after;
	loff_t pos = 0;
	ssize_t count;
	int error;

	if (IS_ERR(file)) {
		error = PTR_ERR(file);
		input->missing = optional && error == -ENOENT;
		return input->missing ? 0 : error;
	}
	error = vfs_getattr(&file->f_path, &input->stamp, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (error)
		goto out;
	if (!S_ISREG(input->stamp.mode) || !uid_eq(input->stamp.uid, owner) ||
	    (input->stamp.mode & forbidden) || input->stamp.size < 0 || input->stamp.size > limit) {
		error = -EACCES;
		goto out;
	}
	input->data = xcy_snapshot_calloc(input->stamp.size + 2, 1);
	if (!input->data) {
		error = -ENOMEM;
		goto out;
	}
	while (pos <= input->stamp.size) {
		count = kernel_read(file, input->data + pos, input->stamp.size + 1 - pos, &pos);
		if (count <= 0) {
			if (count < 0)
				error = count;
			break;
		}
	}
	if (error)
		goto out;
	error = vfs_getattr(&file->f_path, &after, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT);
	if (!error && (pos != input->stamp.size || !xcy_same_stamp(&input->stamp, &after)))
		error = -ESTALE;
	if (!error && memchr(input->data, 0, pos))
		error = -EINVAL;
out:
	fput(file);
	return error;
}

static bool xcy_file_current(const struct xcy_policy_file *input)
{
	struct file *file = xcy_policy_open(input->path, O_PATH);
	struct kstat latest;
	bool valid;

	if (IS_ERR(file))
		return input->missing && PTR_ERR(file) == -ENOENT;
	valid = !input->missing &&
		!vfs_getattr(&file->f_path, &latest, STATX_BASIC_STATS, AT_STATX_SYNC_AS_STAT) &&
		xcy_same_stamp(&input->stamp, &latest);
	fput(file);
	return valid;
}

static bool xcy_config_path(const char *value)
{
	const char *name;

	if (!strstarts(value, XCY_CONFIG_DIR))
		return false;
	name = value + sizeof(XCY_CONFIG_DIR) - 1;
	return *name && strcmp(name, ".") && strcmp(name, "..") && !strchr(name, '/');
}

static int xcy_configure(struct xcy_reclaim_policy *policy, u64 *grace_ms, u64 *scene_valid_ms)
{
	struct xcy_policy_file *files = policy->files;
	const char *const paths[] = {
		"whitelist_file", "full_whitelist_file", "exact_whitelist_file", "current_game_file",
	};
	char *cursor = files[XCY_CONFIG].data, *line;
	unsigned int grace = 180, scene_age = 180, enabled = 1;
	size_t i;

	while ((line = strsep(&cursor, "\n"))) {
		char *value = strchr(line, '=');
		char *key;

		if (!value)
			continue;
		*value++ = 0;
		key = strim(line);
		value = strim(value);
		for (i = 0; i < ARRAY_SIZE(paths); ++i) {
			if (!strcmp(key, paths[i])) {
				if (!xcy_config_path(value))
					return -EINVAL;
				files[XCY_NORMAL + i].path = value;
				break;
			}
		}
		if (!strcmp(key, "enabled") && kstrtouint(value, 10, &enabled))
			return -EINVAL;
		if (!strcmp(key, "recent_fg_grace_sec") && kstrtouint(value, 10, &grace))
			return -EINVAL;
		if (!strcmp(key, "scene_a11y_max_age_sec") && kstrtouint(value, 10, &scene_age))
			return -EINVAL;
	}
	if (enabled != 1 || !grace || !scene_age || grace > U32_MAX / 1000 || scene_age > U32_MAX / 1000)
		return -EINVAL;
	for (i = XCY_NORMAL; i <= XCY_GAME; ++i)
		if (!files[i].path)
			return -EINVAL;
	*grace_ms = grace * 1000U;
	*scene_valid_ms = scene_age * 1000U;
	return 0;
}

static int xcy_name_cmp(const void *a, const void *b)
{
	return strcmp(*(const char *const *)a, *(const char *const *)b);
}

static int xcy_policy_names(struct xcy_policy_file *input, struct xcy_reclaim_names *list)
{
	char *cursor = input->data, *line;
	size_t i, count = 1;

	for (i = 0; i < input->stamp.size; ++i)
		count += input->data[i] == '\n';
	if (count > XCY_LIST_ROWS + 1)
		return -E2BIG;
	input->names = xcy_snapshot_calloc(count, sizeof(*input->names));
	if (!input->names)
		return -ENOMEM;
	while ((line = strsep(&cursor, "\n"))) {
		line = strim(line);
		if (!*line)
			continue;
		if (strnlen(line, 512) == 512 || list->count == XCY_LIST_ROWS)
			return -E2BIG;
		input->names[list->count++] = line;
	}
	sort(input->names, list->count, sizeof(*input->names), xcy_name_cmp, NULL);
	list->values = input->names;
	return 0;
}

static int xcy_read_boot_id(char *buffer, size_t capacity)
{
	struct file *file = xcy_policy_open("/proc/sys/kernel/random/boot_id", O_RDONLY);
	loff_t pos = 0;
	ssize_t length;

	if (IS_ERR(file))
		return PTR_ERR(file);
	length = kernel_read(file, buffer, capacity - 1, &pos);
	fput(file);
	if (length != 37 || buffer[36] != '\n')
		return -EIO;
	buffer[36] = 0;
	return xcy_boot_uuid_relation(buffer, buffer) == 1 ? 0 : -EINVAL;
}

static bool xcy_lease_owner(const xcy_sync_lease_t *lease)
{
	struct xcy_reclaim_member member = {
		.pid = lease->pid, .kernel_uid = lease->uid, .starttime = lease->start_ticks,
	};
	const char *process = lease->headless ? "com.feedmeboost:sync" : "com.seazon.feedme";
	const char *root = lease->headless ? "/data/user/0/com.feedmeboost" : "/data/user/0/com.seazon.feedme";
	struct pid *pid = xcy_reclaim_pin_member(&member);
	struct task_struct *task;
	char cmdline[64];
	bool valid = false;
	int length;

	if (!pid)
		return false;
	task = get_pid_task(pid, PIDTYPE_TGID);
	if (task) {
		length = get_cmdline(task, cmdline, sizeof(cmdline));
		valid = length > strlen(process) && !memcmp(cmdline, process, strlen(process) + 1) &&
			!xcy_safe_dir(root, make_kuid(&init_user_ns, lease->uid), 0) &&
			xcy_reclaim_member_current(pid, &member);
		put_task_struct(task);
	}
	put_pid(pid);
	return valid;
}

static int xcy_read_sync(struct xcy_policy_file *input, struct xcy_reclaim_protection *protection)
{
	xcy_sync_lease_t lease = {0};
	char boot_id[64];
	int error, status;

	error = xcy_safe_dir(XCY_SYNC_DIR, GLOBAL_ROOT_UID, 0022);
	if (error)
		return error;
	error = xcy_read_policy_file(input, 4096, GLOBAL_ROOT_UID, true, 0077);
	if (error || input->missing)
		return error;
	error = xcy_read_boot_id(boot_id, sizeof(boot_id));
	if (error)
		return error;
	status = xcy_parse_sync_lease(input->data, input->stamp.size, boot_id,
				     ktime_to_ms(ktime_get_boottime()), &lease);
	if (!status || (status == 1 && !xcy_lease_owner(&lease)))
		return -ESTALE;
	if (status == 1) {
		protection->sync_uid = lease.uid;
		protection->sync_expires_ms = lease.expires_ms;
	}
	return 0;
}

static int xcy_read_scene(struct xcy_policy_file *input, u64 max_age,
			  struct xcy_reclaim_protection_frame *scene)
{
	struct file *app = xcy_policy_open(XCY_SCENE_ROOT, O_PATH | O_DIRECTORY);
	kuid_t uid;
	int error;

	if (IS_ERR(app))
		return PTR_ERR(app);
	uid = file_inode(app)->i_uid;
	fput(app);
	if (__kuid_val(uid) < 10000 || __kuid_val(uid) > 19999)
		return -EACCES;
	input->path = XCY_SCENE_DIR "/" XCY_SCENE_NAME;
	error = xcy_safe_dir(XCY_SCENE_DIR, uid, 0);
	if (!error)
		error = xcy_read_policy_file(input, 32768, uid, false, 0077);
	if (!error)
		error = xcy_reclaim_scene_parse(input->data, ktime_get_real_seconds(),
						input->stamp.mtime.tv_sec, ktime_to_ms(ktime_get_boottime()),
						max_age, scene);
	if (!error)
		input->data = NULL; /* The parsed Scene view owns the bytes. */
	return error;
}

static void xcy_refresh_scene(struct work_struct *work)
{
	struct xcy_reclaim_protection_frame frame = {0};
	struct xcy_policy_file input = {0};
	const struct cred *previous;
	bool ready = false;

	mutex_lock(&policy_lock);
	if (policy_cred) {
		previous = override_creds(policy_cred);
		ready = !xcy_read_scene(&input, XCY_SCENE_AGE_MS, &frame) && xcy_file_current(&input);
		revert_creds(previous);
	}
	scene_changed(ready ? &frame : NULL);
	mutex_unlock(&policy_lock);
	xcy_reclaim_protection_free(&frame);
	xcy_snapshot_free(input.data);
}

static DECLARE_WORK(scene_work, xcy_refresh_scene);

static int xcy_scene_event(struct fsnotify_mark *mark, u32 mask, struct inode *inode, struct inode *directory,
			   const struct qstr *name, u32 cookie)
{
	if (!name || (name->len == sizeof(XCY_SCENE_NAME) - 1 &&
		      !memcmp(name->name, XCY_SCENE_NAME, name->len)))
		queue_work(system_freezable_power_efficient_wq, &scene_work);
	return 0;
}

static void xcy_scene_free_mark(struct fsnotify_mark *mark)
{
	kfree(mark);
}

static const struct fsnotify_ops xcy_scene_notify_ops = {
	.handle_inode_event = xcy_scene_event,
	.free_mark = xcy_scene_free_mark,
};

static struct fsnotify_group *xcy_watch_scene(void)
{
	struct fsnotify_group *group;
	struct fsnotify_mark *mark;
	struct file *directory = xcy_policy_open(XCY_SCENE_DIR, O_PATH | O_DIRECTORY);
	int error;

	if (IS_ERR(directory))
		return ERR_CAST(directory);
	group = fsnotify_alloc_group(&xcy_scene_notify_ops, 0);
	if (IS_ERR(group))
		goto put_file;
	mark = kzalloc(sizeof(*mark), GFP_KERNEL);
	if (!mark) {
		error = -ENOMEM;
		goto destroy;
	}
	fsnotify_init_mark(mark, group);
	mark->mask = FS_EVENT_ON_CHILD | FS_CLOSE_WRITE | FS_MOVED_TO |
		FS_MOVED_FROM | FS_DELETE | FS_DELETE_SELF | FS_MOVE_SELF;
	error = fsnotify_add_inode_mark(mark, file_inode(directory), 0);
	fsnotify_put_mark(mark);
	if (error)
		goto destroy;
put_file:
	fput(directory);
	return group;
destroy:
	fsnotify_destroy_group(group);
	fput(directory);
	return ERR_PTR(error);
}

void xcy_reclaim_policy_put(struct xcy_reclaim_policy *policy)
{
	size_t i;

	if (!policy)
		return;
	xcy_reclaim_protection_free(&policy->scene);
	if (policy->files[XCY_SYNC].data)
		memzero_explicit(policy->files[XCY_SYNC].data, policy->files[XCY_SYNC].stamp.size + 2);
	for (i = 0; i < XCY_INPUTS; ++i) {
		xcy_snapshot_free(policy->files[i].names);
		xcy_snapshot_free(policy->files[i].data);
	}
	kfree(policy);
}

static bool xcy_policy_files_current(const struct xcy_reclaim_policy *policy)
{
	size_t i;

	for (i = 0; i < XCY_INPUTS; ++i)
		if (!xcy_file_current(&policy->files[i]))
			return false;
	return !xcy_safe_dir(XCY_SYNC_DIR, GLOBAL_ROOT_UID, 0022);
}

int xcy_reclaim_policy_get(struct xcy_reclaim_protection *protection, u64 *grace_ms,
			  struct xcy_reclaim_policy **result)
{
	struct xcy_reclaim_names *lists[] = {
		&protection->package_whitelist, &protection->full_whitelist, &protection->exact_whitelist,
	};
	struct xcy_reclaim_policy *policy;
	struct xcy_policy_file *files;
	const struct cred *previous;
	u64 scene_valid_ms;
	size_t i;
	int error = -ENOKEY;

	policy = kzalloc(sizeof(*policy), GFP_KERNEL);
	if (!policy)
		return -ENOMEM;
	files = policy->files;
	files[XCY_CONFIG].path = XCY_CONFIG_DIR "proc_guard.conf";
	files[XCY_SYNC].path = XCY_SYNC_DIR "/sync-round.json";
	protection->current_game = NULL;
	protection->sync_uid = 0;
	protection->sync_expires_ms = 0;
	mutex_lock(&policy_lock);
	if (!policy_cred)
		goto unlock;
	policy->authority = policy_generation;
	previous = override_creds(policy_cred);
	error = xcy_read_policy_file(&files[XCY_CONFIG], 16384, GLOBAL_ROOT_UID, false, 0);
	if (!error)
		error = xcy_configure(policy, grace_ms, &scene_valid_ms);
	for (i = 0; !error && i < ARRAY_SIZE(lists); ++i) {
		memset(lists[i], 0, sizeof(*lists[i]));
		error = xcy_read_policy_file(&files[XCY_NORMAL + i], XCY_LIST_LIMIT, GLOBAL_ROOT_UID, false, 0);
		if (!error)
			error = xcy_policy_names(&files[XCY_NORMAL + i], lists[i]);
	}
	if (!error)
		error = xcy_read_policy_file(&files[XCY_GAME], XCY_MEMORY_PACKAGE_BYTES, GLOBAL_ROOT_UID, true, 0);
	if (!error && files[XCY_GAME].data) {
		char *game = strim(files[XCY_GAME].data);
		xcy_memory_token_t token = { .text = game, .len = strlen(game) };

		if (*game && !xcy_memory_package(token))
			error = -EINVAL;
		else if (*game)
			protection->current_game = game;
	}
	if (!error)
		error = xcy_read_sync(&files[XCY_SYNC], protection);
	if (!error)
		error = xcy_read_scene(&files[XCY_SCENE], scene_valid_ms, &policy->scene);
	if (!error && !xcy_policy_files_current(policy))
		error = -ESTALE;
	revert_creds(previous);
	if (!error) {
		scene_changed(&policy->scene);
		protection->visible = policy->scene.view.visible;
		protection->active = policy->scene.view.active;
	}
unlock:
	mutex_unlock(&policy_lock);
	if (error)
		xcy_reclaim_policy_put(policy);
	else
		*result = policy;
	return error;
}

bool xcy_reclaim_policy_current(const struct xcy_reclaim_policy *policy)
{
	const struct cred *previous;
	bool valid = false;

	mutex_lock(&policy_lock);
	if (policy_cred && policy->authority == policy_generation) {
		previous = override_creds(policy_cred);
		valid = xcy_policy_files_current(policy) &&
			ktime_to_ms(ktime_get_boottime()) - policy->scene.scene_observed_ms <= policy->scene.scene_valid_ms;
		revert_creds(previous);
	}
	mutex_unlock(&policy_lock);
	return valid;
}

static ssize_t xcy_policy_write(struct file *file, const char __user *buffer,
				size_t length, loff_t *pos)
{
	struct fsnotify_group *old_watch;
	char command[8];
	bool enable;
	int error = 0;

	if (!uid_eq(current_euid(), GLOBAL_ROOT_UID) || current_user_ns() != &init_user_ns ||
	    !capable(CAP_SYS_ADMIN))
		return -EACCES;
	if (length != 7 && length != 8)
		return -EINVAL;
	if (copy_from_user(command, buffer, length))
		return -EFAULT;
	enable = length == 7 && !memcmp(command, "enable\n", 7);
	if (!enable && (length != 8 || memcmp(command, "disable\n", 8)))
		return -EINVAL;
	mutex_lock(&policy_lock);
	old_watch = scene_watch;
	scene_watch = NULL;
	if (policy_cred) {
		put_cred(policy_cred);
		path_put(&policy_root);
		policy_cred = NULL;
	}
	if (enable) {
		get_fs_root(current->fs, &policy_root);
		scene_watch = xcy_watch_scene();
		if (IS_ERR(scene_watch)) {
			error = PTR_ERR(scene_watch);
			scene_watch = NULL;
			path_put(&policy_root);
		} else {
			policy_cred = get_current_cred();
		}
	}
	++policy_generation;
	scene_changed(NULL);
	mutex_unlock(&policy_lock);
	if (old_watch)
		fsnotify_destroy_group(old_watch);
	if (enable && !error)
		queue_work(system_freezable_power_efficient_wq, &scene_work);
	return error ? error : length;
}

static const struct file_operations xcy_policy_fops = {
	.open = nonseekable_open,
	.write = xcy_policy_write,
	.llseek = no_llseek,
};

static struct miscdevice xcy_policy_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "xcy_reclaim_policy",
	.fops = &xcy_policy_fops,
	.mode = 0200,
};

int xcy_reclaim_policy_register(void (*notify)(const struct xcy_reclaim_protection_frame *frame))
{
	scene_changed = notify;
	return misc_register(&xcy_policy_device);
}

void xcy_reclaim_policy_unregister(void)
{
	misc_deregister(&xcy_policy_device);
}
