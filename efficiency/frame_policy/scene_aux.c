// SPDX-License-Identifier: GPL-2.0
/* Event-only Scene auxiliary file leases. Serialized by frame config_lock. */
#include <linux/cred.h>
#include <linux/cpufreq.h>
#include <linux/dcache.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <linux/vmalloc.h>
#include "scene_aux.h"
#include "scene_aux_plan.h"

struct aux_lease {
	struct file *file;
	int original, last;
	bool owned, conflict;
};

struct aux_runtime {
	struct fp_scene_aux_table *table;
	struct aux_lease leases[FP_MAX_CPUS][FP_SCENE_AUX_KNOBS];
	u32 cores[FP_MAX_CPUS];
	/* Borrowed only during the synchronous config_lock-owned transaction. */
	const u64 *current_generation;
	u64 expected_generation;
	bool canceled;
};

static struct aux_runtime *runtime;
static struct fp_aux_status status = {.profile = -1, .cpu = -1, .knob = -1, .stage = "none"};

static void fault(int error, const char *stage, u32 cpu, u32 knob)
{
	if (!status.error) {
		status.error = error;
		status.stage = stage;
		status.cpu = cpu;
		status.knob = knob;
	}
}

static bool value_valid(u32 knob, int value)
{
	if (knob == FP_AUX_KNOB_BOOST)
		return value >= FP_AUX_BOOST_MIN && value <= FP_AUX_BOOST_MAX;
	if (value < 0)
		return false;
	if (knob <= FP_AUX_KNOB_DOWN_RATE)
		return value <= FP_AUX_RATE_MAX_US;
	if (knob == FP_AUX_KNOB_HISPEED)
		return value <= FP_AUX_HISPEED_MAX_KHZ;
	return value <= (knob == FP_AUX_KNOB_CORE_ENABLE ? 1 : 64);
}

static int read_value(struct aux_lease *lease, u32 knob, int *value)
{
	const struct cred *saved;
	char text[32];
	loff_t offset = 0;
	ssize_t length;
	int error;

	if (!lease->file)
		return -EBADF;
	saved = override_creds(lease->file->f_cred);
	length = kernel_read(lease->file, text, sizeof(text) - 1, &offset);
	revert_creds(saved);
	if (length <= 0)
		return length < 0 ? length : -EIO;
	text[length] = 0;
	error = kstrtoint(strim(text), 10, value);
	return error ? error : value_valid(knob, *value) ? 0 : -ERANGE;
}

static int write_value(struct aux_lease *lease, int value)
{
	const struct cred *saved;
	char text[32];
	loff_t offset = 0;
	int length;
	ssize_t written;

	if (runtime)
		runtime->canceled = false;
	if (runtime && runtime->current_generation &&
	    READ_ONCE(*runtime->current_generation) != runtime->expected_generation) {
		runtime->canceled = true;
		return -ECANCELED;
	}
	length = scnprintf(text, sizeof(text), "%d\n", value);
	saved = override_creds(lease->file->f_cred);
	written = kernel_write(lease->file, text, length, &offset);
	revert_creds(saved);
	if (written != length)
		return written < 0 ? written : -EIO;
	/* Save our acknowledged write before readback: a failed verification
	 * must retain a restore/retry journal, never discard the owned identity.
	 */
	lease->last = value;
	status.writes++;
	return 0;
}

