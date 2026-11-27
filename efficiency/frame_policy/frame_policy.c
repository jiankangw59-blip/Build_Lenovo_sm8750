// SPDX-License-Identifier: GPL-2.0
/* Frame events and Scene frequency requests. All arithmetic is integer based. */
#include <linux/capability.h>
#include <linux/cred.h>
#include <linux/ctype.h>
#include <linux/cpufreq.h>
#include <linux/dma-fence.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
#include <linux/irq_work.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/math64.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/platform_device.h>
#include <linux/perf_event.h>
#include <linux/pm_qos.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>
#include <linux/sched/task.h>
#include <linux/slab.h>
#include <linux/sort.h>
#include <linux/spinlock.h>
#include <linux/sync_file.h>
#include <linux/threads.h>
#include <linux/uaccess.h>
#include <linux/uprobes.h>
#include <linux/vmalloc.h>
#include <linux/workqueue.h>
#include <trace/events/sched.h>
#include <asm/ptrace.h>

#include "frame_control.h"
#include "fas_frames.h"
#include "fas_task_load.h"
#include "frame_rate.h"
#include "game_constraints.h"
#include "scene_policy.h"
#include "scene_aux.h"
#include "scene_gpu_aux.h"
#include "scene_limiter_feedback.h"
#include "scene_event_feedback.h"
#include "scene_activity_backend.h"
#include "scene_activity_profiles.h"
#include "frame_policy_uapi.h"

#define FP_PACKAGE 128
#define FP_STREAMS 8
#define FP_CALLS 32
#define FP_FRAME_WORK_NS 20000000ULL
#define FP_INACTIVITY_NS 1000000000ULL
#define FP_LATE_MIN_NS 50000000ULL

struct fp_cpu_request {
	struct cpufreq_policy *policy;
	struct freq_qos_request min, max;
};

struct fp_call {
	pid_t tid;
	u64 start_time, generation, surface, entered_ns;
	struct dma_fence *fence;
	bool gpu_ready;
};

struct fp_fence_watch {
	struct dma_fence_cb callback;
	struct dma_fence *fence;
	u64 generation, rate_generation, entered_ns;
};

struct fp_stream {
	u64 surface, previous_ns, hits;
	struct fp_control control;
	struct fp_rate rate;
	struct fp_fas_history history;
};

struct fp_pmu {
	struct perf_event *cycles;
	u64 count, enabled, running, timestamp;
};

struct fp_game_cpu {
	u64 runtime_ns, previous_ns, running_runtime;
	u32 policy_index;
	/* Borrowed only while holding frame_lock; cleared before task can exit. */
	struct task_struct *running;
};

struct fp_fas_batch_state {
	struct fp_fas_feedback feedback;
	u32 util_permille, effective_mfps;
	s64 last_adjustment;
	bool util_valid;
};

static DEFINE_MUTEX(config_lock);
static DEFINE_SPINLOCK(frame_lock);
static struct workqueue_struct *policy_wq;
static struct irq_work scene_irq_work;
static struct irq_work activity_control_irq;
static struct fp_config configuration;
static struct fp_game_rules *game_rules;
static struct fp_scene_table *scene_table;
static u64 scene_table_epoch;
static const struct fp_scene_profile *scene_profile;
static u32 active_game_fps[FP_MAX_RATES], active_game_fps_count;
static struct fp_cpu_request cpu_requests[FP_MAX_CPUS];
static struct fp_pmu *pmus;
/* Ordered policy work and config_lock own these arrays. No sample-sized
 * allocation, high-order stack buffer, or lock-time allocation is needed.
 */
static struct fp_scene_limiter_cpu_sample scene_cpu_samples[FP_SCENE_LIMITER_MAX_CPUS];
static struct fp_scene_limiter_history scene_history[FP_MAX_CPUS];
static struct fp_scene_limiter_vote_state scene_votes[FP_MAX_CPUS];
static struct fp_scene_limiter_core scene_core;
static u64 scene_native_samples;
static bool scene_gesture_live, scene_notify_live;
static u64 scene_feedback_at;
static struct fp_scene_event_state scene_sched_events;
static u32 scene_panel_peak;
static bool scene_core_applied, scene_core_restore;
static u32 scene_core_mask;
static int gpu_aux_error;
static struct fp_game_cpu *game_cpus;
static bool game_trace;
static u64 game_sample_ns, game_switches;
static u32 cpu_ceilings[FP_MAX_CPUS], cpu_loads[FP_MAX_CPUS];
static u32 game_targets[FP_MAX_CPUS];
static bool game_cap_valid[FP_MAX_CPUS];
static struct fp_fas_tuning fas_tuning = {
	.abi = FP_FAS_ABI, .mode = 1, .margin_base_mfps = 1000,
};
static struct fp_fas_feedback fas_feedback;
static struct fp_fas_events fas_events;
/* Ordered worker owns this batch under config_lock; never put 3KiB on stack. */
static struct fp_fas_event fas_batch[FP_FAS_EVENTS];
static bool fas_overflowed, fas_util_valid;
static u32 fas_util_permille, fas_effective_mfps;
static u64 fas_steps, fas_late_steps, fas_overflows, fas_last_rate_generation;
static u64 fas_task_samples, fas_task_sample_errors, fas_task_sample_ns, fas_task_attempt_ns;
static u64 fas_critical_mask, fas_task_rollbacks;
static s64 fas_last_adjustment;
static struct fp_fas_task_sample *fas_previous_tasks;
static u32 fas_previous_count;
static int render_cpu = -1;
static struct dev_pm_qos_request gpu_min, gpu_max;
static struct device *gpu_device;
static struct file *gpu_governor_file;
static u32 gpu_mode_original, gpu_mode_last;
static bool gpu_mode_owned, gpu_mode_conflict;
static struct task_struct *target;
static struct mm_struct *target_mm;
static struct file *probe_file;
static struct uprobe_consumer consumer;
static struct fp_call calls[FP_CALLS];
static struct fp_fence_watch gpu_watches[FP_CALLS];
static struct fp_control gpu_control;
static struct fp_stream streams[FP_STREAMS];
static char foreground[FP_PACKAGE];
static struct {
	char package[FP_PACKAGE];
	bool valid, game, off;
	bool inactive;
	u64 provider_epoch;
} pending_scene;
static bool configured, source_valid, source_game, display_off, registered;
static bool scene_inactive;
static bool game_active, scene_active, scene_trace;
static u64 generation, applied_generation, last_frame_ns, last_work_ns;
static u64 last_frame_interval_ns, late_frame_ns, late_rate_generation;
static u32 last_frame_margin_mfps;
static bool late_sent;
static u64 frames_seen, frames_used, frame_errors, switches;
static u64 gpu_samples, gpu_unknown;
static u64 rate_generation;
static u32 active_fps;
static unsigned int selected_stream, floor_level;
static u32 cpu_floors[FP_MAX_CPUS], gpu_floor;
static int last_error;
static const char *last_error_stage = "none";
static atomic64_t next_scene_sample = ATOMIC64_INIT(0);
static u64 scene_samples;
/* Distinct from generation: an inactive QoS update must not reset its own input epoch. */
static u64 scene_activity_epoch, scene_inactive_events;
static bool scene_gesture_boundary; /* frame_lock */
#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
static bool test_activity_present_at_game_start;
static bool (*test_scene_cpu_sample)(unsigned int cpu, u32 *khz);
#endif

static void apply_control(struct work_struct *work);
static void apply_frame(struct work_struct *work);
static void expire_frame(struct work_struct *work);
static void expire_scene_feedback(struct work_struct *work);
static void apply_scene(struct work_struct *work);
static void scene_switch(void *data, bool preempt, struct task_struct *previous,
			 struct task_struct *next, unsigned int prev_state);
static void game_switch(void *data, bool preempt, struct task_struct *previous,
			struct task_struct *next, unsigned int prev_state);
static const struct fp_game_rule *active_game_rule(void);
static u32 snap_game_ceiling(struct cpufreq_policy *policy, u32 target_khz,
			    const struct fp_limits *limits);
static void reset_frame_history(void);
static DECLARE_WORK(control_work, apply_control);
static DECLARE_WORK(frame_work, apply_frame);
static DECLARE_WORK(scene_work, apply_scene);
static DECLARE_DELAYED_WORK(expire_work, expire_frame);
static DECLARE_DELAYED_WORK(scene_feedback_deadline, expire_scene_feedback);

static bool observe_only(void)
{
	return configuration.flags & FP_OBSERVE;
}

static int read_gpu_mode(u32 *value)
{
	const struct cred *saved;
	char buffer[24];
	loff_t offset = 0;
	ssize_t length;

	if (!gpu_governor_file)
		return -EBADF;
	/* A kernel worker must retain the authority of the root-opened file. */
	saved = override_creds(gpu_governor_file->f_cred);
	length = kernel_read(gpu_governor_file, buffer, sizeof(buffer) - 1, &offset);
	revert_creds(saved);
	if (length <= 0) {
		last_error_stage = "gpu_mode_read";
		return length < 0 ? (int)length : -EIO;
	}
	buffer[length] = 0;
	return kstrtou32(strim(buffer), 10, value);
}

static int write_gpu_mode(u32 value)
{
	const struct cred *saved;
	char buffer[24];
	loff_t offset = 0;
	u32 readback;
	int length, error;
	ssize_t written;

	length = scnprintf(buffer, sizeof(buffer), "%u\n", value);
	saved = override_creds(gpu_governor_file->f_cred);
	written = kernel_write(gpu_governor_file, buffer, length, &offset);
	revert_creds(saved);
	if (written != length) {
		last_error_stage = "gpu_mode_write";
		return written < 0 ? (int)written : -EIO;
	}
	error = read_gpu_mode(&readback);
	return error ? error : readback == value ? 0 : -EIO;
}

static int release_gpu_mode(void)
{
	u32 current_percent;
	int error;

	if (!gpu_mode_owned)
		return 0;
	error = read_gpu_mode(&current_percent);
	if (!error && current_percent == gpu_mode_last) {
		if (current_percent != gpu_mode_original)
			error = write_gpu_mode(gpu_mode_original);
	} else if (!error) {
		/* A different owner changed it: preserve that owner's value. */
		gpu_mode_conflict = true;
	}
	if (error)
		last_error = error;
	gpu_mode_owned = false;
	return error;
}

static int apply_gpu_mode(bool game)
{
	u32 desired = game ? configuration.gpu_game_percent : configuration.gpu_scene_percent;
	u32 current_percent;
	int error;

	if (!game && scene_table)
		desired = scene_profile ? scene_profile->gpu_percent : 0;
	if (observe_only() || !(configuration.flags & FP_GPU))
		return 0;
	if (!desired || display_off)
		return release_gpu_mode();
	error = read_gpu_mode(&current_percent);
	if (error)
		return error;
	if (!current_percent || current_percent > 200)
		return -ERANGE;
	if (gpu_mode_owned && current_percent != gpu_mode_last) {
		gpu_mode_conflict = true;
		gpu_mode_owned = false;
	}
	if (gpu_mode_conflict) {
		/* This file lease is independent of CPU/FAS and GPU frequency QoS.
		 * Preserve a foreign owner's value and yield only this lease. A real
		 * later context may share ownership when its desired value matches;
		 * never poll or write back merely to resolve an ownership conflict.
		 */
		if (current_percent != desired)
			return 0;
		gpu_mode_conflict = false;
	}
	if (!gpu_mode_owned)
		gpu_mode_original = current_percent;
	if (current_percent != desired) {
		error = write_gpu_mode(desired);
		if (error)
			return error;
	}
	gpu_mode_last = desired;
	gpu_mode_owned = true;
	return 0;
}

static void stop_pmus(void)
{
	unsigned int cpu;

	if (scene_trace) {
		unregister_trace_sched_switch(scene_switch, NULL);
		tracepoint_synchronize_unregister();
		scene_trace = false;
	}
	irq_work_sync(&scene_irq_work);
	/* This may be called by the same ordered policy queue. A nonblocking
	 * cancellation is sufficient: no two policy works run concurrently.
	 */
	cancel_delayed_work(&scene_feedback_deadline);
	if (!pmus)
		return;
	for_each_possible_cpu(cpu) {
		if (pmus[cpu].cycles)
			perf_event_release_kernel(pmus[cpu].cycles);
	}
	kfree(pmus);
	pmus = NULL;
}

static const struct fp_scene_aux_cpu *native_scene_cpu(unsigned int index)
{
	const struct fp_scene_aux_profile *profile = fp_scene_aux_for(foreground,
		source_game, display_off, scene_inactive);

	if (!profile || index >= configuration.cpu_count ||
	    !(profile->cpus[index].feedback_mask & FP_AUX_FEEDBACK_ADDITIVE) ||
	    !profile->cpus[index].additive_count)
		return NULL;
	return &profile->cpus[index];
}

static bool native_scene_feedback(void)
{
	unsigned int i;

	for (i = 0; i < configuration.cpu_count; i++)
		if (native_scene_cpu(i))
			return true;
	return false;
}

