// SPDX-License-Identifier: GPL-2.0
/* KGSL low power defaults. Serialized by the frame policy config_lock. */
#include <linux/cred.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/magic.h>
#include <linux/slab.h>

#include "scene_gpu_aux.h"

#define KGSL_PATH "/sys/class/kgsl/kgsl-3d0/"

struct gpu_aux_lease {
	struct file *file;
	u32 original;
	bool owned, uncertain, conflict, satisfied;
};

struct fp_scene_gpu_aux {
	u32 levels;
	struct gpu_aux_lease leases[FP_SCENE_GPU_AUX_COUNT];
};

static struct fp_scene_gpu_aux *active;
static struct fp_scene_gpu_aux_status status = { .knob = -1, .stage = "none" };

static void clear_error(void)
{
	status.error = 0;
	status.knob = -1;
	status.rollback_error = 0;
	status.stage = "none";
}

static int fail(int error, const char *stage, unsigned int knob)
{
	if (!status.error) {
		status.error = error;
		status.stage = stage;
		status.knob = knob;
	}
	return error;
}

static int read_u32(struct file *file, u32 *value)
{
	const struct cred *saved;
	char text[32];
	loff_t offset = 0;
	ssize_t length;

	saved = override_creds(file->f_cred);
	length = kernel_read(file, text, sizeof(text) - 1, &offset);
	revert_creds(saved);
	if (length <= 0)
		return length < 0 ? length : -EIO;
	if (length == sizeof(text) - 1)
		return -EOVERFLOW;
	text[length] = 0;
	return kstrtou32(strim(text), 10, value);
}

static int read_level(struct file *file, u32 levels, u32 *level)
{
	int error = read_u32(file, level);

	if (error)
		return error;
	return *level < levels ? 0 : -ERANGE;
}

static int write_level(struct file *file, u32 level)
{
	const struct cred *saved;
	char text[16];
	loff_t offset = 0;
	int length = scnprintf(text, sizeof(text), "%u\n", level);
	ssize_t written;

	saved = override_creds(file->f_cred);
	written = kernel_write(file, text, length, &offset);
	revert_creds(saved);
	if (written != length)
		return written < 0 ? written : -EIO;
	status.writes++;
	return 0;
}

static struct file *open_sysfs(const char *path, int flags)
{
	struct file *file = filp_open(path, flags, 0);
	const struct inode *inode;

	if (IS_ERR(file))
		return file;
	inode = file_inode(file);
	if (inode->i_sb->s_magic != SYSFS_MAGIC ||
	    (flags & O_DIRECTORY ? !S_ISDIR(inode->i_mode) : !S_ISREG(inode->i_mode))) {
		fput(file);
		return ERR_PTR(-EINVAL);
	}
	return file;
}

static bool sibling(struct file *base, struct file *attribute)
{
	/* The fixed class symlink is allowed, but a bind-mounted alias or
	 * attributes from another device must not substitute for the receiver.
	 */
	return base->f_path.mnt == attribute->f_path.mnt &&
	       base->f_path.dentry->d_parent == attribute->f_path.dentry->d_parent;
}

void fp_scene_gpu_aux_discard(struct fp_scene_gpu_aux *candidate)
{
	unsigned int knob;

	if (!candidate)
		return;
	for (knob = 0; knob < FP_SCENE_GPU_AUX_COUNT; knob++)
		if (candidate->leases[knob].file)
			fput(candidate->leases[knob].file);
	kfree(candidate);
}

static int prepare_files(struct file *levels, struct file *minimum,
		struct file *initial, struct fp_scene_gpu_aux **candidate)
{
	struct fp_scene_gpu_aux *next;
	u32 count, value;
	int error;