static int scalar(u32 cpu, u32 knob, bool wanted, int desired)
{
	struct aux_lease *lease = &runtime->leases[cpu][knob];
	int observed, error;

	if (!lease->file || (!wanted && !lease->owned))
		return 0;
	error = read_value(lease, knob, &observed);
	if (error) {
		fault(error, "aux_read", cpu, knob);
		return error;
	}
	if (lease->owned && observed != lease->last) {
		lease->owned = false;
		lease->conflict = true;
	}
	if (!wanted) {
		if (!lease->owned)
			return 0;
		desired = lease->original;
	} else if (lease->conflict && observed != desired) {
		return 0;
	}
	if (wanted && !lease->owned) {
		lease->original = observed;
		lease->last = observed;
		lease->owned = true;
		lease->conflict = false;
	}
	if (observed != desired) {
		error = write_value(lease, desired);
		if (!error)
			error = read_value(lease, knob, &observed);
		if (!error && observed != desired) {
			lease->owned = false;
			lease->conflict = true;
			error = -EIO;
		}
		if (error) {
			if (!runtime->canceled)
				fault(error, "aux_write", cpu, knob);
			return error;
		}
	}
	if (!wanted)
		lease->owned = false;
	return 0;
}

static int read_core(u32 cpu, int values[3])
{
	u32 i;
	int error;

	for (i = 0; i < 3; i++) {
		u32 knob = FP_AUX_KNOB_CORE_ENABLE + i;

		error = read_value(&runtime->leases[cpu][knob], knob, &values[i]);
		if (error) {
			fault(error, "aux_core_read", cpu, knob);
			return error;
		}
	}
	if (values[1] > values[2]) {
		fault(-ERANGE, "aux_core_bounds", cpu, FP_AUX_KNOB_CORE_MIN);
		return -ERANGE;
	}
	return 0;
}

static int core(u32 cpu, bool wanted, const struct fp_scene_aux_cpu *profile, bool bounds_only)
{
	struct aux_lease *leases = &runtime->leases[cpu][FP_AUX_KNOB_CORE_ENABLE];
	struct fp_aux_step steps[4];
	int observed[3], expected[3], desired[3], error;
	u32 i, count;

	if (!leases[0].file || (!wanted && !leases[0].owned))
		return 0;
	error = read_core(cpu, observed);
	if (error)
		return error;
	for (i = 0; i < 3; i++)
		expected[i] = leases[i].last;
	if (leases[0].owned && memcmp(observed, expected, sizeof(observed))) {
		leases[0].owned = false;
		leases[0].conflict = true;
	}
	if (!wanted) {
		if (!leases[0].owned)
			return 0;
		for (i = 0; i < 3; i++)
			desired[i] = leases[i].original;
	} else {
		desired[0] = profile->core_enable;
		desired[1] = profile->min_cpus;
		desired[2] = profile->max_cpus;
		if (leases[0].conflict && memcmp(observed, desired, sizeof(observed)))
			return 0;
		if (!leases[0].owned) {
			for (i = 0; i < 3; i++)
				leases[i].original = leases[i].last = observed[i];
			leases[0].owned = true;
			leases[0].conflict = false;
		}
	}
	count = fp_aux_core_plan(observed, desired, steps);
	for (i = 0; i < count; i++) {
		u32 slot = steps[i].knob - FP_AUX_KNOB_CORE_ENABLE, j;

		/* Native force-restore (532bb0) changes max/min only. Its lack of
		 * enable writes is observable and must survive a currently-disabled
		 * vendor controller; do not temporarily enable it as a side effect.
		 */
		if (bounds_only && !slot)
			continue;

		error = read_core(cpu, observed);
		if (error)
			return error;
		for (j = 0; j < 3; j++)
			expected[j] = leases[j].last;
		if (memcmp(observed, expected, sizeof(observed))) {
			leases[0].owned = false;
			leases[0].conflict = true;
			return 0;
		}
		error = write_value(&leases[slot], steps[i].value);
		if (error) {
			if (!runtime->canceled)
				fault(error, "aux_core_write", cpu, steps[i].knob);
			return error;
		}
		error = read_core(cpu, observed);
		if (error)
			return error;
		for (j = 0; j < 3; j++)
			expected[j] = leases[j].last;
		if (memcmp(observed, expected, sizeof(observed))) {
			leases[0].owned = false;
			leases[0].conflict = true;
			fault(-EIO, "aux_core_readback", cpu, steps[i].knob);
			return -EIO;
		}
	}
	if (!wanted)
		leases[0].owned = false;
	return 0;
}