static int start_pmus(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_HARDWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_HW_CPU_CYCLES,
	};
	unsigned int cpu, i;
	unsigned long flags;
	int error;

	pmus = kcalloc(nr_cpu_ids, sizeof(*pmus), GFP_KERNEL);
	if (!pmus)
		return -ENOMEM;
	for (i = 0; i < configuration.cpu_count; i++) {
		struct cpufreq_policy *policy;
		const struct fp_scene_aux_cpu *native = native_scene_cpu(i);
		const struct fp_scene_aux_options *options = fp_scene_aux_options();

		if (!native && scene_profile && !scene_profile->cpus[i].margin_count)
			continue;
		fp_scene_limiter_history_init(&scene_history[i], native ? native->smoothness : 0,
			options && (options->valid_mask & FP_AUX_OPT_STAT_SOURCE) &&
			fp_scene_limiter_extended_window(options->stat_source, display_off, false));
		policy = cpufreq_cpu_get(configuration.cpus[i].cpu);

		if (!policy) {
			error = -ENODEV;
			goto fail;
		}
		for_each_cpu(cpu, policy->related_cpus) {
			struct fp_pmu *sample = &pmus[cpu];

			if (sample->cycles || !cpu_online(cpu) || (native && cpu < 64 &&
			    (native->excludes_mask & (1ULL << cpu))))
				continue;
#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
			if (test_scene_cpu_sample)
				continue; /* The real production PMU reader is replaced only by this fixture. */
#endif
			attr.config = PERF_COUNT_HW_CPU_CYCLES;
			sample->cycles = perf_event_create_kernel_counter(&attr, cpu, NULL, NULL, NULL);
			if (IS_ERR(sample->cycles)) {
				error = PTR_ERR(sample->cycles);
				sample->cycles = NULL;
				cpufreq_cpu_put(policy);
				goto fail;
			}
			sample->count = perf_event_read_value(sample->cycles, &sample->enabled,
							     &sample->running);
			sample->timestamp = ktime_get_ns();
		}
		cpufreq_cpu_put(policy);
	}
	/* Original 0x6a23c0 core manager is reused across app/activity changes.
	 * Its blend map, retained count and demand ring survive these changes.
	 * Replacing the actual typed snapshot resets it explicitly below.
	 */
	scene_core_applied = false;
	{
		const struct fp_scene_aux_options *options = fp_scene_aux_options();
		struct fp_scene_event_config events = {};

		if (native_scene_feedback() && options) {
			events.perf_notify_enabled = (options->valid_mask & FP_AUX_OPT_PERF_NOTIFY) &&
				options->perf_notify;
			events.kswapd_enabled = (options->valid_mask & FP_AUX_OPT_KSWAPD) &&
				options->kswapd_boost;
		}
		spin_lock_irqsave(&frame_lock, flags);
		if (scene_sched_events.epoch != scene_activity_epoch ||
		    scene_sched_events.watch_kswapd != (events.perf_notify_enabled && events.kswapd_enabled))
			fp_scene_event_reset(&scene_sched_events, scene_activity_epoch, &events);
		scene_panel_peak = options && (options->valid_mask & FP_AUX_OPT_PANEL_PEAK) ?
			options->panel_peak_hz : 0;
		spin_unlock_irqrestore(&frame_lock, flags);
	}
	error = register_trace_sched_switch(scene_switch, NULL);
	if (error)
		goto fail;
	scene_trace = true;
	atomic64_set(&next_scene_sample, 0);
	return 0;
fail:
	stop_pmus();
	return error;
}

static void remove_requests(void)
{
	unsigned int i;

	WRITE_ONCE(scene_active, false);
	stop_pmus();
	memset(scene_history, 0, sizeof(scene_history));
	memset(scene_votes, 0, sizeof(scene_votes));
	scene_gesture_live = scene_notify_live = false;
	scene_feedback_at = 0;
	if (gpu_device) {
		if (dev_pm_qos_request_active(&gpu_min))
			dev_pm_qos_remove_request(&gpu_min);
		if (dev_pm_qos_request_active(&gpu_max))
			dev_pm_qos_remove_request(&gpu_max);
		put_device(gpu_device);
		gpu_device = NULL;
	}
	for (i = 0; i < FP_MAX_CPUS; i++) {
		struct fp_cpu_request *request = &cpu_requests[i];

		if (!request->policy)
			continue;
		if (freq_qos_request_active(&request->min))
			freq_qos_remove_request(&request->min);
		if (freq_qos_request_active(&request->max))
			freq_qos_remove_request(&request->max);
		cpufreq_cpu_put(request->policy);
		request->policy = NULL;
		cpu_floors[i] = 0;
		cpu_ceilings[i] = 0;
	}
	gpu_floor = 0;
	scene_active = false;
	scene_profile = NULL;
}

static int add_requests_for(bool game, const struct fp_limits *game_limits)
{
	unsigned int i, j;
	int error;

	if (observe_only())
		return 0;
	if (configuration.flags & FP_CPU) {
		for (i = 0; i < configuration.cpu_count; i++) {
			const struct fp_cpu_config *cpu = &configuration.cpus[i];
			const struct fp_limits *limits = game ? &cpu->game :
				display_off ? &cpu->idle : &cpu->scene;
			struct fp_cpu_request *request = &cpu_requests[i];

			if (game_limits)
				limits = &game_limits[i];
			else if (!game && scene_profile)
				limits = &scene_profile->cpus[i].limits;
			request->policy = cpufreq_cpu_get(cpu->cpu);
			if (!request->policy) {
				error = -ENODEV;
				goto fail;
			}
			if (limits->max_khz > request->policy->cpuinfo.max_freq) {
				error = -ERANGE;
				goto fail;
			}
			for (j = 0; j < i; j++)
				if (cpumask_intersects(request->policy->related_cpus,
						      cpu_requests[j].policy->related_cpus)) {
					error = -EINVAL;
					goto fail;
				}
			error = freq_qos_add_request(&request->policy->constraints, &request->max,
						    FREQ_QOS_MAX, limits->max_khz);
			if (error < 0)
				goto fail;
			error = freq_qos_add_request(&request->policy->constraints, &request->min,
						    FREQ_QOS_MIN, limits->min_khz);
			if (error < 0)
				goto fail;
			cpu_floors[i] = limits->min_khz;
			cpu_ceilings[i] = limits->max_khz;
		}
	}
	if (configuration.flags & FP_GPU) {
		const struct fp_limits *limits = game ? &configuration.gpu_game :
			display_off ? &configuration.gpu_idle : &configuration.gpu_scene;

		if (!game && scene_profile)
			limits = &scene_profile->gpu;
		gpu_device = bus_find_device_by_name(&platform_bus_type, NULL,
						    configuration.gpu_device);
		if (!gpu_device) {
			error = -ENODEV;
			goto fail;
		}
		error = dev_pm_qos_add_request(gpu_device, &gpu_max, DEV_PM_QOS_MAX_FREQUENCY,
					      limits->max_khz);
		if (error < 0)
			goto fail;
		error = dev_pm_qos_add_request(gpu_device, &gpu_min, DEV_PM_QOS_MIN_FREQUENCY,
					      limits->min_khz);
		if (error < 0)
			goto fail;
		gpu_floor = limits->min_khz;
	}
	return 0;
fail:
	remove_requests();
	return error;
}

static int add_requests(bool game)
{
	return add_requests_for(game, NULL);
}

static int start_game_accounting(void)
{
	unsigned int i, cpu;
	int error;

	game_cpus = kcalloc(nr_cpu_ids, sizeof(*game_cpus), GFP_KERNEL);
	if (!game_cpus)
		return -ENOMEM;
	for (i = 0; i < configuration.cpu_count; i++) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(configuration.cpus[i].cpu);

		if (!policy)
			return -ENODEV;
		error = 0;
		for_each_cpu(cpu, policy->related_cpus) {
			if (game_cpus[cpu].policy_index) {
				error = -EINVAL;
				break;
			}
			game_cpus[cpu].policy_index = i + 1;
		}
		cpufreq_cpu_put(policy);
		if (error)
			return error;
	}
	game_sample_ns = ktime_get_ns();
	error = register_trace_sched_switch(game_switch, NULL);
	game_trace = !error;
	return error;
}

/* config_lock held. Frame producers only publish their own history and queue. */
static void reset_fas_samples(void)
{
	kvfree(fas_previous_tasks);
	fas_previous_tasks = NULL;
	fas_previous_count = 0;
	fas_task_sample_ns = 0;
	fas_task_attempt_ns = 0;
	fas_util_permille = 0;
	fas_util_valid = false;
	fas_critical_mask = 0;
}

static void reset_fas_feedback(void)
{
	fas_feedback = (struct fp_fas_feedback){};
	fas_effective_mfps = 0;
	fas_last_adjustment = 0;
}

static void stop_game(void)
{
	unsigned long flags;
	unsigned int i;

	/* Disable event consumption before waiting for registered handlers. */
	spin_lock_irqsave(&frame_lock, flags);
	game_active = false;
	reset_frame_history();
	fas_overflowed = false;
	render_cpu = -1;
	spin_unlock_irqrestore(&frame_lock, flags);
	cancel_delayed_work(&expire_work);
	if (game_trace) {
		unregister_trace_sched_switch(game_switch, NULL);
		tracepoint_synchronize_unregister();
		game_trace = false;
	}
	kfree(game_cpus);
	game_cpus = NULL;
	game_sample_ns = 0;
	reset_fas_samples();
	reset_fas_feedback();
	memset(game_cap_valid, 0, sizeof(game_cap_valid));
	memset(cpu_loads, 0, sizeof(cpu_loads));
	if (registered) {
		uprobe_unregister(file_inode(probe_file), configuration.probe_offset, &consumer);
		registered = false;
	}
	for (i = 0; i < FP_CALLS; i++) {
		struct dma_fence *fence;

		spin_lock_irqsave(&frame_lock, flags);
		fence = calls[i].fence;
		calls[i].fence = NULL;
		calls[i].tid = 0;
		spin_unlock_irqrestore(&frame_lock, flags);
		dma_fence_put(fence);
		/*
		 * No new callbacks can be armed after uprobe_unregister().
		 * Never acquire a fence lock while holding frame_lock.
		 */
		spin_lock_irqsave(&frame_lock, flags);
		fence = dma_fence_get(gpu_watches[i].fence);
		spin_unlock_irqrestore(&frame_lock, flags);
		if (!fence)
			continue;
		if (dma_fence_remove_callback(fence, &gpu_watches[i].callback)) {
			spin_lock_irqsave(&frame_lock, flags);
			gpu_watches[i].fence = NULL;
			spin_unlock_irqrestore(&frame_lock, flags);
			dma_fence_put(fence); /* The canceled callback's reference. */
		}
		dma_fence_put(fence);
	}
	if (probe_file) {
		fput(probe_file);
		probe_file = NULL;
	}
	if (target_mm) {
		mmput(target_mm);
		target_mm = NULL;
	}
	if (target) {
		put_task_struct(target);
		target = NULL;
	}
}

static struct task_struct *find_game(const char *package)
{
	struct task_struct **leaders, *task, *result = NULL;
	unsigned int capacity = READ_ONCE(nr_threads) + 32, count = 0, i;
	char name[FP_PACKAGE];
	bool full = false;

	leaders = kvmalloc_array(capacity, sizeof(*leaders), GFP_KERNEL);
	if (!leaders)
		return ERR_PTR(-ENOMEM);
	rcu_read_lock();
	for_each_process(task) {
		if (!READ_ONCE(task->mm) || (READ_ONCE(task->flags) & (PF_KTHREAD | PF_EXITING)))
			continue;
		if (count == capacity) {
			full = true;
			break;
		}
		get_task_struct(task);
		leaders[count++] = task;
	}
	rcu_read_unlock();
	if (!full) {
		for (i = 0; i < count; i++) {
			int length = get_cmdline(leaders[i], name, sizeof(name));

			if (length > strlen(package) && length < sizeof(name) &&
			    !memcmp(name, package, strlen(package) + 1)) {
				get_task_struct(leaders[i]);
				result = leaders[i];
				break;
			}
		}
	}
	for (i = 0; i < count; i++)
		put_task_struct(leaders[i]);
	kvfree(leaders);
	return full ? ERR_PTR(-EAGAIN) : result;
}

static bool probe_filter(struct uprobe_consumer *self, enum uprobe_filter_ctx context,
			 struct mm_struct *mm)
{
	return mm == READ_ONCE(target_mm);
}