	error = read_u32(levels, &count);
	if (error)
		return error;
	if (!count)
		return -ERANGE;
	error = read_level(minimum, count, &value);
	if (!error && initial)
		error = read_level(initial, count, &value);
	if (error)
		return error;
	next = kzalloc(sizeof(*next), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	next->levels = count;
	next->leases[FP_SCENE_GPU_AUX_MIN].file = get_file(minimum);
	if (initial)
		next->leases[FP_SCENE_GPU_AUX_DEFAULT].file = get_file(initial);
	*candidate = next;
	return 0;
}

int fp_scene_gpu_aux_prepare(bool enabled, struct fp_scene_gpu_aux **candidate)
{
	struct file *levels, *minimum, *initial = NULL, *devfreq;
	int error;

	*candidate = NULL;
	if (!enabled)
		return 0;
	levels = open_sysfs(KGSL_PATH "num_pwrlevels", O_RDONLY | O_NOFOLLOW);
	if (IS_ERR(levels))
		return PTR_ERR(levels);
	minimum = open_sysfs(KGSL_PATH "min_pwrlevel", O_RDWR | O_NOFOLLOW);
	if (IS_ERR(minimum)) {
		error = PTR_ERR(minimum);
		goto out_levels;
	}
	if (!sibling(levels, minimum)) {
		error = -EINVAL;
		goto out_minimum;
	}
	/* Like the stock stat check, follow a devfreq symlink. Only the two
	 * validated KGSL sibling attributes may be written.
	 */
	devfreq = open_sysfs(KGSL_PATH "devfreq", O_RDONLY | O_DIRECTORY);
	if (IS_ERR(devfreq)) {
		error = PTR_ERR(devfreq);
		if (error != -ENOENT)
			goto out_minimum;
	} else {
		fput(devfreq);
		initial = open_sysfs(KGSL_PATH "default_pwrlevel", O_RDWR | O_NOFOLLOW);
		if (IS_ERR(initial)) {
			error = PTR_ERR(initial);
			goto out_minimum;
		}
		if (!sibling(levels, initial)) {
			error = -EINVAL;
			goto out_initial;
		}
	}
	error = prepare_files(levels, minimum, initial, candidate);
out_initial:
	if (initial)
		fput(initial);
out_minimum:
	fput(minimum);
out_levels:
	fput(levels);
	return error;
}

static bool live(u64 generation, const u64 *live_generation)
{
	return !live_generation || READ_ONCE(*live_generation) == generation;
}

static void observe(struct gpu_aux_lease *lease, u32 observed, u32 target)
{
	lease->satisfied = observed == target;
	if (lease->owned && !lease->satisfied) {
		lease->owned = false;
		if (!lease->uncertain || observed != lease->original)
			lease->conflict = true;
	}
	if (lease->satisfied)
		lease->conflict = false;
	lease->uncertain = false;
}

static int set_level(unsigned int knob, struct gpu_aux_lease *lease, u32 value, u32 target)
{
	u32 observed;
	int error, read_error;

	/* A failed write can still have side effects. Keep the journal until
	 * readback identifies the original, target, or an external value.
	 */
	lease->owned = true;
	lease->uncertain = true;
	error = write_level(lease->file, value);
	read_error = read_level(lease->file, active->levels, &observed);
	if (!read_error)
		observe(lease, observed, target);
	if (error)
		return fail(error, "write", knob);
	if (read_error)
		return fail(read_error, "verify", knob);
	if (!lease->conflict && observed != value)
		return fail(-EIO, "verify", knob);
	return 0;
}

static int release_knob(unsigned int knob)
{
	struct gpu_aux_lease *lease = &active->leases[knob];
	u32 observed, target = active->levels - 1;
	int error;

	lease->satisfied = false;
	if (!lease->owned)
		return 0;
	error = read_level(lease->file, active->levels, &observed);
	if (error)
		return fail(error, "release_read", knob);
	observe(lease, observed, target);
	if (!lease->owned)
		return 0;
	error = set_level(knob, lease, lease->original, target);
	if (!error && !lease->owned && !lease->conflict)
		status.restores++;
	return error;
}

int fp_scene_gpu_aux_release(void)
{
	int first_error = 0, error;
	unsigned int knob;

	if (!active)
		return 0;
	clear_error();
	for (knob = FP_SCENE_GPU_AUX_COUNT; knob-- > 0;) {
		error = release_knob(knob);
		if (!first_error)
			first_error = error;
	}
	return first_error;
}

void fp_scene_gpu_aux_commit(struct fp_scene_gpu_aux *candidate)
{
	fp_scene_gpu_aux_discard(active);
	active = candidate;
	clear_error();
}

int fp_scene_gpu_aux_apply(bool allowed, u64 expected_generation, const u64 *current_generation)
{
	u32 acquired = 0, target, observed;
	unsigned int knob;
	int error = 0;
	bool stale = false;

	if (!active)
		return 0;
	if (!allowed)
		return fp_scene_gpu_aux_release();
	clear_error();
	if (!live(expected_generation, current_generation)) {
		status.stage = "stale";
		return 0;
	}
	target = active->levels - 1;
	for (knob = 0; knob < FP_SCENE_GPU_AUX_COUNT; knob++) {
		struct gpu_aux_lease *lease = &active->leases[knob];

		if (!lease->file)
			continue;
		error = read_level(lease->file, active->levels, &observed);
		if (error) {
			fail(error, "read", knob);
			break;
		}
		observe(lease, observed, target);
		if (lease->satisfied || lease->conflict)
			continue;
		if (!live(expected_generation, current_generation)) {
			stale = true;
			status.stage = "stale";
			break;
		}
		lease->original = observed;
		acquired |= BIT(knob);
		error = set_level(knob, lease, target, target);
		if (error)
			break;
	}
	if (!error && acquired && !live(expected_generation, current_generation)) {
		stale = true;
		status.stage = "stale";
	}
	if (!error && !stale)
		return 0;
	for (knob = FP_SCENE_GPU_AUX_COUNT; knob-- > 0;) {
		int rollback;

		if (!(acquired & BIT(knob)))
			continue;
		rollback = release_knob(knob);
		if (!status.rollback_error)
			status.rollback_error = rollback;
	}
	return stale ? status.rollback_error : error;
}

void fp_scene_gpu_aux_status(struct fp_scene_gpu_aux_status *out)
{
	unsigned int knob;

	*out = status;
	if (!active)
		return;
	out->prepared = true;
	out->target = active->levels - 1;
	for (knob = 0; knob < FP_SCENE_GPU_AUX_COUNT; knob++) {
		const struct gpu_aux_lease *lease = &active->leases[knob];

		if (lease->owned)
			out->owned_mask |= BIT(knob);
		if (lease->conflict)
			out->conflict_mask |= BIT(knob);
		if (lease->satisfied)
			out->satisfied_mask |= BIT(knob);
	}
}

void fp_scene_gpu_aux_exit(void)
{
	fp_scene_gpu_aux_release();
	fp_scene_gpu_aux_discard(active);
	active = NULL;
}

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
int fp_scene_gpu_aux_test_prepare(struct file *levels, struct file *minimum,
		struct file *initial, struct fp_scene_gpu_aux **candidate)
{
	*candidate = NULL;
	if (!levels || !minimum)
		return -EBADF;
	return prepare_files(levels, minimum, initial, candidate);
}
#endif