static int release_all(void)
{
	u32 i, j;
	int first = 0, error;

	if (!runtime)
		return 0;
	for (i = 0; i < runtime->table->cpu_count; i++) {
		for (j = 0; j < FP_AUX_KNOB_CORE_ENABLE; j++) {
			error = scalar(i, j, false, 0);
			if (!first)
				first = error;
		}
		error = core(i, false, NULL, false);
		if (!first)
			first = error;
	}
	return first;
}

static void free_runtime(struct aux_runtime *state)
{
	u32 i, j;

	if (!state)
		return;
	for (i = 0; i < FP_MAX_CPUS; i++)
		for (j = 0; j < FP_SCENE_AUX_KNOBS; j++)
			if (state->leases[i][j].file)
				fput(state->leases[i][j].file);
	kvfree(state->table);
	kfree(state);
}

int fp_scene_aux_clear(void)
{
	int error = release_all();

	if (error)
		return error; /* Keep pinned restore metadata for the next explicit action. */
	free_runtime(runtime);
	runtime = NULL;
	status.profile = -1;
	status.error = 0;
	status.stage = "none";
	status.cpu = status.knob = -1;
	return 0;
}

void fp_scene_aux_exit(void)
{
	release_all();
	free_runtime(runtime);
	runtime = NULL;
}

static int validate_file(struct file *file, u32 cpu, u32 knob, char *buffer)
{
	static const char *const governor[] = {
		"up_rate_limit_us", "down_rate_limit_us", "hispeed_freq", "boost",
	};
	static const char *const controller[] = {"enable", "min_cpus", "max_cpus"};
	char expected[128], *actual;

	if ((file->f_mode & (FMODE_READ | FMODE_WRITE)) != (FMODE_READ | FMODE_WRITE) ||
	    strcmp(file_inode(file)->i_sb->s_type->name, "sysfs"))
		return -EINVAL;
	if (knob < FP_AUX_KNOB_CORE_ENABLE)
		scnprintf(expected, sizeof(expected), "/sys/devices/system/cpu/cpufreq/policy%u/walt/%s",
			cpu, governor[knob]);
	else
		scnprintf(expected, sizeof(expected), "/sys/devices/system/cpu/cpu%u/core_ctl/%s",
			cpu, controller[knob - FP_AUX_KNOB_CORE_ENABLE]);
	actual = d_path(&file->f_path, buffer, PAGE_SIZE);
	return IS_ERR(actual) ? PTR_ERR(actual) : strcmp(actual, expected) ? -EINVAL : 0;
}

static int prepare(struct fp_scene_aux_table *next, const struct fp_scene_table *scene,
		   u64 epoch, struct aux_runtime **result,
		   struct file *test_files[FP_MAX_CPUS][FP_SCENE_AUX_KNOBS])
{
	struct aux_runtime *candidate;
	char *path;
	u32 i, j, k;
	int error = 0;

	if (!fp_scene_aux_table_valid(next, true))
		return -EINVAL;
	if (!next->profile_count) {
		*result = NULL;
		return 0;
	}
	if (!scene || next->expected_scene_epoch != epoch || next->cpu_count != scene->cpu_count ||
	    memcmp(next->cpu_ids, scene->cpu_ids, sizeof(next->cpu_ids)))
		return -ESTALE;
	candidate = kzalloc(sizeof(*candidate), GFP_KERNEL);
	if (!candidate)
		return -ENOMEM;
	path = kmalloc(PAGE_SIZE, GFP_KERNEL);
	if (!path) {
		kfree(candidate);
		return -ENOMEM;
	}
	for (i = 0; i < next->cpu_count && !error; i++) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(next->cpu_ids[i]);
		u32 cores;