static int probe_entry(struct uprobe_consumer *self, struct pt_regs *regs)
{
	struct fp_call *slot = NULL;
	struct dma_fence *fence = NULL;
	unsigned long flags;
	unsigned int i;
	pid_t tid = task_pid_nr(current);

	if ((int)regs->regs[2] >= 0 && current->mm == READ_ONCE(target_mm))
		fence = sync_file_get_fence((int)regs->regs[2]);
	spin_lock_irqsave(&frame_lock, flags);
	if (!game_active || applied_generation != generation || current->mm != target_mm)
		goto out;
	for (i = 0; i < FP_CALLS; i++) {
		if (calls[i].tid == tid) {
			/* Reentrant calls have no unambiguous outer Surface token. */
			calls[i].surface = 0;
			frame_errors++;
			goto out;
		}
		if (!slot && !calls[i].tid)
			slot = &calls[i];
	}
	if (slot) {
		*slot = (struct fp_call){
			.tid = tid,
			.start_time = current->start_boottime,
			.generation = generation,
			.surface = regs->regs[0],
			.entered_ns = ktime_get_ns(),
			.fence = fence,
			.gpu_ready = (int)regs->regs[2] == -1,
		};
		fence = NULL;
	} else {
		frame_errors++;
	}
out:
	spin_unlock_irqrestore(&frame_lock, flags);
	dma_fence_put(fence);
	return 0;
}

static void record_gpu_wait(u64 epoch, u64 rate_epoch, u64 entered_ns, u64 pending)
{
	unsigned long flags;

	spin_lock_irqsave(&frame_lock, flags);
	if (game_active && epoch == generation && epoch == applied_generation &&
	    ktime_get_ns() - entered_ns < FP_INACTIVITY_NS) {
		if (active_fps && rate_epoch == rate_generation)
			fp_control_gpu(&gpu_control, min_t(u64, pending, FP_INACTIVITY_NS),
				       active_fps);
		gpu_samples++;
	}
	spin_unlock_irqrestore(&frame_lock, flags);
}

static void record_gpu_sample(u64 epoch, u64 rate_epoch, u64 entered_ns,
			      struct dma_fence *fence)
{
	u64 finished = ktime_to_ns(dma_fence_timestamp(fence));

	record_gpu_wait(epoch, rate_epoch, entered_ns,
			finished > entered_ns ? finished - entered_ns : 0);
}

static void gpu_ready(struct dma_fence *fence, struct dma_fence_cb *callback)
{
	struct fp_fence_watch *watch = container_of(callback, struct fp_fence_watch, callback);
	unsigned long flags;

	/* DMA-fence invokes callbacks with its lock held; do not reacquire it. */
	if (!READ_ONCE(fence->error))
		record_gpu_sample(watch->generation, watch->rate_generation, watch->entered_ns, fence);
	spin_lock_irqsave(&frame_lock, flags);
	watch->fence = NULL;
	spin_unlock_irqrestore(&frame_lock, flags);
	dma_fence_put(fence);
}

/* The fence reference is consumed on every path. */
static void watch_gpu(struct dma_fence *fence, u64 epoch, u64 rate_epoch,
		      u64 entered_ns, bool ready)
{
	struct fp_fence_watch *watch = NULL;
	unsigned long flags;
	unsigned int i;
	int error;

	if (!fence) {
		/* Android uses fd=-1 for an already-ready buffer, not missing data. */
		if (ready) {
			record_gpu_wait(epoch, rate_epoch, entered_ns, 0);
			return;
		}
		spin_lock_irqsave(&frame_lock, flags);
		gpu_unknown++;
		spin_unlock_irqrestore(&frame_lock, flags);
		return;
	}
	spin_lock_irqsave(&frame_lock, flags);
	if (game_active && epoch == generation) {
		for (i = 0; i < FP_CALLS; i++) {
			if (!gpu_watches[i].fence) {
				watch = &gpu_watches[i];
				watch->generation = epoch;
				watch->rate_generation = rate_epoch;
				watch->entered_ns = entered_ns;
				watch->fence = fence;
				break;
			}
		}
	}
	if (!watch)
		gpu_unknown++;
	spin_unlock_irqrestore(&frame_lock, flags);
	if (!watch) {
		dma_fence_put(fence);
		return;
	}
	error = dma_fence_add_callback(fence, &watch->callback, gpu_ready);
	if (!error)
		return;
	if (error == -ENOENT && dma_fence_get_status(fence) > 0)
		record_gpu_sample(epoch, rate_epoch, entered_ns, fence);
	spin_lock_irqsave(&frame_lock, flags);
	watch->fence = NULL;
	spin_unlock_irqrestore(&frame_lock, flags);
	dma_fence_put(fence);
}

/* frame_lock held. Stale ring slots are inaccessible after resetting count. */
static void reset_frame_history(void)
{
	unsigned int i;

	fp_control_reset(&gpu_control);
	active_fps = 0;
	rate_generation++;
	selected_stream = 0;
	last_frame_ns = 0;
	last_frame_interval_ns = 0;
	last_frame_margin_mfps = 0;
	late_frame_ns = 0;
	late_rate_generation = 0;
	late_sent = false;
	last_work_ns = 0;
	floor_level = 0;
	fas_events.head = fas_events.count = 0;
	for (i = 0; i < FP_STREAMS; i++) {
		struct fp_stream *stream = &streams[i];

		stream->surface = stream->hits = stream->previous_ns = 0;
		fp_control_reset(&stream->control);
		fp_rate_reset(&stream->rate);
		fp_fas_history_reset(&stream->history);
	}
}

/* frame_lock held. A full queue is fatal even if cadence changes before work. */
static bool enqueue_fas_frame(struct fp_fas_event event)
{
	if (fas_overflowed)
		return false;
	if (fp_fas_event_push(&fas_events, event))
		return true;
	fas_overflowed = true;
	fas_overflows++;
	frame_errors++;
	return false;
}

static int probe_return(struct uprobe_consumer *self, unsigned long function,
			struct pt_regs *regs)
{
	u64 now, surface = 0, epoch = 0;
	u64 entered_ns = 0, rate_epoch = 0, interval_ns = 0;
	struct dma_fence *fence = NULL;
	unsigned long flags;
	unsigned int i, selected = FP_STREAMS, old_selected, fps;
	bool schedule = false, use_fence = false, gpu_ready_now = false;
	pid_t tid = task_pid_nr(current);

	spin_lock_irqsave(&frame_lock, flags);
	now = ktime_get_ns();
	for (i = 0; i < FP_CALLS; i++) {
		if (calls[i].tid != tid)
			continue;
		if (calls[i].start_time == current->start_boottime) {
			surface = calls[i].surface;
			epoch = calls[i].generation;
			entered_ns = calls[i].entered_ns;
			gpu_ready_now = calls[i].gpu_ready;
		}
		fence = calls[i].fence;
		calls[i].fence = NULL;
		calls[i].tid = 0;
		break;
	}
	if (!surface || !game_active || epoch != generation || current->mm != target_mm)
		goto out;
	if ((long)regs_return_value(regs)) {
		frame_errors++;
		goto out;
	}
	frames_seen++;
	if (last_frame_ns && now - last_frame_ns >= FP_INACTIVITY_NS)
		reset_frame_history();
	/* Fence tracking is independent of CPU frametime error. */
	for (i = 0; i < FP_STREAMS; i++) {
		if (streams[i].surface == surface || !streams[i].surface) {
			selected = i;
			break;
		}
	}
	/* Surface recreation must not leave the controller tied to an old layer. */
	if (selected == FP_STREAMS) {
		for (i = 0; i < FP_STREAMS; i++)
			if (now - streams[i].previous_ns >= FP_INACTIVITY_NS) {
				struct fp_stream *stream = &streams[i];

				selected = i;
				stream->hits = stream->previous_ns = 0;
				fp_control_reset(&stream->control);
				fp_rate_reset(&stream->rate);
				fp_fas_history_reset(&stream->history);
				break;
			}
	}
	if (selected == FP_STREAMS) {
		frame_errors++;
		goto out;
	}
	old_selected = selected_stream;
	streams[selected].surface = surface;
	streams[selected].hits++;
	if (streams[selected].hits > streams[selected_stream].hits ||
	    now - streams[selected_stream].previous_ns >= 250000000ULL)
		selected_stream = selected;
	if (streams[selected].previous_ns) {
		struct fp_stream *stream = &streams[selected];
		u64 dt = now - stream->previous_ns;
		u32 previous_fps = stream->rate.target;

		fps = fp_rate_sample(&stream->rate, dt, active_game_fps, active_game_fps_count);
		if (fps != previous_fps) {
			fp_control_reset(&stream->control);
			fp_fas_history_reset(&stream->history);
		}
		if (fps)
			fp_control_frame(&stream->control, dt, fps);
		fp_fas_history_push(&stream->history, dt, fps);
		interval_ns = dt;
	}
	streams[selected].previous_ns = now;
	if (selected != selected_stream)
		goto out;
	fps = streams[selected].rate.target;
	if (old_selected != selected_stream || active_fps != fps) {
		active_fps = fps;
		rate_generation++;
		fp_control_reset(&gpu_control);
		/* A surface/FPS change cannot consume the previous grade's feedback. */
		fas_events.head = fas_events.count = 0;
		if (old_selected != selected_stream)
			fp_fas_history_reset(&streams[selected].history);
	}
	last_frame_interval_ns = interval_ns;
	last_frame_margin_mfps = fp_fas_margin(&fas_tuning, fps);
	late_sent = false;
	if ((configuration.flags & FP_CPU) && fps && interval_ns && interval_ns < FP_INACTIVITY_NS &&
	    streams[selected].history.count >= FP_FAS_MIN_INTERVALS && !fas_overflowed) {
		struct fp_fas_event event = {
			.ended_ns = now, .interval_ns = interval_ns, .fps = fps,
			.janked = fp_fas_history_janked(&streams[selected].history, fps),
			.margin_mfps = last_frame_margin_mfps,
		};

		if (!enqueue_fas_frame(event))
			schedule = true;
	}
	rate_epoch = rate_generation;
	render_cpu = raw_smp_processor_id();
	use_fence = true;
	last_frame_ns = now;
	floor_level = streams[selected].control.level;
	frames_used++;
	if (now - last_work_ns >= FP_FRAME_WORK_NS) {
		last_work_ns = now;
		schedule = true;
	}
out:
	spin_unlock_irqrestore(&frame_lock, flags);
	if (use_fence)
		watch_gpu(fence, epoch, rate_epoch, entered_ns, gpu_ready_now);
	else
		dma_fence_put(fence);
	if (schedule)
		queue_work(policy_wq, &frame_work);
	return 0;
}

static int start_game(void)
{
	const struct fp_game_rule *rule = active_game_rule();
	u8 bytes[sizeof(configuration.probe_bytes)];
	loff_t offset = configuration.probe_offset;
	unsigned long flags;
	ssize_t count;
	int error;

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
	{
		struct sab_status status;

		scene_activity_backend_status(&status);
		test_activity_present_at_game_start = status.registered || status.deadline_valid;
	}
#endif
	target = find_game(foreground);
	if (IS_ERR(target)) {
		error = PTR_ERR(target);
		target = NULL;
		return error;
	}
	if (!target)
		return 0; /* The process exec event will retry this transition. */
	target_mm = get_task_mm(target);
	if (!target_mm) {
		error = -ESRCH;
		goto fail;
	}
	probe_file = filp_open("/system/lib64/libgui.so", O_RDONLY | O_LARGEFILE, 0);
	if (IS_ERR(probe_file)) {
		error = PTR_ERR(probe_file);
		probe_file = NULL;
		goto fail;
	}
	count = kernel_read(probe_file, bytes, sizeof(bytes), &offset);
	if (count != sizeof(bytes) || memcmp(bytes, configuration.probe_bytes, sizeof(bytes))) {
		error = -ESTALE;
		goto fail;
	}
	spin_lock_irqsave(&frame_lock, flags);
	active_game_fps_count = rule && rule->fps_count ? rule->fps_count : configuration.fps_count;
	memcpy(active_game_fps, rule && rule->fps_count ? rule->fps_values : configuration.fps_values,
	       sizeof(active_game_fps));
	memset(calls, 0, sizeof(calls));
	reset_frame_history();
	render_cpu = -1;
	game_active = true;
	spin_unlock_irqrestore(&frame_lock, flags);
	if (configuration.flags & FP_CPU) {
		error = start_game_accounting();
		if (error)
			goto fail;
	}
	error = uprobe_register(file_inode(probe_file), configuration.probe_offset, &consumer);
	if (!error) {
		registered = true;
		return 0;
	}
fail:
	stop_game();
	return error;
}

static void scene_activity_changed(u64 epoch, bool inactive)
{
	unsigned long flags;
	bool changed = false;

	/* Backend notifier never takes config_lock: sync teardown cannot self-deadlock. */
	spin_lock_irqsave(&frame_lock, flags);
	if (epoch == scene_activity_epoch && READ_ONCE(configured) && pending_scene.valid &&
	    !pending_scene.game && !pending_scene.off && pending_scene.inactive != inactive) {
		pending_scene.inactive = inactive;
		generation++;
		scene_inactive_events += inactive;
		changed = true;
	}
	spin_unlock_irqrestore(&frame_lock, flags);
	if (changed)
		irq_work_queue(&activity_control_irq);
}

static void activity_control_irq_fn(struct irq_work *work)
{
	/* Input event_lock -> activity_lock -> frame_lock never reaches pool locks. */
	if (READ_ONCE(configured))
		queue_work(policy_wq, &control_work);
}

