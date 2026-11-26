// SPDX-License-Identifier: GPL-2.0
/* Frame events and Scene frequency requests. All arithmetic is integer based. */
#include <linux/capability.h>
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
#include "frame_rate.h"
#include "frame_policy_uapi.h"

#define FP_PACKAGE 128
#define FP_STREAMS 8
#define FP_CALLS 32
#define FP_FRAME_WORK_NS 20000000ULL
#define FP_INACTIVITY_NS 1000000000ULL

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

static DEFINE_MUTEX(config_lock);
static DEFINE_SPINLOCK(frame_lock);
static struct workqueue_struct *policy_wq;
static struct irq_work scene_irq_work;
static struct fp_config configuration;
static struct fp_cpu_request cpu_requests[FP_MAX_CPUS];
static struct fp_pmu *pmus;
static struct fp_game_cpu *game_cpus;
static bool game_trace;
static u64 game_sample_ns, game_switches, critical_until[FP_MAX_CPUS];
static u32 cpu_ceilings[FP_MAX_CPUS], cpu_loads[FP_MAX_CPUS];
static u32 game_targets[FP_MAX_CPUS];
static bool game_cap_valid[FP_MAX_CPUS];
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
} pending_scene;
static bool configured, source_valid, source_game, display_off, registered;
static bool game_active, scene_active, scene_trace;
static u64 generation, applied_generation, last_frame_ns, last_work_ns;
static u64 frames_seen, frames_used, frame_errors, switches;
static u64 gpu_samples, gpu_unknown;
static u64 rate_generation;
static u32 active_fps;
static unsigned int selected_stream, floor_level;
static u32 cpu_floors[FP_MAX_CPUS], gpu_floor;
static int last_error;
static atomic64_t next_scene_sample = ATOMIC64_INIT(0);
static u64 scene_samples;

static void apply_control(struct work_struct *work);
static void apply_frame(struct work_struct *work);
static void expire_frame(struct work_struct *work);
static void apply_scene(struct work_struct *work);
static void scene_switch(void *data, bool preempt, struct task_struct *previous,
			 struct task_struct *next, unsigned int prev_state);
static void game_switch(void *data, bool preempt, struct task_struct *previous,
			struct task_struct *next, unsigned int prev_state);
static DECLARE_WORK(control_work, apply_control);
static DECLARE_WORK(frame_work, apply_frame);
static DECLARE_WORK(scene_work, apply_scene);
static DECLARE_DELAYED_WORK(expire_work, expire_frame);

static bool observe_only(void)
{
	return configuration.flags & FP_OBSERVE;
}

static int read_gpu_mode(u32 *value)
{
	char buffer[24];
	loff_t offset = 0;
	ssize_t length;

	if (!gpu_governor_file)
		return -EBADF;
	length = kernel_read(gpu_governor_file, buffer, sizeof(buffer) - 1, &offset);
	if (length <= 0)
		return length < 0 ? (int)length : -EIO;
	buffer[length] = 0;
	return kstrtou32(strim(buffer), 10, value);
}

static int write_gpu_mode(u32 value)
{
	char buffer[24];
	loff_t offset = 0;
	u32 readback;
	int length, error;
	ssize_t written;

	length = scnprintf(buffer, sizeof(buffer), "%u\n", value);
	written = kernel_write(gpu_governor_file, buffer, length, &offset);
	if (written != length)
		return written < 0 ? (int)written : -EIO;
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

	if (observe_only() || !(configuration.flags & FP_GPU) || !desired || display_off)
		return 0;
	if (gpu_mode_conflict)
		return -EBUSY;
	error = read_gpu_mode(&current_percent);
	if (error)
		return error;
	if (gpu_mode_owned && current_percent != gpu_mode_last) {
		gpu_mode_conflict = true;
		gpu_mode_owned = false;
		return -EBUSY;
	}
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
	if (!pmus)
		return;
	for_each_possible_cpu(cpu) {
		if (pmus[cpu].cycles)
			perf_event_release_kernel(pmus[cpu].cycles);
	}
	kfree(pmus);
	pmus = NULL;
}