		if (!policy || policy->cpu != next->cpu_ids[i]) {
			if (policy)
				cpufreq_cpu_put(policy);
			error = -ENODEV;
			break;
		}
		cores = cpumask_weight(policy->related_cpus);
		candidate->cores[i] = cores;
		for (k = 0; k < next->profile_count; k++) {
			const struct fp_scene_aux_cpu *cpu = &next->profiles[k].cpus[i];

			if (cpu->hispeed_khz > policy->cpuinfo.max_freq || cpu->max_cpus > cores)
				error = -ERANGE;
		}
		cpufreq_cpu_put(policy);
		for (j = 0; j < FP_SCENE_AUX_KNOBS && !error; j++) {
			struct aux_lease *lease = &candidate->leases[i][j];
			int value;

			if (next->fds[i][j] == -1)
				continue;
			lease->file = test_files ? test_files[i][j] : fget(next->fds[i][j]);
			if (test_files && lease->file)
				get_file(lease->file);
			if (!lease->file) {
				error = -EBADF;
				break;
			}
			if (!test_files)
				error = validate_file(lease->file, next->cpu_ids[i], j, path);
			if (!error)
				error = read_value(lease, j, &value);
			if (!error && j >= FP_AUX_KNOB_CORE_MIN && value > cores)
				error = -ERANGE;
		}
		if (!error && (fp_scene_aux_required(next, i) & FP_AUX_CORE_CTL)) {
			int minimum, maximum;

			error = read_value(&candidate->leases[i][FP_AUX_KNOB_CORE_MIN],
				FP_AUX_KNOB_CORE_MIN, &minimum);
			if (!error)
				error = read_value(&candidate->leases[i][FP_AUX_KNOB_CORE_MAX],
					FP_AUX_KNOB_CORE_MAX, &maximum);
			if (!error && minimum > maximum)
				error = -ERANGE;
		}
	}
	kfree(path);
	if (error) {
		free_runtime(candidate);
		return error;
	}
	candidate->table = next;
	*result = candidate;
	return 0;
}

static int replace(struct fp_scene_aux_table *next, const struct fp_scene_table *scene,
		   u64 epoch, struct file *test_files[FP_MAX_CPUS][FP_SCENE_AUX_KNOBS])
{
	struct aux_runtime *candidate;
	int error = prepare(next, scene, epoch, &candidate, test_files);

	if (error)
		return error;
	error = fp_scene_aux_commit(next, candidate);
	if (error) {
		fp_scene_aux_discard(candidate);
		return error;
	}
	return 0;
}

int fp_scene_aux_prepare(struct fp_scene_aux_table *next, const struct fp_scene_table *scene,
		u64 scene_epoch, struct aux_runtime **candidate)
{
	return prepare(next, scene, scene_epoch, candidate, NULL);
}

void fp_scene_aux_discard(struct aux_runtime *candidate)
{
	if (candidate)
		candidate->table = NULL;
	free_runtime(candidate);
}

int fp_scene_aux_commit(struct fp_scene_aux_table *next, struct aux_runtime *candidate)
{
	int error = fp_scene_aux_clear();

	if (error)
		return error;
	runtime = candidate;
	if (!candidate)
		kvfree(next);
	return 0;
}

int fp_scene_aux_replace(struct fp_scene_aux_table *next,
		const struct fp_scene_table *scene, u64 epoch)
{
	return replace(next, scene, epoch, NULL);
}