static void scene_feedback_changed(u64 epoch)
{
	/* Called under the input backend lock, potentially input_dev.event_lock.
	 * Only irq_work crosses into policy workqueue pool locks.
	 */
	if (epoch == READ_ONCE(scene_activity_epoch) && READ_ONCE(scene_active))
		irq_work_queue(&scene_irq_work);
}

static bool gpu_aux_allowed(void)
{
	return configured && source_valid && (configuration.flags & FP_GPU) &&
		!observe_only() && !display_off && (source_game || scene_profile || !scene_table);
}

/* config_lock held. Own ordered backend queue permits synchronous cancellation. */
static void reconcile_scene_activity(void)
{
	const struct fp_scene_profile *active, *inactive;
	struct sa_context context = {};
	struct fp_scene_event_config events = {};
	const struct fp_scene_aux_options *options = fp_scene_aux_options();
	unsigned long flags;
	bool current_source, boundary_pending;

	spin_lock_irqsave(&frame_lock, flags);
	context.epoch = scene_activity_epoch;
	current_source = applied_generation == generation;
	boundary_pending = scene_gesture_boundary;
	spin_unlock_irqrestore(&frame_lock, flags);
	context.valid = source_valid;
	context.screen_off = display_off;
	context.game = source_game;
	events.gesture_enabled = options && (options->valid_mask & FP_AUX_OPT_GESTURE) &&
		options->gesture_boost;
	/* A newer ordinary foreground already queued control. Leave its panel
	 * attached while this stale snapshot finishes dropping frequency votes.
	 */
	if (!current_source && !boundary_pending && configured && context.valid &&
	    !context.game && !context.screen_off && (configuration.flags & FP_CPU) &&
	    !observe_only() && events.gesture_enabled)
		return;
	/* These are explicit launcher package identities, never a windowed app guess. */
	context.launcher = !strcmp(foreground, "com.android.launcher") ||
		!strcmp(foreground, "com.oplus.launcher");
	active = fp_scene_select(scene_table, foreground, source_game, display_off, false);
	inactive = fp_scene_select(scene_table, foreground, source_game, display_off, true);
	context.enabled = configured && current_source && scene_table &&
		(fp_scene_activity_needed(active, inactive, configuration.flags, configuration.cpu_count) ||
		 fp_scene_aux_activity_needed(foreground, source_game, display_off,
			(configuration.flags & FP_CPU) && !observe_only() && (active || inactive)) ||
		 ((configuration.flags & FP_CPU) && !observe_only() && native_scene_feedback()));
	scene_activity_backend_configure(&context,
		configuration.cpu_count ? configuration.cpus[0].cpu : 0);
	if (context.enabled && !context.game && !context.screen_off && native_scene_feedback() &&
	    (configuration.flags & FP_CPU) && !observe_only() && options) {
		events.gesture_duration_ms = options->gesture_duration_ms;
		scene_activity_backend_set_feedback(context.epoch, &events, scene_feedback_changed);
	} else {
		scene_activity_backend_set_feedback(0, NULL, NULL);
		spin_lock_irqsave(&frame_lock, flags);
		fp_scene_event_reset(&scene_sched_events, 0, NULL);
		scene_panel_peak = 0;
		spin_unlock_irqrestore(&frame_lock, flags);
	}
}

static void apply_control(struct work_struct *work)
{
	typeof(pending_scene) next;
	unsigned long flags;
	u64 epoch;
	bool aux_applied = false;
	bool clear_gesture;

	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	next = pending_scene;
	epoch = generation;
	clear_gesture = scene_gesture_boundary;
	scene_gesture_boundary = false;
	spin_unlock_irqrestore(&frame_lock, flags);
	if (clear_gesture)
		scene_activity_backend_set_feedback(0, NULL, NULL);
	if (applied_generation == epoch && target &&
	    !(READ_ONCE(target->flags) & PF_EXITING) && READ_ONCE(target->mm) == target_mm)
		goto out;
	/*
	 * This is the sole transition point. All old requests are removed before
	 * either branch starts. Queued frame work checks the new generation.
	 */
	stop_game();
	remove_requests();
	applied_generation = epoch;
	strscpy(foreground, next.package, sizeof(foreground));
	source_valid = next.valid;
	source_game = next.game;
	display_off = next.off;
	scene_inactive = next.inactive;
	if (configured) {
		last_error = 0;
		last_error_stage = "none";
	}
	switches++;
	if (!configured || !source_valid) {
		release_gpu_mode();
		goto out;
	}
	if (source_game && !display_off) {
		/* Transfer ownership before installing even the first game request. */
		reconcile_scene_activity();
		fp_scene_aux_apply(foreground, true, false, false, false, epoch, &generation);
		aux_applied = true;
		last_error = start_game();
	} else {
		scene_profile = fp_scene_select(scene_table, foreground, source_game,
					       display_off, next.inactive);
		/* An explicit ignored profile owns no CPU, GPU or PMU resources. */
		if (scene_table && !scene_profile) {
			last_error = release_gpu_mode();
			if (last_error)
				configured = false;
			goto out;
		}
		last_error = add_requests(false);
		if (!last_error && !display_off && (configuration.flags & FP_CPU) &&
		    (!scene_table || fp_scene_uses_feedback(scene_profile, configuration.cpu_count) ||
		     native_scene_feedback()))
			last_error = start_pmus();
		if (last_error)
			remove_requests();
		scene_active = !last_error;
	}
	if (epoch != READ_ONCE(generation)) {
		stop_game();
		remove_requests();
	} else if (!last_error) {
		last_error = apply_gpu_mode(source_game && !display_off);
	}
	if (last_error) {
		stop_game();
		remove_requests();
		release_gpu_mode();
		configured = false;
	}
out:
	if (!aux_applied)
		fp_scene_aux_apply(foreground, source_game, display_off, next.inactive,
			configured && source_valid && !observe_only() &&
			(configuration.flags & FP_CPU) && scene_profile &&
			epoch == READ_ONCE(generation), epoch, &generation);
	if (scene_active && !display_off && native_scene_feedback() &&
	    scene_core.demand.capacity != 3 && scene_profile) {
		const struct fp_scene_aux_cpu *native = native_scene_cpu(0);
		u32 map, retained;

		if (native && native->limiter_cluster_count &&
		    !fp_scene_aux_core_seed(&map, &retained) &&
		    fp_scene_limiter_sun_core_init(&scene_core, retained))
			scene_core.enabled_clusters = map;
	}
	/* Global anti-boost is independent of Scene/FAS frequency ownership.
	 * It can coexist with FAS GPU QoS, and never writes a thermal/max vote.
	 */
	gpu_aux_error = fp_scene_gpu_aux_apply(gpu_aux_allowed(), epoch, &generation);
	reconcile_scene_activity();
	mutex_unlock(&config_lock);
}

static void scene_switch(void *data, bool preempt, struct task_struct *previous,
			 struct task_struct *next, unsigned int prev_state)
{
	u64 now, due;
	unsigned long flags;
	bool pulse = false;

	if (!READ_ONCE(scene_active) || READ_ONCE(display_off) || !READ_ONCE(pmus) ||
	    READ_ONCE(applied_generation) != READ_ONCE(generation))
		return;
	now = ktime_get_mono_fast_ns();
	if (READ_ONCE(scene_sched_events.watch_kswapd)) {
		spin_lock_irqsave(&frame_lock, flags);
		pulse = fp_scene_event_sched_switch(&scene_sched_events, scene_activity_epoch, now,
			scene_panel_peak,
			(READ_ONCE(previous->flags) & PF_KTHREAD) && !strcmp(previous->comm, "kswapd0"),
			(READ_ONCE(next->flags) & PF_KTHREAD) && !strcmp(next->comm, "kswapd0"));
		spin_unlock_irqrestore(&frame_lock, flags);
	}
	due = atomic64_read(&next_scene_sample);
	if (pulse || (now >= due &&
	    atomic64_cmpxchg(&next_scene_sample, due, now + 80000000ULL) == due))
		irq_work_queue(&scene_irq_work);
}

static void scene_irq(struct irq_work *work)
{
	/* sched_switch holds rq->lock: never take workqueue/pool locks there. */
	if (READ_ONCE(scene_active) && !READ_ONCE(display_off))
		queue_work(policy_wq, &scene_work);
}

static void expire_scene_feedback(struct work_struct *work)
{
	if (READ_ONCE(scene_active) && !READ_ONCE(display_off))
		queue_work(policy_wq, &scene_work);
}

static void apply_scene(struct work_struct *work)
{
	u32 effective[FP_MAX_CPUS] = {};
	struct sab_status activity;
	struct fp_scene_event_snapshot input_events, scheduler_events;
	const struct fp_scene_aux_profile *aux_profile;
	const struct fp_scene_aux_options *options;
	unsigned int i, cpu;
	u64 now;
	u64 epoch, deadline;
	unsigned long flags;
	u32 core_demand = 0;
	bool launcher, enabled, special_peak, have_native = false;

	mutex_lock(&config_lock);
	if (!scene_active || display_off || !pmus || applied_generation != generation)
		goto out;
	now = ktime_get_ns();
	epoch = applied_generation;
	scene_activity_backend_status(&activity);
	scene_activity_backend_feedback(scene_activity_epoch, now, &input_events);
	spin_lock_irqsave(&frame_lock, flags);
	scheduler_events = fp_scene_event_snapshot(&scene_sched_events, scene_activity_epoch, now);
	spin_unlock_irqrestore(&frame_lock, flags);
	scene_gesture_live = input_events.gesture_active;
	scene_notify_live = scheduler_events.notify_active;
	enabled = fp_scene_limiter_input_recent(now, activity.last_input_ns,
		activity.contacts, FP_SCENE_LIMITER_SMOOTH_RECENT_NS) || input_events.gesture_active;
	special_peak = input_events.gesture_active || (enabled && scheduler_events.notify_active);
	launcher = !strcmp(foreground, "com.android.launcher") ||
		!strcmp(foreground, "com.oplus.launcher");
	aux_profile = fp_scene_aux_for(foreground, source_game, display_off, scene_inactive);
	options = fp_scene_aux_options();
	for (i = 0; i < configuration.cpu_count; i++) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(configuration.cpus[i].cpu);
		const struct fp_scene_aux_cpu *native = native_scene_cpu(i);
		u32 ceiling, sample_count = 0;
		int error;
		bool have_data = false;

		if (!policy)
			continue;
		for_each_cpu(cpu, policy->related_cpus) {
			struct fp_pmu *sample = &pmus[cpu];
			u64 count, enabled, running, cycles, dt, denabled, drunning;

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
			if (test_scene_cpu_sample) {
				u32 khz;

				if (test_scene_cpu_sample(cpu, &khz)) {
					effective[i] = max(effective[i], khz);
					if (sample_count < FP_SCENE_LIMITER_MAX_CPUS)
						scene_cpu_samples[sample_count++] =
							(struct fp_scene_limiter_cpu_sample){cpu, khz, true};
					have_data = true;
				}
				continue;
			}
#endif
			if (!sample->cycles)
				continue;
			count = perf_event_read_value(sample->cycles, &enabled, &running);
			cycles = count - sample->count;
			dt = now - sample->timestamp;
			denabled = enabled - sample->enabled;
			drunning = running - sample->running;
			if (dt && drunning && denabled && count >= sample->count &&
			    enabled >= sample->enabled && running >= sample->running) {
				u64 scaled = mul_u64_u64_div_u64(cycles, denabled, drunning);
				u64 khz = mul_u64_u64_div_u64(scaled, 1000000, dt);

				effective[i] = max(effective[i],
					(u32)min_t(u64, khz, policy->cpuinfo.max_freq));
				if (sample_count < FP_SCENE_LIMITER_MAX_CPUS)
					scene_cpu_samples[sample_count++] =
						(struct fp_scene_limiter_cpu_sample){cpu,
						min_t(u64, khz, policy->cpuinfo.max_freq), true};
				have_data = true;
			}
			sample->count = count;
			sample->enabled = enabled;
			sample->running = running;
			sample->timestamp = now;
		}
		if (!have_data || observe_only() || !cpu_requests[i].policy) {
			cpufreq_cpu_put(policy);
			continue;
		}
		if (!native && scene_profile && !scene_profile->cpus[i].margin_count) {
			cpufreq_cpu_put(policy);
			continue;
		}
		if (native) {
			struct fp_scene_limiter_load load;
			struct fp_scene_limiter_margin_step margins[FP_MAX_MARGINS];
			const struct fp_limits *limits = &scene_profile->cpus[i].limits;
			u32 mt, perfect, smooth, j;
			s32 margin;

			mt = fp_scene_limiter_mt(native->mt_milli_percent, i,
				fp_scene_limiter_core_blend(&scene_core, i,
					native->limiter_cluster_count != 0), enabled);
			if (!fp_scene_limiter_aggregate(scene_cpu_samples, sample_count,
				native->excludes_mask, mt, &load)) {
				cpufreq_cpu_put(policy);
				continue;
			}
			perfect = fp_scene_limiter_sun_perfect(native->perfect_khz,
				policy->cpuinfo.max_freq, limits->max_khz);
			smooth = fp_scene_limiter_smooth(&scene_history[i], load.mixed_khz,
				perfect, native->prefer, enabled, launcher, special_peak);
			for (j = 0; j < native->additive_count; j++)
				margins[j] = (struct fp_scene_limiter_margin_step){
					native->additive[j].threshold_khz, native->additive[j].offset_khz};
			/* Both Sun clusters use the non-bottom generic peak margin. A
			 * contact by itself never impersonates a PerfNotify pulse.
			 */
			margin = special_peak ? 650000 :
				fp_scene_limiter_margin(smooth, margins, native->additive_count, 0);
			/* Sun's two physical clusters have no inter-cluster peer gate.
			 * The original consumes pre-OPP estimates, then clamps/rounds
			 * the CPU vote independently. Exclusions never change affinity.
			 */
			ceiling = fp_scene_limiter_estimate(&scene_votes[i], smooth,
				margin, perfect, 0, enabled);
			core_demand = max(core_demand, ceiling);
			have_native = true;
			ceiling = snap_game_ceiling(policy,
				clamp(ceiling, limits->min_khz, limits->max_khz), limits);
			if (!ceiling) {
				cpufreq_cpu_put(policy);
				last_error = -ERANGE;
				last_error_stage = "scene_cpu_opp";
				remove_requests();
				release_gpu_mode();
				configured = false;
				break;
			}
			ceiling = fp_scene_limiter_vote(&scene_votes[i], ceiling);
			scene_native_samples++;
		} else {
			ceiling = scene_profile ?
				fp_scene_profile_ceiling(effective[i], &scene_profile->cpus[i]) :
				fp_scene_ceiling(effective[i], &configuration.cpus[i]);
		}
		cpufreq_cpu_put(policy);
		if (epoch != READ_ONCE(generation))
			break;
		if (ceiling == cpu_ceilings[i])
			continue;
		error = freq_qos_update_request(&cpu_requests[i].max, ceiling);
		if (error < 0) {
			last_error = error;
			last_error_stage = "scene_cpu_qos";
			remove_requests();
			release_gpu_mode();
			configured = false;
			break;
		}
		cpu_ceilings[i] = ceiling;
	}
	if (configured && scene_active && have_native && aux_profile && options && configuration.cpu_count == 2 &&
	    (options->valid_mask & FP_AUX_OPT_SOURCE) &&
	    aux_profile->cpus[0].limiter_cluster_count && epoch == READ_ONCE(generation)) {
		const struct fp_scene_aux_cpu *first = &aux_profile->cpus[0];
		bool smooth_core = fp_scene_limiter_input_recent(now, activity.last_input_ns,
			activity.contacts, FP_SCENE_LIMITER_CORE_RECENT_NS) || input_events.gesture_active;
		struct fp_scene_limiter_core_result result = fp_scene_limiter_sun_core_step(
			&scene_core, options->source, core_demand, first->limiter_cluster_count,
			first->limiter_clusters_packed, input_events.force_core, smooth_core);

		if (result.applicable && (!scene_core_applied || scene_core_mask != result.active_clusters ||
		    scene_core_restore != result.restore_all)) {
			struct fp_aux_status aux;
			u32 j;

			fp_scene_aux_core_update(result.active_clusters, result.restore_all, epoch, &generation);
			fp_scene_aux_status(&aux);
			/* A yielded foreign group has no verified native blend state.
			 * Keep its external policy, and use peak demand conservatively.
			 */
			for (j = 0; j < configuration.cpu_count; j++)
				if (aux.conflict_mask & (1U << (j * FP_SCENE_AUX_KNOBS + FP_AUX_KNOB_CORE_ENABLE)))
					scene_core.enabled_clusters &= ~(1U << j);
			scene_core_applied = true;
			scene_core_mask = result.active_clusters;
			scene_core_restore = result.restore_all;
		}
	}
	/* Two independent pulses share one absolute one-shot deadline. A further
	 * interval exists only if a real input/scheduler event queued it.
	 */
	deadline = input_events.next_deadline_ns;
	if (!deadline || (scheduler_events.next_deadline_ns && scheduler_events.next_deadline_ns < deadline))
		deadline = scheduler_events.next_deadline_ns;
	scene_feedback_at = deadline;
	if (deadline && scene_active && epoch == READ_ONCE(generation))
		mod_delayed_work(policy_wq, &scene_feedback_deadline,
			deadline > now ? nsecs_to_jiffies(deadline - now) + 1 : 0);
	else
		cancel_delayed_work(&scene_feedback_deadline);
	scene_samples++;