static int start_pmus(void)
{
	struct perf_event_attr attr = {
		.type = PERF_TYPE_HARDWARE,
		.size = sizeof(attr),
		.config = PERF_COUNT_HW_CPU_CYCLES,
	};
	unsigned int cpu, i;
	int error;

	pmus = kcalloc(nr_cpu_ids, sizeof(*pmus), GFP_KERNEL);
	if (!pmus)
		return -ENOMEM;
	for (i = 0; i < configuration.cpu_count; i++) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(configuration.cpus[i].cpu);

		if (!policy) {
			error = -ENODEV;
			goto fail;
		}
		for_each_cpu(cpu, policy->related_cpus) {
			struct fp_pmu *sample = &pmus[cpu];

			if (sample->cycles || !cpu_online(cpu))
				continue;
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

		game_cap_valid[i] = false;
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
}

static int add_requests(bool game)
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

static int start_game_accounting(void)
{
	unsigned int i, j, cpu;
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
	for (j = 0; j < FP_MAX_CPUS; j++)
		game_cap_valid[j] = false;
	game_sample_ns = ktime_get_ns();
	error = register_trace_sched_switch(game_switch, NULL);
	game_trace = !error;
	return error;
}

static void stop_game(void)
{
	unsigned long flags;
	unsigned int i;

	/* Disable event consumption before waiting for registered handlers. */
	spin_lock_irqsave(&frame_lock, flags);
	game_active = false;
	active_fps = 0;
	rate_generation++;
	fp_control_reset(&gpu_control);
	floor_level = 0;
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
	memset(cpu_loads, 0, sizeof(cpu_loads));
	memset(critical_until, 0, sizeof(critical_until));
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

static int probe_return(struct uprobe_consumer *self, unsigned long function,
			struct pt_regs *regs)
{
	u64 now = ktime_get_ns(), surface = 0, epoch = 0;
	u64 entered_ns = 0, rate_epoch = 0;
	struct dma_fence *fence = NULL;
	unsigned long flags;
	unsigned int i, selected = FP_STREAMS, old_selected, fps;
	bool schedule = false, use_fence = false, gpu_ready_now = false;
	pid_t tid = task_pid_nr(current);

	spin_lock_irqsave(&frame_lock, flags);
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
				selected = i;
				memset(&streams[i], 0, sizeof(streams[i]));
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

		fps = fp_rate_sample(&stream->rate, dt, configuration.fps_values,
				     configuration.fps_count);
		if (fps != previous_fps)
			fp_control_reset(&stream->control);
		if (fps)
			fp_control_frame(&stream->control, dt, fps);
	}
	streams[selected].previous_ns = now;
	if (selected != selected_stream)
		goto out;
	fps = streams[selected].rate.target;
	if (old_selected != selected_stream || active_fps != fps) {
		active_fps = fps;
		rate_generation++;
		fp_control_reset(&gpu_control);
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
	u8 bytes[sizeof(configuration.probe_bytes)];
	loff_t offset = configuration.probe_offset;
	unsigned long flags;
	ssize_t count;
	int error;

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
	memset(calls, 0, sizeof(calls));
	memset(streams, 0, sizeof(streams));
	fp_control_reset(&gpu_control);
	active_fps = 0;
	rate_generation++;
	selected_stream = 0;
	last_frame_ns = 0;
	last_work_ns = 0;
	floor_level = 0;
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

static void apply_control(struct work_struct *work)
{
	typeof(pending_scene) next;
	unsigned long flags;
	u64 epoch;

	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	next = pending_scene;
	epoch = generation;
	spin_unlock_irqrestore(&frame_lock, flags);
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
	last_error = 0;
	switches++;
	if (!configured || !source_valid) {
		release_gpu_mode();
		goto out;
	}
	if (source_game && !display_off) {
		last_error = start_game();
	} else {
		last_error = add_requests(false);
		if (!last_error && !display_off && (configuration.flags & FP_CPU))
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
	}
out:
	mutex_unlock(&config_lock);
}

static void scene_switch(void *data, bool preempt, struct task_struct *previous,
			 struct task_struct *next, unsigned int prev_state)
{
	u64 now, due;

	if (!READ_ONCE(scene_active) || READ_ONCE(display_off) || !READ_ONCE(pmus) ||
	    READ_ONCE(applied_generation) != READ_ONCE(generation))
		return;
	now = ktime_get_mono_fast_ns();
	due = atomic64_read(&next_scene_sample);
	if (now >= due && atomic64_cmpxchg(&next_scene_sample, due, now + 80000000ULL) == due)
		irq_work_queue(&scene_irq_work);
}

static void scene_irq(struct irq_work *work)
{
	/* sched_switch holds rq->lock: never take workqueue/pool locks there. */
	if (READ_ONCE(scene_active) && !READ_ONCE(display_off))
		queue_work(policy_wq, &scene_work);
}

static void apply_scene(struct work_struct *work)
{
	u32 effective[FP_MAX_CPUS] = {};
	unsigned int i, cpu;
	u64 now = ktime_get_ns();

	mutex_lock(&config_lock);
	if (!scene_active || display_off || !pmus || applied_generation != generation)
		goto out;
	for (i = 0; i < configuration.cpu_count; i++) {
		struct cpufreq_policy *policy = cpufreq_cpu_get(configuration.cpus[i].cpu);
		u32 ceiling;
		int error;
		bool have_data = false;

		if (!policy)
			continue;
		for_each_cpu(cpu, policy->related_cpus) {
			struct fp_pmu *sample = &pmus[cpu];
			u64 count, enabled, running, cycles, dt, denabled, drunning;

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
				have_data = true;
			}
			sample->count = count;
			sample->enabled = enabled;
			sample->running = running;
			sample->timestamp = now;
		}
		cpufreq_cpu_put(policy);
		if (!have_data || observe_only() || !cpu_requests[i].policy)
			continue;
		ceiling = fp_scene_ceiling(effective[i], &configuration.cpus[i]);
		error = freq_qos_update_request(&cpu_requests[i].max, ceiling);
		if (error < 0) {
			last_error = error;
			remove_requests();
			break;
		}
	}
	scene_samples++;
out:
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
static u64 sample_game_load(u64 now)
{
	u64 dt = game_sample_ns && now > game_sample_ns ? now - game_sample_ns : 0;
	unsigned int i, cpu;

	memset(cpu_loads, 0, sizeof(cpu_loads));
	if (!game_cpus)
		return 0;
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
	return dt;
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

static void apply_frame(struct work_struct *work)
{
	unsigned long flags;
	unsigned int i, gpu_level, fps, samples;
	u64 now, last, dt, average;
	int renderer;
	int error = 0;

	mutex_lock(&config_lock);
	now = ktime_get_ns();
	spin_lock_irqsave(&frame_lock, flags);
	last = last_frame_ns;
	gpu_level = gpu_control.level;
	fps = active_fps;
	average = streams[selected_stream].control.average_ns;
	samples = streams[selected_stream].control.samples;
	renderer = render_cpu;
	if (!game_active || applied_generation != generation || !last ||
	    now - last >= FP_INACTIVITY_NS) {
		spin_unlock_irqrestore(&frame_lock, flags);
		goto out;
	}
	dt = sample_game_load(now);
	spin_unlock_irqrestore(&frame_lock, flags);
	if (observe_only())
		goto arm;
	if (!fps || samples < 8) {
		/* Menus/unknown cadence must not drive the maximum configured FPS. */
		remove_requests();
		goto arm;
	}
	if ((!cpu_requests[0].policy && (configuration.flags & FP_CPU)) ||
	    (!gpu_device && (configuration.flags & FP_GPU))) {
		error = add_requests(true);
		if (error)
			goto failed;
	}
	for (i = 0; i < configuration.cpu_count; i++) {
		const struct fp_limits *limits = &configuration.cpus[i].game;
		struct cpufreq_policy *policy = cpu_requests[i].policy;
		u32 floor = limits->min_khz, ceiling, previous, target_khz;

		if (!policy || !dt || samples < 8)
			continue;
		previous = game_cap_valid[i] ? game_targets[i] :
			clamp(READ_ONCE(policy->cur), limits->min_khz, limits->max_khz);
		target_khz = fp_game_ceiling(previous, limits->min_khz, limits->max_khz,
					     cpu_loads[i], average, fps, dt);
		ceiling = snap_game_ceiling(policy, target_khz, limits);
		if (!ceiling) {
			error = -ERANGE;
			goto failed;
		}
		if ((renderer >= 0 && cpumask_test_cpu(renderer, policy->related_cpus)) ||
		    cpu_loads[i] >= 800)
			critical_until[i] = now + 250000000ULL;
		if (now < critical_until[i])
			floor = ceiling;
		error = update_cpu_bounds(i, floor, ceiling);
		if (error < 0)
			goto failed;
		game_cap_valid[i] = true;
		game_targets[i] = target_khz;
	}
	if (gpu_device) {
		const struct fp_limits *limits = &configuration.gpu_game;
		u32 floor = fp_control_floor(gpu_level, limits->min_khz, limits->max_khz);

		if (floor != gpu_floor) {
			error = dev_pm_qos_update_request(&gpu_min, floor);
			if (error < 0)
				goto failed;
			gpu_floor = floor;
		}
	}
arm:
	/* A single inactivity deadline, refreshed only by actual frame work. */
	mod_delayed_work(policy_wq, &expire_work, HZ);
	goto out;
failed:
	last_error = error;
	stop_game();
	remove_requests();
	release_gpu_mode();
out:
	mutex_unlock(&config_lock);
}

static void expire_frame(struct work_struct *work)
{
	unsigned long flags;
	unsigned int i;
	u64 last, now = ktime_get_ns();

	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	last = last_frame_ns;
	spin_unlock_irqrestore(&frame_lock, flags);
	if (!game_active || applied_generation != generation)
		goto out;
	if (last && now - last < FP_INACTIVITY_NS) {
		mod_delayed_work(policy_wq, &expire_work,
				 nsecs_to_jiffies(FP_INACTIVITY_NS - (now - last)) + 1);
		goto out;
	}
	remove_requests();
	spin_lock_irqsave(&frame_lock, flags);
	fp_control_reset(&gpu_control);
	active_fps = 0;
	rate_generation++;
	floor_level = 0;
	for (i = 0; i < FP_STREAMS; i++) {
		fp_control_reset(&streams[i].control);
		fp_rate_reset(&streams[i].rate);
		streams[i].hits = 0;
		streams[i].previous_ns = 0;
	}
	spin_unlock_irqrestore(&frame_lock, flags);
out:
	mutex_unlock(&config_lock);
}

void frame_policy_scene(const char *package, bool off, bool game)
{
	unsigned long flags;
	bool valid = package && *package;

	/* The affinity/event provider must never wait for PMU or probe teardown. */
	spin_lock_irqsave(&frame_lock, flags);
	if (pending_scene.valid == valid && pending_scene.game == (valid && game) &&
	    pending_scene.off == off &&
	    !strncmp(pending_scene.package, valid ? package : "", sizeof(pending_scene.package))) {
		spin_unlock_irqrestore(&frame_lock, flags);
		return;
	}
	pending_scene.valid = valid;
	strscpy(pending_scene.package, pending_scene.valid ? package : "",
		 sizeof(pending_scene.package));
	pending_scene.game = pending_scene.valid && game;
	pending_scene.off = off;
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	if (policy_wq && READ_ONCE(configured))
		queue_work(policy_wq, &control_work);
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

static long policy_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct fp_config next;
	struct file *governor = NULL;
	unsigned long flags;
	int error = 0;

	if (!capable(CAP_SYS_ADMIN))
		return -EPERM;
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
	spin_lock_irqsave(&frame_lock, flags);
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	queue_work(policy_wq, &control_work);
	flush_work(&control_work);
	return error;
}

static ssize_t policy_read(struct file *file, char __user *data, size_t size, loff_t *offset)
{
	char status[1536];
	unsigned long flags;
	u64 frames, used, errors, epoch, gpu_count, gpu_missing, gpu_average;
	unsigned int level, gpu_level, fps;
	int length;

	mutex_lock(&config_lock);
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
	spin_unlock_irqrestore(&frame_lock, flags);
	length = scnprintf(status, sizeof(status),
		"abi=%u\nconfigured=%u\nobserve=%u\nowner=%s\nforeground=%s\n"
		"game_pid=%d\nprobe=%u\ngeneration=%llu\napplied_generation=%llu\n"
		"frames=%llu\nselected_frames=%llu\nframe_errors=%llu\nlevel=%u\n"
		"qualified_fps=%u\n"
		"gpu_samples=%llu\ngpu_unknown=%llu\ngpu_level=%u\ngpu_pending_ns=%llu\n"
		"cpu0_floor_khz=%u\ncpu1_floor_khz=%u\ngpu_floor_khz=%u\n"
		"cpu0_ceiling_khz=%u\ncpu1_ceiling_khz=%u\n"
		"cpu0_busy_permille=%u\ncpu1_busy_permille=%u\ngame_sched_hook=%u\n"
		"cpu_requests=%u\ngpu_requests=%u\nscene_pmu=%u\nsource_valid=%u\n"
		"gpu_mode_owned=%u\ngpu_mode_percent=%u\ngpu_mode_conflict=%u\n"
		"switches=%llu\nscene_samples=%llu\nlast_error=%d\n",
		FP_ABI, configured, observe_only(), game_active ? "frame" : scene_active ? "scene" : "idle",
		foreground, target ? task_pid_nr(target) : 0, registered, epoch,
		applied_generation, frames, used, errors, level, fps,
		gpu_count, gpu_missing, gpu_level, gpu_average,
		cpu_floors[0], cpu_floors[1], gpu_floor,
		cpu_ceilings[0], cpu_ceilings[1], cpu_loads[0], cpu_loads[1], game_trace,
		!!cpu_requests[0].policy, !!gpu_device, !!pmus, source_valid,
		gpu_mode_owned, gpu_mode_last, gpu_mode_conflict,
		switches, scene_samples, last_error);
	mutex_unlock(&config_lock);
	return simple_read_from_buffer(data, size, offset, status, length);
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
	policy_wq = alloc_ordered_workqueue("frame_policy", WQ_FREEZABLE | WQ_POWER_EFFICIENT);
	if (!policy_wq)
		return -ENOMEM;
	error = misc_register(&policy_device);
	if (error) {
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
	mutex_lock(&config_lock);
	configured = false;
	source_valid = false;
	spin_lock_irqsave(&frame_lock, flags);
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	stop_game();
	remove_requests();
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