void fp_scene_aux_apply(const char *package, bool game, bool off, bool inactive, bool allowed,
		u64 expected_generation, const u64 *current_generation)
{
	const struct fp_scene_aux_profile *profile = allowed && runtime ?
		fp_scene_aux_select(runtime->table, package, game, off, inactive) : NULL;
	u32 i, j;
	int error = 0;

	status.profile = profile ? profile - runtime->table->profiles : -1;
	status.error = 0;
	status.stage = "none";
	status.cpu = status.knob = -1;
	if (!runtime)
		return;
	runtime->current_generation = profile ? current_generation : NULL;
	runtime->expected_generation = expected_generation;
	runtime->canceled = false;
	if (!profile) {
		release_all();
		return;
	}
	for (i = 0; i < runtime->table->cpu_count && !error; i++) {
		const struct fp_scene_aux_cpu *cpu = &profile->cpus[i];
		int desired[] = {cpu->up_us, cpu->down_us, cpu->hispeed_khz, cpu->walt_boost};

		for (j = 0; j < FP_AUX_KNOB_CORE_ENABLE && !error; j++)
			error = scalar(i, j, cpu->valid_mask & fp_scene_aux_knob_bit(j), desired[j]);
		if (!error && ((cpu->valid_mask & FP_AUX_CORE_CTL) || !cpu->limiter_cluster_count))
			error = core(i, cpu->valid_mask & FP_AUX_CORE_CTL, cpu, false);
		/* A retained dynamic limiter is not an instruction to restore its
		 * group merely because the new profile has no direct core alias.
		 * Keep the singleton's group until its next changed demand, unless
		 * this profile explicitly removes the dynamic limiter altogether.
		 */
	}
	if (error || (current_generation && READ_ONCE(*current_generation) != expected_generation)) {
		/* Stop only this aux activation; preserve its first errno/stage and
		 * retry journal. Conditional restore never writes a foreign value.
		 */
		status.profile = -1;
		status.rollbacks++;
		/* Restoring our own old leases is allowed across the canceled epoch;
		 * acquiring any new desired value is not. The provider never blocks.
		 */
		runtime->current_generation = NULL;
		if (!status.error)
			status.stage = "aux_stale";
		release_all();
	}
	runtime->current_generation = NULL;
}

bool fp_scene_aux_activity_needed(const char *package, bool game, bool off, bool allowed)
{
	const struct fp_scene_aux_profile *active, *inactive;

	if (!allowed || !runtime)
		return false;
	active = fp_scene_aux_select(runtime->table, package, game, off, false);
	inactive = fp_scene_aux_select(runtime->table, package, game, off, true);
	return active && inactive && memcmp(active, inactive, sizeof(*active));
}

const struct fp_scene_aux_profile *fp_scene_aux_current(void)
{
	return runtime && status.profile >= 0 ? &runtime->table->profiles[status.profile] : NULL;
}

/* Metadata belongs to the validated snapshot, independently of direct-file
 * activation success. A failed WALT knob must not revert CPU estimation to
 * the legacy percentage approximation or disable the CPU/GPU controller.
 */
const struct fp_scene_aux_profile *fp_scene_aux_for(const char *package,
		bool game, bool off, bool inactive)
{
	return runtime ? fp_scene_aux_select(runtime->table, package, game, off, inactive) : NULL;
}

const struct fp_scene_aux_options *fp_scene_aux_options(void)
{
	return runtime ? &runtime->table->options : NULL;
}

int fp_scene_aux_core_seed(u32 *enabled_clusters, u32 *retained)
{
	const struct fp_scene_aux_cpu *first;
	u32 i, list = 0;
	int observed[3], error;

	if (!runtime || status.profile < 0 || status.error)
		return status.error ? status.error : -ENOENT;
	first = &runtime->table->profiles[status.profile].cpus[0];
	if (!first->limiter_cluster_count)
		return -ENOENT;
	for (i = 0; i < first->limiter_cluster_count; i++)
		list |= 1U << ((first->limiter_clusters_packed >> (8 * i)) & 255);
	*enabled_clusters = *retained = 0;
	/* Native singleton constructor 532ff8..533104: physical-order selected
	 * clusters, map based on min==N, consecutive-full count resets at a
	 * selected non-full cluster. This is not popcount or selected-list order.
	 */
	for (i = 0; i < runtime->table->cpu_count; i++) {
		if (!(list & (1U << i)))
			continue;
		error = read_core(i, observed);
		if (error) {
			status.profile = -1;
			status.rollbacks++;
			release_all();
			return error;
		}
		if (observed[1] == runtime->cores[i]) {
			*enabled_clusters |= 1U << i;
			(*retained)++;
		} else {
			*retained = 0;
		}
	}
	return 0;
}