out:
	if (!configured) {
		fp_scene_aux_apply(NULL, false, false, false, false, 0, NULL);
		gpu_aux_error = fp_scene_gpu_aux_apply(false, 0, NULL);
		reconcile_scene_activity();
	}
	mutex_unlock(&config_lock);
}

static void game_switch(void *data, bool preempt, struct task_struct *previous,
			struct task_struct *next, unsigned int prev_state)
{
	struct fp_game_cpu *sample;
	unsigned long flags;
	u64 runtime;

	/* Called under rq->lock: arithmetic only, no workqueue/PMU/QoS calls. */
	if (!READ_ONCE(game_active))
		return;
	sample = &game_cpus[raw_smp_processor_id()];
	if (previous->mm != READ_ONCE(target_mm) && next->mm != READ_ONCE(target_mm) &&
	    !READ_ONCE(sample->running))
		return;
	spin_lock_irqsave(&frame_lock, flags);
	if (!game_active || !game_cpus || applied_generation != generation)
		goto out;
	sample = &game_cpus[raw_smp_processor_id()];
	if (sample->running == previous) {
		runtime = READ_ONCE(previous->se.sum_exec_runtime);
		if (runtime >= sample->running_runtime)
			sample->runtime_ns += runtime - sample->running_runtime;
	}
	sample->running = NULL;
	if (next->mm == target_mm) {
		sample->running_runtime = READ_ONCE(next->se.sum_exec_runtime);
		sample->running = next;
	}
	game_switches++;
out:
	spin_unlock_irqrestore(&frame_lock, flags);
}

/* config_lock and frame_lock held. Includes a running thread without a scan. */
static void sample_game_load(u64 now)
{
	u64 dt = game_sample_ns && now > game_sample_ns ? now - game_sample_ns : 0;
	unsigned int i, cpu;

	memset(cpu_loads, 0, sizeof(cpu_loads));
	if (!game_cpus)
		return;
	for (i = 0; i < configuration.cpu_count; i++) {
		for_each_possible_cpu(cpu) {
			struct fp_game_cpu *sample = &game_cpus[cpu];
			u64 runtime = sample->runtime_ns, delta;

			if (sample->policy_index != i + 1)
				continue;
			if (sample->running) {
				u64 live = READ_ONCE(sample->running->se.sum_exec_runtime);

				if (live >= sample->running_runtime)
					runtime += live - sample->running_runtime;
			}
			delta = runtime >= sample->previous_ns ? runtime - sample->previous_ns : 0;
			sample->previous_ns = runtime;
			if (dt)
				cpu_loads[i] = max(cpu_loads[i],
					(u32)mul_u64_u64_div_u64(min(delta, dt), 1000, dt));
		}
	}
	game_sample_ns = now;
}

static int compare_fas_tasks(const void *a, const void *b)
{
	return fp_fas_task_identity(a, b);
}

/* Process context only. Copy values under RCU; never allocate or take rq locks
 * there. A PID/start-time pair, not a borrowed task pointer, survives the read. */
static int snapshot_fas_tasks(struct fp_fas_task_sample **out, u32 *out_count)
{
	struct fp_fas_task_sample *samples;
	struct task_struct *task;
	u32 capacity, count, attempt;
	int nr_threads;
	bool full;

	if (!target || nr_cpu_ids > 64)
		return -ENODEV;
	/* target's task reference pins its signal struct. nr_threads is signed. */
	nr_threads = READ_ONCE(target->signal->nr_threads);
	capacity = (u32)max(nr_threads, 1) + 16U;
	for (attempt = 0; attempt < 3; attempt++) {
		samples = kvmalloc_array(capacity, sizeof(*samples), GFP_KERNEL);
		if (!samples)
			return -ENOMEM;
		count = 0;
		full = false;
		rcu_read_lock();
		for_each_thread(target, task) {
			if (READ_ONCE(task->mm) != target_mm || (READ_ONCE(task->flags) & PF_EXITING))
				continue;
			if (count == capacity) {
				full = true;
				break;
			}
			samples[count++] = (struct fp_fas_task_sample){
				.tid = task_pid_nr(task), .cpu = task_cpu(task),
				.start_time = task->start_boottime,
				.runtime_ns = READ_ONCE(task->se.sum_exec_runtime),
				.allowed_mask = READ_ONCE(cpumask_bits(&task->cpus_mask)[0]),
			};
		}
		rcu_read_unlock();
		if (!full) {
			sort(samples, count, sizeof(*samples), compare_fas_tasks, NULL);
			*out = samples;
			*out_count = count;
			return 0;
		}
		kvfree(samples);
		if (capacity > U32_MAX / 2)
			return -EOVERFLOW;
		capacity *= 2;
	}
	return -EAGAIN;
}

static void sample_fas_tasks(u64 now)
{
	struct fp_fas_task_sample *next;
	struct fp_fas_task_load result;
	u64 previous_attempt = max(fas_task_attempt_ns, fas_task_sample_ns);
	u32 count;
	int error;

	if (previous_attempt && (now <= previous_attempt || now - previous_attempt < 300000000ULL))
		return;
	if (fas_task_sample_ns && now - fas_task_sample_ns >= FP_INACTIVITY_NS)
		reset_fas_samples();
	fas_task_attempt_ns = now;
	error = snapshot_fas_tasks(&next, &count);
	if (error) {
		fas_task_sample_errors++;
		fas_util_permille = 0;
		fas_util_valid = false;
		fas_critical_mask = 0;
		return;
	}
	if (fas_task_sample_ns) {
		fp_fas_task_measure(fas_previous_tasks, fas_previous_count, next, count,
				    now - fas_task_sample_ns, &result);
		fas_util_valid = result.valid;
		fas_util_permille = result.util_permille;
		fas_critical_mask = result.critical_mask;
		fas_task_rollbacks += result.rollback;
		if (result.error != FP_FAS_TASK_OK)
			fas_task_sample_errors++;
	}
	kvfree(fas_previous_tasks);
	fas_previous_tasks = next;
	fas_previous_count = count;
	fas_task_sample_ns = now;
	fas_task_samples++;
}

static u32 snap_game_ceiling(struct cpufreq_policy *policy, u32 target_khz,
			    const struct fp_limits *limits)
{
	struct cpufreq_frequency_table *entry;
	u32 above = U32_MAX, highest = 0;

	if (!policy->freq_table)
		return target_khz; /* A driver with continuous frequency selection. */
	cpufreq_for_each_valid_entry(entry, policy->freq_table) {
		u32 frequency = entry->frequency;

		if (frequency < limits->min_khz || frequency > limits->max_khz)
			continue;
		highest = max(highest, frequency);
		if (frequency >= target_khz)
			above = min(above, frequency);
	}
	return above == U32_MAX ? highest : above;
}

/* Always order min/max changes so our own intermediate bounds stay valid. */
static int update_cpu_bounds(unsigned int index, u32 floor, u32 ceiling)
{
	struct fp_cpu_request *request = &cpu_requests[index];
	int error;

	if (ceiling > cpu_ceilings[index]) {
		error = freq_qos_update_request(&request->max, ceiling);
		if (error < 0)
			return error;
		cpu_ceilings[index] = ceiling;
	}
	if (floor != cpu_floors[index]) {
		error = freq_qos_update_request(&request->min, floor);
		if (error < 0)
			return error;
		cpu_floors[index] = floor;
	}
	if (ceiling != cpu_ceilings[index]) {
		error = freq_qos_update_request(&request->max, ceiling);
		if (error < 0)
			return error;
		cpu_ceilings[index] = ceiling;
	}
	return 0;
}

/* config_lock protects both this table and the active foreground identity. */
static const struct fp_game_rule *active_game_rule(void)
{
	unsigned int i;

	if (!game_rules)
		return NULL;
	for (i = 0; i < game_rules->count; i++)
		if (!strcmp(game_rules->rules[i].package, foreground))
			return &game_rules->rules[i];
	return &game_rules->fallback;
}

/* Each selected event has its own P step. Only frequency writes are coalesced. */
static bool run_fas_batch(const struct fp_fas_event *events, unsigned int count,
			  u32 targets[FP_MAX_CPUS], const struct fp_limits bounds[FP_MAX_CPUS],
			  const struct fp_game_rule *rule, unsigned int cpu_count, u32 actual_max,
			  struct fp_fas_batch_state *state)
{
	u32 hardware_max = 0;
	unsigned int i, step;

