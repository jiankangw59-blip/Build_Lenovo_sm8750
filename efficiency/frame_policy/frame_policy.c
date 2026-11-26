// SPDX-License-Identifier: GPL-2.0
/* Frame events and Scene frequency requests. All arithmetic is integer based. */
#include <linux/capability.h>
#include <linux/cpufreq.h>
#include <linux/dma-fence.h>
#include <linux/file.h>
#include <linux/fs.h>
#include <linux/init.h>
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
};

struct fp_fence_watch {
	struct dma_fence_cb callback;
	struct dma_fence *fence;
	u64 generation, entered_ns;
};

struct fp_stream {
	u64 surface, previous_ns, hits;
	struct fp_control control;
};

struct fp_pmu {
	struct perf_event *cycles;
	u64 count, enabled, running, timestamp;
};

static DEFINE_MUTEX(config_lock);
static DEFINE_SPINLOCK(frame_lock);
static struct workqueue_struct *policy_wq;
static struct fp_config configuration;
static struct fp_cpu_request cpu_requests[FP_MAX_CPUS];
static struct fp_pmu *pmus;
static struct dev_pm_qos_request gpu_min, gpu_max;
static struct device *gpu_device;
static struct task_struct *target;
static struct mm_struct *target_mm;
static struct file *probe_file;
static struct uprobe_consumer consumer;
static struct fp_call calls[FP_CALLS];
static struct fp_fence_watch gpu_watches[FP_CALLS];
static struct fp_control gpu_control;
static struct fp_stream streams[FP_STREAMS];
static char foreground[FP_PACKAGE];
static bool configured, source_valid, source_game, display_off, registered;
static bool game_active, scene_active;
static u64 generation, applied_generation, last_frame_ns, last_work_ns;
static u64 frames_seen, frames_used, frame_errors, switches;
static u64 gpu_samples, gpu_unknown;
static unsigned int selected_stream, floor_level;
static u32 cpu_floors[FP_MAX_CPUS], gpu_floor;
static int last_error;
static atomic64_t next_scene_sample = ATOMIC64_INIT(0);
static u64 scene_samples;

static void apply_control(struct work_struct *work);
static void apply_frame(struct work_struct *work);
static void expire_frame(struct work_struct *work);
static void apply_scene(struct work_struct *work);
static DECLARE_WORK(control_work, apply_control);
static DECLARE_WORK(frame_work, apply_frame);
static DECLARE_WORK(scene_work, apply_scene);
static DECLARE_DELAYED_WORK(expire_work, expire_frame);

static bool observe_only(void)
{
	return configuration.flags & FP_OBSERVE;
}