void fp_scene_aux_core_update(u32 active_clusters, bool restore,
		u64 expected_generation, const u64 *current_generation)
{
	const struct fp_scene_aux_cpu *first;
	u32 i, list = 0;
	int error = 0;

	if (!runtime || status.profile < 0 || status.error)
		return;
	first = &runtime->table->profiles[status.profile].cpus[0];
	if (!first->limiter_cluster_count)
		return;
	for (i = 0; i < first->limiter_cluster_count; i++)
		list |= 1U << ((first->limiter_clusters_packed >> (8 * i)) & 255);
	runtime->current_generation = current_generation;
	runtime->expected_generation = expected_generation;
	runtime->canceled = false;
	for (i = 0; i < runtime->table->cpu_count && !error; i++) {
		struct fp_scene_aux_cpu desired = {};
		u32 n = runtime->cores[i];
		int observed[3];

		if (!restore && !(list & (1U << i)))
			continue;
		if (restore) {
			error = read_core(i, observed);
			if (error)
				break;
			desired.core_enable = observed[0];
			desired.min_cpus = desired.max_cpus = n;
		} else {
			desired.core_enable = 1;
			desired.min_cpus = desired.max_cpus = active_clusters & (1U << i) ?
				n : n > 4 ? 4 : n > 3 ? 2 : n > 2 ? 1 : 0;
		}
		/* No read/write at all when an unchanged group is still ours.
		 * Ownership is revalidated at the next real group transition.
		 */
		if (runtime->leases[i][FP_AUX_KNOB_CORE_ENABLE].owned &&
		    runtime->leases[i][FP_AUX_KNOB_CORE_ENABLE].last == desired.core_enable &&
		    runtime->leases[i][FP_AUX_KNOB_CORE_MIN].last == desired.min_cpus &&
		    runtime->leases[i][FP_AUX_KNOB_CORE_MAX].last == desired.max_cpus)
			continue;
		error = core(i, true, &desired, restore);
	}
	if (error) {
		status.profile = -1;
		status.rollbacks++;
		runtime->current_generation = NULL;
		if (!status.error)
			status.stage = "aux_stale";
		release_all();
	}
	runtime->current_generation = NULL;
}

void fp_scene_aux_status(struct fp_aux_status *output)
{
	u32 i, j;

	*output = status;
	output->loaded = !!runtime;
	output->owned = output->conflicts = 0;
	output->owned_mask = output->conflict_mask = 0;
	if (!runtime)
		return;
	for (i = 0; i < runtime->table->cpu_count; i++)
		for (j = 0; j <= FP_AUX_KNOB_CORE_ENABLE; j++) {
			output->owned += runtime->leases[i][j].owned;
			output->conflicts += runtime->leases[i][j].conflict;
			if (runtime->leases[i][j].owned)
				output->owned_mask |= 1U << (i * FP_SCENE_AUX_KNOBS + j);
			if (runtime->leases[i][j].conflict)
				output->conflict_mask |= 1U << (i * FP_SCENE_AUX_KNOBS + j);
		}
}

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
int fp_scene_aux_test_replace(struct fp_scene_aux_table *next,
		const struct fp_scene_table *scene, u64 epoch,
		struct file *files[FP_MAX_CPUS][FP_SCENE_AUX_KNOBS])
{
	return replace(next, scene, epoch, files);
}

void fp_scene_aux_test_reset(void)
{
	fp_scene_aux_exit();
	status = (struct fp_aux_status){.profile = -1, .cpu = -1, .knob = -1, .stage = "none"};
}
#endif