	for (i = 0; i < cpu_count; i++)
		hardware_max = max(hardware_max, bounds[i].max_khz);
	for (step = 0; step < count; step++) {
		const struct fp_fas_event *event = &events[step];
		u32 previous_max = 0, common;

		if (!event->late)
			fp_fas_util_event(&state->feedback, event->ended_ns,
					  state->util_valid ? state->util_permille : 0);
		state->effective_mfps = fp_fas_effective_rate_margin(&state->feedback,
							     event->fps, event->margin_mfps);
		state->last_adjustment = fp_fas_proportional(event->interval_ns, state->effective_mfps);
		if (event->janked)
			state->util_valid = false;
		for (i = 0; i < cpu_count; i++)
			previous_max = max(previous_max, targets[i]);
		common = fp_fas_common_target(previous_max, actual_max, state->util_permille,
					      state->util_valid && actual_max, event->janked,
					      state->last_adjustment, hardware_max);
		for (i = 0; i < cpu_count; i++)
			targets[i] = common;
		if (rule) {
			if (!fp_apply_relations(targets, bounds, rule->cpus, cpu_count))
				return false;
		} else {
			for (i = 0; i < cpu_count; i++)
				targets[i] = clamp(targets[i], bounds[i].min_khz, bounds[i].max_khz);
		}
	}
	return true;
}

static bool frame_batch_current(u64 epoch, u64 rate_epoch)
{
	return epoch == READ_ONCE(generation) && rate_epoch == READ_ONCE(rate_generation);
}

/* Caller holds config_lock and frame_lock. */
static bool late_recovery_eligible(void)
{
	return (configuration.flags & FP_CPU) && !observe_only() && active_fps &&
	       streams[selected_stream].history.count >= FP_FAS_MIN_INTERVALS && last_frame_interval_ns &&
	       !late_sent && !fas_overflowed;
}

static u64 late_recovery_deadline(void)
{
	return last_frame_ns + max(FP_LATE_MIN_NS, div_u64(2ULL * NSEC_PER_SEC, active_fps));
}

static void arm_frame_deadline(u64 deadline, u64 now)
{
	mod_delayed_work(policy_wq, &expire_work,
			 deadline > now ? nsecs_to_jiffies(deadline - now) + 1 : 0);
}

/* Only the worker that saw this real submission may arm its late deadline. */
static void arm_frame_expiry(u64 epoch, u64 rate_epoch, u64 last)
{
	unsigned long flags;
	u64 deadline;

	spin_lock_irqsave(&frame_lock, flags);
	if (!game_active || !frame_batch_current(epoch, rate_epoch)) {
		spin_unlock_irqrestore(&frame_lock, flags);
		return;
	}
	deadline = last_frame_ns + FP_INACTIVITY_NS;
	if (last == last_frame_ns && late_recovery_eligible()) {
		late_frame_ns = last;
		late_rate_generation = rate_epoch;
		deadline = late_recovery_deadline();
	}
	spin_unlock_irqrestore(&frame_lock, flags);
	arm_frame_deadline(deadline, ktime_get_ns());
}

/* Called by expiry under both locks, with the inactivity deadline as fallback. */
static bool queue_late_frame(u64 now, u64 *deadline)
{
	u64 elapsed, due;

	if (applied_generation != generation || late_frame_ns != last_frame_ns ||
	    late_rate_generation != rate_generation || !late_recovery_eligible())
		return false;
	due = late_recovery_deadline();
	if (now < due) {
		*deadline = due;
		return false;
	}
	elapsed = now - last_frame_ns;
	if (elapsed >= FP_INACTIVITY_NS)
		return false;
	late_sent = true;
	enqueue_fas_frame((struct fp_fas_event){
		.ended_ns = now, .interval_ns = max(elapsed, last_frame_interval_ns),
		.fps = active_fps, .late = true, .margin_mfps = last_frame_margin_mfps,
		.janked = fp_fas_history_janked_elapsed(&streams[selected_stream].history,
						       active_fps, elapsed),
	});
	/* Queue the worker even if enqueue failed, so it can unwind on overflow. */
	return true;
}

static void apply_frame(struct work_struct *work)
{
	unsigned long flags;
	unsigned int i, gpu_level, event_count = 0, real_count = 0;
	u64 now, last, epoch, rate_epoch;
	int error;
	u32 actual_max = 0;
	u32 targets[FP_MAX_CPUS] = {};
	struct fp_limits bounds[FP_MAX_CPUS], requested[FP_MAX_CPUS];
	struct fp_fas_batch_state batch = {};
	bool warm;

	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	now = ktime_get_ns();
	last = last_frame_ns;
	epoch = generation;
	rate_epoch = rate_generation;
	if (!game_active || applied_generation != epoch) {
		spin_unlock_irqrestore(&frame_lock, flags);
		goto out;
	}
	if (fas_overflowed) {
		spin_unlock_irqrestore(&frame_lock, flags);
		goto overflow;
	}
	if (!last || now - last >= FP_INACTIVITY_NS) {
		spin_unlock_irqrestore(&frame_lock, flags);
		goto out;
	}
	gpu_level = gpu_control.level;
	warm = active_fps && streams[selected_stream].history.count >= FP_FAS_MIN_INTERVALS;
	while (event_count < ARRAY_SIZE(fas_batch) && fp_fas_event_pop(&fas_events, &fas_batch[event_count])) {
		if (!fas_batch[event_count].late)
			real_count++;
		event_count++;
	}
	if (!event_count || real_count)
		sample_game_load(now);
	spin_unlock_irqrestore(&frame_lock, flags);
	if (fas_last_rate_generation != rate_epoch) {
		reset_fas_feedback();
		fas_last_rate_generation = rate_epoch;
	}
	if (!warm) {
		/* Unknown or warming cadence owns no frequency votes. */
		remove_requests();
		goto arm;
	}
	if (event_count) {
		if (real_count)
			sample_fas_tasks(now);
		batch = (struct fp_fas_batch_state) {
			.feedback = fas_feedback,
			.util_permille = fas_util_permille,
			.util_valid = fas_util_valid,
		};
		for (i = 0; i < configuration.cpu_count; i++) {
			const struct fp_cpu_config *cpu = &configuration.cpus[i];
			struct cpufreq_policy *policy = cpu_requests[i].policy;
			u32 actual, policy_cpu;

			if (!policy)
				policy = cpufreq_cpu_get(cpu->cpu);
			if (!policy) {
				error = -ENODEV;
				goto failed;
			}
			bounds[i].min_khz = max(cpu->game.min_khz, policy->cpuinfo.min_freq);
			bounds[i].max_khz = cpu->game.max_khz;
			if (bounds[i].min_khz > bounds[i].max_khz ||
			    bounds[i].max_khz > policy->cpuinfo.max_freq) {
				if (!cpu_requests[i].policy)
					cpufreq_cpu_put(policy);
				error = -ERANGE;
				goto failed;
			}
			if (batch.util_valid && !fas_batch[0].janked) {
				/* Match scaling_cur_freq's architecture-first read. */
				policy_cpu = READ_ONCE(policy->cpu);
				actual = arch_freq_get_on_cpu(policy_cpu);
				if (!actual)
					actual = cpufreq_quick_get(policy_cpu);
				actual_max = max(actual_max, actual);
			}
			if (!cpu_requests[i].policy)
				cpufreq_cpu_put(policy);
			targets[i] = game_cap_valid[i] ? game_targets[i] : bounds[i].max_khz;
		}
		if (!run_fas_batch(fas_batch, event_count, targets, bounds, active_game_rule(),
				   configuration.cpu_count, actual_max, &batch)) {
			error = -EINVAL;
			goto failed;
		}
	}
	if (!frame_batch_current(epoch, rate_epoch))
		goto stale;
	if (READ_ONCE(fas_overflowed))
		goto overflow;
	if (observe_only())
		goto publish;
	for (i = 0; event_count && i < configuration.cpu_count; i++) {
		struct cpufreq_policy *policy = cpu_requests[i].policy;

		if (!policy)
			policy = cpufreq_cpu_get(configuration.cpus[i].cpu);
		if (!policy) {
			error = -ENODEV;
			goto failed;
		}
		requested[i].max_khz = snap_game_ceiling(policy, targets[i], &bounds[i]);
		requested[i].min_khz = fas_critical_mask & cpumask_bits(policy->related_cpus)[0] ?
			requested[i].max_khz : bounds[i].min_khz;
		if (!cpu_requests[i].policy)
			cpufreq_cpu_put(policy);
		if (!requested[i].max_khz) {
			error = -ERANGE;
			goto failed;
		}
	}
	if (!frame_batch_current(epoch, rate_epoch))
		goto stale;
	if (!cpu_requests[0].policy && (configuration.flags & FP_CPU) && !event_count)
		goto arm;
	if ((!cpu_requests[0].policy && (configuration.flags & FP_CPU)) ||
	    (!gpu_device && (configuration.flags & FP_GPU))) {
		error = add_requests_for(true, event_count ? requested : NULL);
		if (error)
			goto failed;
	} else {
		for (i = 0; event_count && i < configuration.cpu_count; i++) {
			if (!frame_batch_current(epoch, rate_epoch))
				goto stale;
			error = update_cpu_bounds(i, requested[i].min_khz, requested[i].max_khz);
			if (error < 0)
				goto failed;
		}
	}
	if (!frame_batch_current(epoch, rate_epoch))
		goto stale;
	if (gpu_device && (!event_count || real_count)) {
		const struct fp_limits *limits = &configuration.gpu_game;
		u32 floor = fp_control_floor(gpu_level, limits->min_khz, limits->max_khz);

		if (floor != gpu_floor) {
			error = dev_pm_qos_update_request(&gpu_min, floor);
			if (error < 0)
				goto failed;
			gpu_floor = floor;
		}
	}
publish:
	spin_lock_irqsave(&frame_lock, flags);
	if (!frame_batch_current(epoch, rate_epoch)) {
		spin_unlock_irqrestore(&frame_lock, flags);
		goto stale;
	}
	if (fas_overflowed) {
		spin_unlock_irqrestore(&frame_lock, flags);
		goto overflow;
	}
	if (event_count) {
		for (i = 0; i < configuration.cpu_count; i++) {
			game_targets[i] = targets[i];
			game_cap_valid[i] = true;
		}
		fas_feedback = batch.feedback;
		fas_effective_mfps = batch.effective_mfps;
		fas_last_adjustment = batch.last_adjustment;
		fas_util_valid = batch.util_valid;
		fas_steps += event_count;
		fas_late_steps += event_count - real_count;
	}
	spin_unlock_irqrestore(&frame_lock, flags);
arm:
	arm_frame_expiry(epoch, rate_epoch, last);
	goto out;
stale:
	remove_requests();
	goto arm;
overflow:
	error = -EOVERFLOW;
failed:
	if (epoch != READ_ONCE(generation))
		goto stale;
	last_error = error;
	last_error_stage = error == -EOVERFLOW ? "fas_queue_overflow" : "frame_frequency";
	stop_game();
	remove_requests();
	release_gpu_mode();
	configured = false;
out:
	if (!configured)
		reconcile_scene_activity();
	mutex_unlock(&config_lock);
}

static void expire_frame(struct work_struct *work)
{
	unsigned long flags;
	u64 last, now, deadline;
	bool queued;

	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	if (!game_active || applied_generation != generation) {
		spin_unlock_irqrestore(&frame_lock, flags);
		goto out;
	}
	now = ktime_get_ns();
	last = last_frame_ns;
	if (last && now - last < FP_INACTIVITY_NS) {
		deadline = last + FP_INACTIVITY_NS;
		queued = queue_late_frame(now, &deadline);
		spin_unlock_irqrestore(&frame_lock, flags);
		arm_frame_deadline(deadline, now);
		if (queued)
			queue_work(policy_wq, &frame_work);
		goto out;
	}
	reset_frame_history();
	render_cpu = -1;
	spin_unlock_irqrestore(&frame_lock, flags);
	remove_requests();
	reset_fas_feedback();
	reset_fas_samples();
out:
	mutex_unlock(&config_lock);
}

void frame_policy_scene_full(const char *package, bool off, bool game, u64 source_epoch)
{
	unsigned long flags;
	bool valid = package && *package;

	/* The affinity/event provider must never wait for PMU or probe teardown. */
	spin_lock_irqsave(&frame_lock, flags);
	if (pending_scene.valid == valid && pending_scene.game == (valid && game) &&
	    pending_scene.off == off &&
	    !strncmp(pending_scene.package, valid ? package : "", sizeof(pending_scene.package))) {
		if (pending_scene.provider_epoch == source_epoch || (valid && game && !off)) {
			/* Keep latest visible/windowed epoch for a later Scene transition,
			 * but never reprobe a continuing game's unchanged owner/package.
			 */
			pending_scene.provider_epoch = source_epoch;
			spin_unlock_irqrestore(&frame_lock, flags);
			return;
		}
	}
	pending_scene.valid = valid;
	strscpy(pending_scene.package, pending_scene.valid ? package : "",
		 sizeof(pending_scene.package));
	pending_scene.game = pending_scene.valid && game;
	pending_scene.off = off;
	pending_scene.inactive = false;
	pending_scene.provider_epoch = source_epoch;
	if (!valid || off || game)
		scene_gesture_boundary = true;
	scene_activity_epoch++;
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	if (policy_wq && READ_ONCE(configured))
		queue_work(policy_wq, &control_work);
}