static void stop_pmus(void)
{
	unsigned int cpu;

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

		if (!request->policy)
			continue;
		if (freq_qos_request_active(&request->min))
			freq_qos_remove_request(&request->min);
		if (freq_qos_request_active(&request->max))
			freq_qos_remove_request(&request->max);
		cpufreq_cpu_put(request->policy);
		request->policy = NULL;
		cpu_floors[i] = 0;
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

static void stop_game(void)
{
	unsigned long flags;
	unsigned int i;

	/* Disable event consumption before waiting for registered handlers. */
	spin_lock_irqsave(&frame_lock, flags);
	game_active = false;
	spin_unlock_irqrestore(&frame_lock, flags);
	cancel_delayed_work(&expire_work);
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
	if (!game_active || current->mm != target_mm)
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

static void record_gpu_sample(u64 epoch, u64 entered_ns, struct dma_fence *fence)
{
	unsigned long flags;
	u64 finished = ktime_to_ns(dma_fence_timestamp(fence));
	u64 pending = finished > entered_ns ? finished - entered_ns : 0;

	spin_lock_irqsave(&frame_lock, flags);
	if (game_active && epoch == generation) {
		fp_control_gpu(&gpu_control, min_t(u64, pending, FP_INACTIVITY_NS),
			       configuration.target_fps);
		gpu_samples++;
	}
	spin_unlock_irqrestore(&frame_lock, flags);
}

static void gpu_ready(struct dma_fence *fence, struct dma_fence_cb *callback)
{
	struct fp_fence_watch *watch = container_of(callback, struct fp_fence_watch, callback);
	unsigned long flags;

	record_gpu_sample(watch->generation, watch->entered_ns, fence);
	spin_lock_irqsave(&frame_lock, flags);
	watch->fence = NULL;
	spin_unlock_irqrestore(&frame_lock, flags);
	dma_fence_put(fence);
}

/* The fence reference is consumed on every path. */
static void watch_gpu(struct dma_fence *fence, u64 epoch, u64 entered_ns)
{
	struct fp_fence_watch *watch = NULL;
	unsigned long flags;
	unsigned int i;
	int error;

	if (!fence) {
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
		record_gpu_sample(epoch, entered_ns, fence);
	spin_lock_irqsave(&frame_lock, flags);
	watch->fence = NULL;
	spin_unlock_irqrestore(&frame_lock, flags);
	dma_fence_put(fence);
}

static int probe_return(struct uprobe_consumer *self, unsigned long function,
			struct pt_regs *regs)
{
	u64 now = ktime_get_ns(), surface = 0, epoch = 0;
	u64 entered_ns = 0;
	struct dma_fence *fence = NULL;
	unsigned long flags;
	unsigned int i, selected = FP_STREAMS;
	bool schedule = false;
	pid_t tid = task_pid_nr(current);

	spin_lock_irqsave(&frame_lock, flags);
	for (i = 0; i < FP_CALLS; i++) {
		if (calls[i].tid != tid)
			continue;
		if (calls[i].start_time == current->start_boottime) {
			surface = calls[i].surface;
			epoch = calls[i].generation;
			entered_ns = calls[i].entered_ns;
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
	if (selected == FP_STREAMS) {
		frame_errors++;
		goto out;
	}
	streams[selected].surface = surface;
	streams[selected].hits++;
	if (streams[selected].hits > streams[selected_stream].hits)
		selected_stream = selected;
	if (streams[selected].previous_ns)
		fp_control_frame(&streams[selected].control, now - streams[selected].previous_ns,
				 configuration.target_fps);
	streams[selected].previous_ns = now;
	if (selected != selected_stream)
		goto out;
	last_frame_ns = now;
	floor_level = streams[selected].control.level;
	frames_used++;
	if (now - last_work_ns >= FP_FRAME_WORK_NS) {
		last_work_ns = now;
		schedule = true;
	}
out:
	spin_unlock_irqrestore(&frame_lock, flags);
	if (surface && epoch && !(long)regs_return_value(regs))
		watch_gpu(fence, epoch, entered_ns);
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
	selected_stream = 0;
	last_frame_ns = 0;
	last_work_ns = 0;
	floor_level = 0;
	game_active = true;
	spin_unlock_irqrestore(&frame_lock, flags);
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
	mutex_lock(&config_lock);
	if (applied_generation == generation && target &&
	    !(READ_ONCE(target->flags) & PF_EXITING))
		goto out;
	/*
	 * This is the sole transition point. All old requests are removed before
	 * either branch starts. Queued frame work checks the new generation.
	 */
	stop_game();
	remove_requests();
	applied_generation = generation;
	last_error = 0;
	switches++;
	if (!configured || !source_valid)
		goto out;
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
out:
	mutex_unlock(&config_lock);
}

static void scene_switch(void *data, bool preempt, struct task_struct *previous,
			 struct task_struct *next, unsigned int prev_state)
{
	u64 now, due;

	if (!READ_ONCE(scene_active) || READ_ONCE(display_off) || !READ_ONCE(pmus))
		return;
	now = ktime_get_mono_fast_ns();
	due = atomic64_read(&next_scene_sample);
	if (now >= due && atomic64_cmpxchg(&next_scene_sample, due, now + 80000000ULL) == due)
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

static void apply_frame(struct work_struct *work)
{
	unsigned long flags;
	unsigned int i, level, gpu_level;
	u64 now = ktime_get_ns(), last;
	int error = 0;

	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	last = last_frame_ns;
	level = floor_level;
	gpu_level = gpu_control.level;
	if (!game_active || applied_generation != generation || !last ||
	    now - last >= FP_INACTIVITY_NS) {
		spin_unlock_irqrestore(&frame_lock, flags);
		goto out;
	}
	spin_unlock_irqrestore(&frame_lock, flags);
	if (observe_only())
		goto arm;
	if ((!cpu_requests[0].policy && (configuration.flags & FP_CPU)) ||
	    (!gpu_device && (configuration.flags & FP_GPU))) {
		error = add_requests(true);
		if (error)
			goto failed;
	}
	for (i = 0; i < configuration.cpu_count; i++) {
		const struct fp_limits *limits = &configuration.cpus[i].game;
		u32 floor = fp_control_floor(level, limits->min_khz, limits->max_khz);

		if (!cpu_requests[i].policy || cpu_floors[i] == floor)
			continue;
		error = freq_qos_update_request(&cpu_requests[i].min, floor);
		if (error < 0)
			goto failed;
		cpu_floors[i] = floor;
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
	remove_requests();
out:
	mutex_unlock(&config_lock);
}

static void expire_frame(struct work_struct *work)
{
	unsigned long flags;
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
out:
	mutex_unlock(&config_lock);
}

void frame_policy_scene(const char *package, bool off, bool game)
{
	unsigned long flags;

	mutex_lock(&config_lock);
	source_valid = package && *package;
	strscpy(foreground, source_valid ? package : "", sizeof(foreground));
	source_game = source_valid && game;
	display_off = off;
	spin_lock_irqsave(&frame_lock, flags);
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	if (policy_wq)
		queue_work(policy_wq, &control_work);
}

void frame_policy_task_event(struct task_struct *task)
{
	unsigned long flags;
	unsigned int i;

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
	u32 unused = 0;

	for (i = 0; i < ARRAY_SIZE(config->reserved); i++)
		unused |= config->reserved[i];
	if (config->abi != FP_ABI || !config->flags || (config->flags & ~FP_FLAGS) || unused ||
	    !config->cpu_count || config->cpu_count > FP_MAX_CPUS ||
	    config->target_fps < 15 || config->target_fps > 240 ||
	    !config->probe_offset || (config->probe_offset & 3) ||
	    config->probe_offset > S64_MAX ||
	    !memchr(config->gpu_device, 0, sizeof(config->gpu_device)))
		return -EINVAL;
	if ((config->flags & FP_GPU) && (!config->gpu_device[0] ||
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
	unsigned long flags;
	int error;

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
	}
	mutex_lock(&config_lock);
	stop_game();
	remove_requests();
	if (cmd == FP_CONFIG)
		configuration = next;
	configured = cmd == FP_CONFIG;
	spin_lock_irqsave(&frame_lock, flags);
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	queue_work(policy_wq, &control_work);
	flush_work(&control_work);
	return 0;
}

static ssize_t policy_read(struct file *file, char __user *data, size_t size, loff_t *offset)
{
	char status[1024];
	unsigned long flags;
	u64 frames, used, errors, epoch, gpu_count, gpu_missing, gpu_average;
	unsigned int level, gpu_level;
	int length;

	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	frames = frames_seen;
	used = frames_used;
	errors = frame_errors;
	epoch = generation;
	level = floor_level;
	gpu_level = gpu_control.level;
	gpu_average = gpu_control.average_ns;
	gpu_count = gpu_samples;
	gpu_missing = gpu_unknown;
	spin_unlock_irqrestore(&frame_lock, flags);
	length = scnprintf(status, sizeof(status),
		"abi=2\nconfigured=%u\nobserve=%u\nowner=%s\nforeground=%s\n"
		"game_pid=%d\nprobe=%u\ngeneration=%llu\napplied_generation=%llu\n"
		"frames=%llu\nselected_frames=%llu\nframe_errors=%llu\nlevel=%u\n"
		"gpu_samples=%llu\ngpu_unknown=%llu\ngpu_level=%u\ngpu_pending_ns=%llu\n"
		"cpu0_floor_khz=%u\ncpu1_floor_khz=%u\ngpu_floor_khz=%u\n"
		"cpu_requests=%u\ngpu_requests=%u\nswitches=%llu\nscene_samples=%llu\nlast_error=%d\n",
		configured, observe_only(), game_active ? "frame" : scene_active ? "scene" : "idle",
		foreground, target ? task_pid_nr(target) : 0, registered, epoch,
		applied_generation, frames, used, errors, level,
		gpu_count, gpu_missing, gpu_level, gpu_average,
		cpu_floors[0], cpu_floors[1], gpu_floor,
		!!cpu_requests[0].policy, !!gpu_device, switches, scene_samples, last_error);
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
	policy_wq = alloc_ordered_workqueue("frame_policy", WQ_FREEZABLE | WQ_POWER_EFFICIENT);
	if (!policy_wq)
		return -ENOMEM;
	error = misc_register(&policy_device);
	if (!error) {
		error = register_trace_sched_switch(scene_switch, NULL);
		if (error)
			misc_deregister(&policy_device);
	}
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
	unregister_trace_sched_switch(scene_switch, NULL);
	tracepoint_synchronize_unregister();
	mutex_lock(&config_lock);
	configured = false;
	source_valid = false;
	spin_lock_irqsave(&frame_lock, flags);
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	stop_game();
	remove_requests();
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