void frame_policy_scene(const char *package, bool off, bool game)
{
	frame_policy_scene_full(package, off, game, 0);
}

void frame_policy_task_event(struct task_struct *task)
{
	unsigned long flags;
	unsigned int i;

	if (!READ_ONCE(configured))
		return;
	if (READ_ONCE(task->flags) & PF_EXITING) {
		struct dma_fence *pending_fence = NULL;

		spin_lock_irqsave(&frame_lock, flags);
		for (i = 0; i < FP_CALLS; i++)
			if (calls[i].tid == task_pid_nr(task) &&
			    calls[i].start_time == task->start_boottime) {
				pending_fence = calls[i].fence;
				calls[i].fence = NULL;
				calls[i].tid = 0;
				break;
			}
		spin_unlock_irqrestore(&frame_lock, flags);
		dma_fence_put(pending_fence);
	}
	if (!policy_wq || task != task->group_leader || !READ_ONCE(source_game))
		return;
	if (!READ_ONCE(target) || READ_ONCE(target) == task)
		queue_work(policy_wq, &control_work);
}

static bool valid_limits(const struct fp_limits *limits)
{
	return limits->min_khz <= limits->max_khz &&
		limits->max_khz > 0 && limits->max_khz <= 10000000;
}

static int validate_configuration(const struct fp_config *config)
{
	unsigned int i, j;
	if (config->abi != FP_ABI || !config->flags || (config->flags & ~FP_FLAGS) ||
	    !config->fps_count || config->fps_count > FP_MAX_RATES ||
	    !config->cpu_count || config->cpu_count > FP_MAX_CPUS ||
	    config->target_fps < 15 || config->target_fps > 240 ||
	    !config->probe_offset || (config->probe_offset & 3) ||
	    config->probe_offset > S64_MAX ||
	    !memchr(config->gpu_device, 0, sizeof(config->gpu_device)))
		return -EINVAL;
	for (i = 0; i < FP_MAX_RATES; i++) {
		u32 fps = config->fps_values[i];

		if (i >= config->fps_count) {
			if (fps)
				return -EINVAL;
			continue;
		}
		if (fps < 15 || fps > config->target_fps ||
		    (i && fps <= config->fps_values[i - 1]))
			return -EINVAL;
	}
	if (config->fps_values[config->fps_count - 1] != config->target_fps)
		return -EINVAL;
	if ((config->flags & FP_GPU) && (!config->gpu_device[0] ||
	    !config->gpu_scene_percent || config->gpu_scene_percent > 200 ||
	    !config->gpu_game_percent || config->gpu_game_percent > 200 ||
	    config->gpu_percent_fd < 0 ||
	    !valid_limits(&config->gpu_scene) || !valid_limits(&config->gpu_game) ||
	    !valid_limits(&config->gpu_idle)))
		return -EINVAL;
	for (i = 0; i < config->cpu_count; i++) {
		const struct fp_cpu_config *cpu = &config->cpus[i];

		if (cpu->reserved || cpu->reserved2 || cpu->cpu >= nr_cpu_ids ||
		    !cpu_possible(cpu->cpu) || !cpu->margin_count ||
		    cpu->margin_count > FP_MAX_MARGINS ||
		    !valid_limits(&cpu->scene) || !valid_limits(&cpu->game) ||
		    !valid_limits(&cpu->idle))
			return -EINVAL;
		for (j = 0; j < cpu->margin_count; j++) {
			if (!cpu->margins[j].percent || cpu->margins[j].percent > 1000 ||
			    (!j && cpu->margins[j].threshold_khz) ||
			    (j && cpu->margins[j].threshold_khz <= cpu->margins[j - 1].threshold_khz))
				return -EINVAL;
		}
		for (j = 0; j < i; j++)
			if (config->cpus[j].cpu == cpu->cpu)
				return -EINVAL;
	}
	return 0;
}

static bool rules_match_cpus(const struct fp_game_rules *rules, const struct fp_config *config)
{
	unsigned int i;

	if (rules->cpu_count != config->cpu_count)
		return false;
	for (i = 0; i < rules->cpu_count; i++)
		if (rules->cpu_ids[i] != config->cpus[i].cpu)
			return false;
	return true;
}

static int validate_game_rules(const struct fp_game_rules *rules)
{
	u32 order[FP_MAX_CPUS];
	unsigned int i, j;

	if (rules->abi != FP_GAME_RULES_ABI || rules->count > FP_MAX_GAME_RULES ||
	    !rules->cpu_count || rules->cpu_count > FP_MAX_CPUS || rules->reserved ||
	    rules->fallback.package[0])
		return -EINVAL;
	for (i = 0; i < FP_MAX_CPUS; i++) {
		if (i >= rules->cpu_count) {
			if (rules->cpu_ids[i])
				return -EINVAL;
			continue;
		}
		if (rules->cpu_ids[i] >= nr_cpu_ids || !cpu_possible(rules->cpu_ids[i]))
			return -EINVAL;
		for (j = 0; j < i; j++)
			if (rules->cpu_ids[i] == rules->cpu_ids[j])
				return -EINVAL;
	}
	for (i = 0; i <= rules->count; i++) {
		const struct fp_game_rule *rule = i ? &rules->rules[i - 1] : &rules->fallback;
		const char *name = rule->package;

		if (!memchr(name, 0, sizeof(rule->package)) || (i && !*name) ||
		    !fp_relation_order(rule->cpus, rules->cpu_count, order) || rule->fps_count > FP_MAX_RATES)
			return -EINVAL;
		for (j = 0; j < FP_MAX_RATES; j++) {
			u32 fps = rule->fps_values[j];

			if (j >= rule->fps_count) {
				if (fps)
					return -EINVAL;
			} else if (fps < 15 || fps > 240 || (j && fps <= rule->fps_values[j - 1])) {
				return -EINVAL;
			}
		}
		for (; *name; name++)
			if (!isalnum(*name) && *name != '.' && *name != '_')
				return -EINVAL;
		for (j = 0; i && j + 1 < i; j++)
			if (!strcmp(rule->package, rules->rules[j].package))
				return -EINVAL;
		for (j = rules->cpu_count; j < FP_MAX_CPUS; j++)
			if (rule->cpus[j].kind || rule->cpus[j].reference ||
			    rule->cpus[j].minimum_khz || rule->cpus[j].maximum_khz)
				return -EINVAL;
	}
	return 0;
}

static int replace_game_rules(unsigned long arg)
{
	struct fp_game_rules *next, *previous;
	unsigned long flags;
	int error;

	next = kvmalloc(sizeof(*next), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	if (copy_from_user(next, (void __user *)arg, sizeof(*next))) {
		kvfree(next);
		return -EFAULT;
	}
	error = validate_game_rules(next);
	if (error) {
		kvfree(next);
		return error;
	}
	mutex_lock(&config_lock);
	if (configuration.cpu_count && !rules_match_cpus(next, &configuration)) {
		mutex_unlock(&config_lock);
		kvfree(next);
		return -EINVAL;
	}
	stop_game();
	remove_requests();
	previous = game_rules;
	game_rules = next;
	scene_activity_backend_set_feedback(0, NULL, NULL);
	last_error = 0;
	last_error_stage = "none";
	spin_lock_irqsave(&frame_lock, flags);
	scene_activity_epoch++;
	pending_scene.inactive = false;
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	kvfree(previous);
	queue_work(policy_wq, &control_work);
	flush_work(&control_work);
	mutex_lock(&config_lock);
	error = last_error;
	mutex_unlock(&config_lock);
	return error;
}

static int replace_scene_table(unsigned long arg)
{
	struct fp_scene_table *next, *previous;
	unsigned long flags;
	unsigned int i;
	int error;

	next = kvmalloc(sizeof(*next), GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	if (copy_from_user(next, (void __user *)arg, sizeof(*next))) {
		kvfree(next);
		return -EFAULT;
	}
	if (!fp_scene_table_valid(next)) {
		kvfree(next);
		return -EINVAL;
	}
	for (i = 0; i < next->cpu_count; i++) {
		if (next->cpu_ids[i] >= nr_cpu_ids || !cpu_possible(next->cpu_ids[i])) {
			kvfree(next);
			return -EINVAL;
		}
	}
	mutex_lock(&config_lock);
	if (configuration.cpu_count && !fp_scene_topology_matches(next, &configuration)) {
		mutex_unlock(&config_lock);
		kvfree(next);
		return -EINVAL;
	}
	error = fp_scene_gpu_aux_release();
	if (error) {
		gpu_aux_error = error;
		mutex_unlock(&config_lock);
		kvfree(next);
		return error;
	}
	error = fp_scene_aux_clear();
	if (error) {
		gpu_aux_error = fp_scene_gpu_aux_apply(gpu_aux_allowed(), generation, &generation);
		mutex_unlock(&config_lock);
		kvfree(next);
		return error;
	}
	fp_scene_gpu_aux_commit(NULL);
	gpu_aux_error = 0;
	stop_game();
	remove_requests();
	previous = scene_table;
	scene_table = next;
	scene_table_epoch++;
	scene_activity_backend_set_feedback(0, NULL, NULL);
	memset(&scene_core, 0, sizeof(scene_core));
	last_error = 0;
	last_error_stage = "none";
	spin_lock_irqsave(&frame_lock, flags);
	scene_activity_epoch++;
	pending_scene.inactive = false;
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	kvfree(previous);
	queue_work(policy_wq, &control_work);
	flush_work(&control_work);
	mutex_lock(&config_lock);
	error = last_error;
	mutex_unlock(&config_lock);
	return error;
}

static int replace_scene_aux(unsigned long arg)
{
	struct fp_scene_aux_request request;
	struct fp_scene_aux_table *next;
	struct fp_scene_gpu_aux *gpu_candidate = NULL;
	struct aux_runtime *cpu_candidate = NULL;
	struct fp_aux_status aux;
	unsigned long flags;
	int error;

	if (copy_from_user(&request, (void __user *)arg, sizeof(request)))
		return -EFAULT;
	if (request.abi != FP_SCENE_AUX_ABI || request.flags || request.reserved ||
	    request.length != sizeof(*next) || request.length > FP_SCENE_AUX_MAX_BYTES ||
	    !request.user_ptr)
		return -EINVAL;
	next = kvzalloc(request.length, GFP_KERNEL);
	if (!next)
		return -ENOMEM;
	if (copy_from_user(next, u64_to_user_ptr(request.user_ptr), request.length)) {
		kvfree(next);
		return -EFAULT;
	}
	if (!fp_scene_aux_table_valid(next, true)) {
		kvfree(next);
		return -EINVAL;
	}
	/* Fixed KGSL resources are opened under the real root ioctl caller,
	 * before publication. Neither backend touches old values at prepare.
	 */
	error = fp_scene_gpu_aux_prepare(next->options.gpu_lock, &gpu_candidate);
	if (error) {
		kvfree(next);
		return error;
	}
	mutex_lock(&config_lock);
	error = fp_scene_aux_prepare(next, scene_table, scene_table_epoch, &cpu_candidate);
	if (!error)
		error = fp_scene_gpu_aux_release();
	if (!error) {
		error = fp_scene_aux_commit(next, cpu_candidate);
		if (error)
			gpu_aux_error = fp_scene_gpu_aux_apply(gpu_aux_allowed(), generation, &generation);
	}
	if (!error) {
		fp_scene_gpu_aux_commit(gpu_candidate);
		gpu_candidate = NULL;
		cpu_candidate = NULL;
		gpu_aux_error = 0;
		memset(&scene_core, 0, sizeof(scene_core));
		scene_activity_backend_set_feedback(0, NULL, NULL);
		spin_lock_irqsave(&frame_lock, flags);
		scene_activity_epoch++;
		pending_scene.inactive = false;
		generation++;
		spin_unlock_irqrestore(&frame_lock, flags);
	}
	mutex_unlock(&config_lock);
	if (error) {
		fp_scene_aux_discard(cpu_candidate);
		fp_scene_gpu_aux_discard(gpu_candidate);
		kvfree(next);
		return error;
	}
	queue_work(policy_wq, &control_work);
	flush_work(&control_work);
	mutex_lock(&config_lock);
	fp_scene_aux_status(&aux);
	error = aux.error ? aux.error : gpu_aux_error;
	mutex_unlock(&config_lock);
	return error;
}

static int set_fas_tuning(const struct fp_fas_tuning *next)
{
	unsigned long flags;

	if (!fp_fas_tuning_valid(next))
		return -EINVAL;
	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	fas_tuning = *next;
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	return 0;
}

static long policy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fp_config next;
	struct file *governor = NULL;
	unsigned long flags;
	int error = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
	if (cmd == FP_GAME_RULES)
		return replace_game_rules(arg);
	if (cmd == FP_SCENE_TABLE)
		return replace_scene_table(arg);
	if (cmd == FP_SCENE_AUX)
		return replace_scene_aux(arg);
	if (cmd == FP_FAS_TUNING) {
		struct fp_fas_tuning tuning;

		if (copy_from_user(&tuning, (void __user *)arg, sizeof(tuning)))
			return -EFAULT;
		return set_fas_tuning(&tuning);
	}
	if (cmd != FP_CONFIG && cmd != FP_DISABLE)
		return -ENOTTY;
	if (cmd == FP_CONFIG) {
		if (copy_from_user(&next, (void __user *)arg, sizeof(next)))
			return -EFAULT;
		error = validate_configuration(&next);
		if (error)
			return error;
		if (next.flags & FP_GPU) {
			governor = fget(next.gpu_percent_fd);
			if (!governor)
				return -EBADF;
			if ((governor->f_mode & (FMODE_READ | FMODE_WRITE)) != (FMODE_READ | FMODE_WRITE) ||
			    strcmp(file_inode(governor)->i_sb->s_type->name, "sysfs") ||
			    strcmp((const char *)governor->f_path.dentry->d_name.name, "mod_percent")) {
				fput(governor);
				return -EINVAL;
			}
		}
	}
	mutex_lock(&config_lock);
	if (cmd == FP_CONFIG &&
	    ((game_rules && !rules_match_cpus(game_rules, &next)) ||
	     (scene_table && !fp_scene_topology_matches(scene_table, &next)))) {
		mutex_unlock(&config_lock);
		if (governor)
			fput(governor);
		return -EINVAL;
	}
	stop_game();
	remove_requests();
	error = release_gpu_mode();
	if (gpu_governor_file)
		fput(gpu_governor_file);
	gpu_governor_file = governor;
	gpu_mode_owned = false;
	gpu_mode_conflict = false;
	gpu_mode_last = 0;
	if (governor && !error) {
		error = read_gpu_mode(&gpu_mode_original);
		if (!error && (!gpu_mode_original || gpu_mode_original > 200))
			error = -ERANGE;
	}
	if (error && gpu_governor_file) {
		fput(gpu_governor_file);
		gpu_governor_file = NULL;
	}
	if (cmd == FP_CONFIG && !error)
		configuration = next;
	configured = cmd == FP_CONFIG && !error;
	scene_activity_backend_set_feedback(0, NULL, NULL);
	last_error = error;
	if (!error)
		last_error_stage = "none";
	spin_lock_irqsave(&frame_lock, flags);
	scene_activity_epoch++;
	pending_scene.inactive = false;
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	queue_work(policy_wq, &control_work);
	flush_work(&control_work);
	if (!error) {
		struct fp_aux_status aux;

		mutex_lock(&config_lock);
		fp_scene_aux_status(&aux);
		error = last_error ? last_error : aux.error ? aux.error : gpu_aux_error;
		if (!error && cmd == FP_DISABLE) {
			error = fp_scene_aux_clear();
			if (!error)
				fp_scene_gpu_aux_commit(NULL);
		}
		mutex_unlock(&config_lock);
	}
	return error;
}

#define FP_STATUS_SIZE 4096

static int format_policy_status(char *status, size_t capacity, const struct sab_status *activity)
{
	struct fp_aux_status aux;
	struct fp_scene_gpu_aux_status gpu_aux;
	unsigned long flags;
	u64 frames, used, errors, epoch, gpu_count, gpu_missing, gpu_average, overflows, inactive_events;
	unsigned int level, gpu_level, fps;
	int length, renderer;

	mutex_lock(&config_lock);
	fp_scene_aux_status(&aux);
	fp_scene_gpu_aux_status(&gpu_aux);
	spin_lock_irqsave(&frame_lock, flags);
	frames = frames_seen;
	used = frames_used;
	errors = frame_errors;
	epoch = generation;
	level = floor_level;
	gpu_level = gpu_control.level;
	fps = active_fps;
	gpu_average = gpu_control.average_ns;
	gpu_count = gpu_samples;
	gpu_missing = gpu_unknown;
	overflows = fas_overflows;
	renderer = render_cpu;
	inactive_events = scene_inactive_events;
	spin_unlock_irqrestore(&frame_lock, flags);
	length = scnprintf(status, capacity,
		"abi=%u\nconfigured=%u\nobserve=%u\nowner=%s\nforeground=%s\n"
		"features=cpu,scene_pmu,game_frames,gpu,gpu_file_credentials,game_relations,fps_per_game,scene_table,scene_activity,scene_aux,fas_common_p,fas_tuning,fas_task_util\n"
		"game_pid=%d\nprobe=%u\ngeneration=%llu\napplied_generation=%llu\n"
		"frames=%llu\nselected_frames=%llu\nframe_errors=%llu\nlevel=%u\n"
		"qualified_fps=%u\n"
		"fas_steps=%llu\nfas_late_steps=%llu\nfas_overflows=%llu\n"
		"fas_effective_mfps=%u\nfas_last_adjustment=%lld\n"
		"fas_mode=%u\nfas_margin_base_mfps=%u\nfas_util_offset_mfps=%d\n"
		"fas_task_samples=%llu\nfas_task_sample_errors=%llu\nfas_task_rollbacks=%llu\n"
		"fas_util_valid=%u\nfas_util_permille=%u\nfas_critical_mask=%016llx\nrender_cpu=%d\n"
		"gpu_samples=%llu\ngpu_unknown=%llu\ngpu_level=%u\ngpu_pending_ns=%llu\n"
		"cpu0_floor_khz=%u\ncpu1_floor_khz=%u\ngpu_floor_khz=%u\n"
		"cpu0_ceiling_khz=%u\ncpu1_ceiling_khz=%u\n"
		"cpu0_busy_permille=%u\ncpu1_busy_permille=%u\ngame_sched_hook=%u\n"
		"cpu_requests=%u\ngpu_requests=%u\nscene_pmu=%u\nsource_valid=%u\n"
		"gpu_mode_owned=%u\ngpu_mode_percent=%u\ngpu_mode_conflict=%u\n"
		"game_constraint_rules=%u\n"
		"scene_policy_rules=%u\nscene_profile=%d\nscene_inactive_events=%llu\nscene_activity_epoch=%llu\nscene_activity_phase=%u\n"
		"scene_input_registered=%u\nscene_input_devices=%u\nscene_input_reliable=%u\nscene_contacts=%u\n"
		"scene_input_events=%llu\nscene_activity_transitions=%llu\nscene_load_samples=%llu\nscene_load_errors=%llu\n"
		"scene_load_permille=%u\nscene_activity_deadline=%llu\nscene_activity_error=%d\n"
		"scene_table_epoch=%llu\naux_loaded=%u\naux_profile=%d\naux_error=%d\naux_stage=%s\n"
		"aux_cpu=%d\naux_knob=%d\naux_owned=%u\naux_conflicts=%u\naux_writes=%llu\naux_rollbacks=%llu\n"
		"aux_owned_mask=%08x\naux_conflict_mask=%08x\n"
		"aux_abi=%u\naux_table_bytes=%zu\naux_feedback_supported=1\n"
		"scene_native_samples=%llu\nscene_native_feedback=%u\nscene_core_enabled_mask=%x\n"
		"scene_core_retained=%u\nscene_gesture_pulse=%u\nscene_notify_pulse=%u\nscene_feedback_deadline_ns=%llu\n"
		"gpu_aux_prepared=%u\ngpu_aux_target=%u\ngpu_aux_owned_mask=%x\ngpu_aux_conflict_mask=%x\n"
		"gpu_aux_satisfied_mask=%x\ngpu_aux_error=%d\ngpu_aux_stage=%s\ngpu_aux_knob=%d\n"
		"gpu_aux_rollback_error=%d\ngpu_aux_writes=%llu\ngpu_aux_restores=%llu\n"
		"switches=%llu\nscene_samples=%llu\nlast_error=%d\nlast_error_stage=%s\n",
		FP_ABI, configured, observe_only(), game_active ? "frame" : scene_active ? "scene" : "idle",
		foreground, target ? task_pid_nr(target) : 0, registered, epoch,
		applied_generation, frames, used, errors, level, fps,
		fas_steps, fas_late_steps, overflows, fas_effective_mfps, fas_last_adjustment,
		fas_tuning.mode, fas_tuning.margin_base_mfps, fas_feedback.util_offset_mfps,
		fas_task_samples, fas_task_sample_errors, fas_task_rollbacks,
		fas_util_valid, fas_util_permille, fas_critical_mask, renderer,
		gpu_count, gpu_missing, gpu_level, gpu_average,
		cpu_floors[0], cpu_floors[1], gpu_floor,
		cpu_ceilings[0], cpu_ceilings[1], cpu_loads[0], cpu_loads[1], game_trace,
		!!cpu_requests[0].policy, !!gpu_device, !!pmus, source_valid,
		gpu_mode_owned, gpu_mode_last, gpu_mode_conflict,
		game_rules ? game_rules->count : 0,
		scene_table ? scene_table->count : 0,
		scene_profile ? (int)(scene_profile - scene_table->profiles) : -1,
		inactive_events, activity->epoch, activity->phase,
		activity->registered, activity->devices, activity->reliable, activity->contacts,
		activity->inputs, activity->transitions, activity->load_samples, activity->load_errors,
		activity->load_permille, activity->deadline_valid ? activity->deadline_ms : 0, activity->error,
		scene_table_epoch, aux.loaded, aux.profile, aux.error, aux.stage,
		aux.cpu, aux.knob, aux.owned, aux.conflicts, aux.writes, aux.rollbacks,
		aux.owned_mask, aux.conflict_mask,
		FP_SCENE_AUX_ABI, sizeof(struct fp_scene_aux_table), scene_native_samples,
		scene_active && !!pmus && native_scene_feedback(), scene_core.enabled_clusters, scene_core.retained,
		scene_gesture_live, scene_notify_live, scene_feedback_at,
		gpu_aux.prepared, gpu_aux.target, gpu_aux.owned_mask, gpu_aux.conflict_mask,
		gpu_aux.satisfied_mask, gpu_aux.error, gpu_aux.stage, gpu_aux.knob,
		gpu_aux.rollback_error, gpu_aux.writes, gpu_aux.restores,
		switches, scene_samples, last_error, last_error_stage);
	mutex_unlock(&config_lock);
	return length;
}

static ssize_t policy_read(struct file *file, char __user *data, size_t size, loff_t *offset)
{
	char *status = kmalloc(FP_STATUS_SIZE, GFP_KERNEL);
	struct sab_status activity;
	ssize_t copied;
	int length;

	if (!status)
		return -ENOMEM;
	scene_activity_backend_status(&activity);
	length = format_policy_status(status, FP_STATUS_SIZE, &activity);
	copied = simple_read_from_buffer(data, size, offset, status, length);
	kfree(status);
	return copied;
}

static const struct file_operations policy_ops = {
	.owner = THIS_MODULE,
	.unlocked_ioctl = policy_ioctl,
	.compat_ioctl = policy_ioctl,
	.read = policy_read,
	.llseek = no_llseek,
};

static struct miscdevice policy_device = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "frame_policy",
	.fops = &policy_ops,
	.mode = 0600,
};

static int __init frame_policy_init(void)
{
	int error;

	consumer.handler = probe_entry;
	consumer.ret_handler = probe_return;
	consumer.filter = probe_filter;
	init_irq_work(&scene_irq_work, scene_irq);
	init_irq_work(&activity_control_irq, activity_control_irq_fn);
	policy_wq = alloc_ordered_workqueue("frame_policy", WQ_FREEZABLE | WQ_POWER_EFFICIENT);
	if (!policy_wq)
		return -ENOMEM;
	error = scene_activity_backend_init(scene_activity_changed);
	if (!error)
		error = misc_register(&policy_device);
	if (error) {
		scene_activity_backend_exit();
		destroy_workqueue(policy_wq);
		policy_wq = NULL;
	}
	return error;
}
module_init(frame_policy_init);

static void __exit frame_policy_exit(void)
{
	unsigned long flags;

	misc_deregister(&policy_device);
	scene_activity_backend_exit();
	irq_work_sync(&activity_control_irq);
	mutex_lock(&config_lock);
	configured = false;
	source_valid = false;
	spin_lock_irqsave(&frame_lock, flags);
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	stop_game();
	remove_requests();
	kvfree(game_rules);
	game_rules = NULL;
	kvfree(scene_table);
	scene_table = NULL;
	fp_scene_aux_exit();
	fp_scene_gpu_aux_exit();
	release_gpu_mode();
	if (gpu_governor_file) {
		fput(gpu_governor_file);
		gpu_governor_file = NULL;
	}
	mutex_unlock(&config_lock);
	cancel_work_sync(&control_work);
	cancel_work_sync(&frame_work);
	cancel_work_sync(&scene_work);
	cancel_delayed_work_sync(&expire_work);
	destroy_workqueue(policy_wq);
	policy_wq = NULL;
}
module_exit(frame_policy_exit);
MODULE_LICENSE("GPL");
MODULE_DESCRIPTION("Mutually exclusive Scene and frame frequency requests");

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
#include "frame_policy_kunit.h"
#endif
