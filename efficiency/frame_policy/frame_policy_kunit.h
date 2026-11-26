/* SPDX-License-Identifier: GPL-2.0 */
/* Only compiled into the isolated KUnit kernel, never the device profile. */
#include <kunit/test.h>
#include <linux/anon_inodes.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/uio.h>

static unsigned int fp_test_frequency = 1000000;
static bool fp_test_driver_registered;
static struct platform_device *fp_test_gpu;
static struct cpufreq_policy *fp_test_policy;
static struct freq_qos_request fp_test_cpu_cap;
static struct dev_pm_qos_request fp_test_gpu_cap;

static struct cpufreq_frequency_table fp_test_frequency_table[] = {
	{ .frequency = 500000 }, { .frequency = 1000000 },
	{ .frequency = 1500000 }, { .frequency = 2000000 },
	{ .frequency = 3000000 }, { .frequency = 4000000 },
	{ .frequency = CPUFREQ_TABLE_END }
};

static int fp_test_cpufreq_init(struct cpufreq_policy *policy)
{
	cpumask_copy(policy->cpus, cpu_possible_mask);
	cpufreq_generic_init(policy, fp_test_frequency_table, 1000);
	return 0;
}

static unsigned int fp_test_cpufreq_get(unsigned int cpu)
{
	return READ_ONCE(fp_test_frequency);
}

static int fp_test_cpufreq_target(struct cpufreq_policy *policy, unsigned int index)
{
	WRITE_ONCE(fp_test_frequency, fp_test_frequency_table[index].frequency);
	return 0;
}

static struct cpufreq_driver fp_test_cpufreq_driver = {
	.name = "frame-kunit",
	.init = fp_test_cpufreq_init,
	.verify = cpufreq_generic_frequency_table_verify,
	.target_index = fp_test_cpufreq_target,
	.get = fp_test_cpufreq_get,
};

static void fp_test_reset(void)
{
	unsigned long flags;

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
	gpu_mode_owned = false;
	gpu_mode_conflict = false;
	applied_generation = generation;
	memset(&configuration, 0, sizeof(configuration));
	configuration.flags = FP_OBSERVE;
	configuration.target_fps = 60;
	configuration.fps_count = 1;
	configuration.fps_values[0] = 60;
	last_error = 0;
	mutex_unlock(&config_lock);
	cancel_work_sync(&control_work);
	cancel_work_sync(&frame_work);
	cancel_work_sync(&scene_work);
	cancel_delayed_work_sync(&expire_work);
	if (freq_qos_request_active(&fp_test_cpu_cap))
		freq_qos_remove_request(&fp_test_cpu_cap);
	if (dev_pm_qos_request_active(&fp_test_gpu_cap))
		dev_pm_qos_remove_request(&fp_test_gpu_cap);
	if (fp_test_policy) {
		cpufreq_cpu_put(fp_test_policy);
		fp_test_policy = NULL;
	}
	if (fp_test_driver_registered) {
		cpufreq_unregister_driver(&fp_test_cpufreq_driver);
		fp_test_driver_registered = false;
	}
	if (fp_test_gpu) {
		platform_device_unregister(fp_test_gpu);
		fp_test_gpu = NULL;
	}
}

static int fp_test_init(struct kunit *test)
{
	if (!policy_wq)
		return -ENODEV;
	fp_test_reset();
	return 0;
}

static void fp_test_exit(struct kunit *test)
{
	fp_test_reset();
}

static void frame_test_sched_switch_handoff(struct kunit *test)
{
	u64 before;
	unsigned int attempt;
	int error;

	/* No hardware events or frequency votes: exercise the real rq context. */
	mutex_lock(&config_lock);
	pmus = kcalloc(nr_cpu_ids, sizeof(*pmus), GFP_KERNEL);
	if (!pmus) {
		mutex_unlock(&config_lock);
		KUNIT_FAIL(test, "Cannot allocate test PMU markers");
		return;
	}
	configured = true;
	source_valid = true;
	source_game = false;
	display_off = false;
	before = scene_samples;
	atomic64_set(&next_scene_sample, 0);
	error = register_trace_sched_switch(scene_switch, NULL);
	scene_trace = !error;
	scene_active = !error;
	mutex_unlock(&config_lock);
	KUNIT_ASSERT_EQ(test, error, 0);
	kunit_info(test, "Exercising the actual sched_switch to workqueue handoff");
	for (attempt = 0; attempt < 80 && READ_ONCE(scene_samples) < before + 3; attempt++)
		msleep(25);
	KUNIT_EXPECT_GE(test, READ_ONCE(scene_samples), before + 3);

	/* Entering a game must retire Scene even when the app is not running. */
	frame_policy_scene("frame.policy.kunit.missing", false, true);
	flush_work(&control_work);
	KUNIT_EXPECT_FALSE(test, scene_active);
	KUNIT_EXPECT_FALSE(test, scene_trace);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_FALSE(test, registered);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	before = READ_ONCE(scene_samples);
	msleep(120);
	KUNIT_EXPECT_EQ(test, READ_ONCE(scene_samples), before);
}

static void frame_test_qos_ownership(struct kunit *test)
{
	struct fp_cpu_config *cpu = &configuration.cpus[0];
	int error;

	error = cpufreq_register_driver(&fp_test_cpufreq_driver);
	KUNIT_ASSERT_EQ(test, error, 0);
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	KUNIT_ASSERT_NOT_NULL(test, fp_test_policy);
	fp_test_gpu = platform_device_register_simple("frame-kunit-gpu", -1, NULL, 0);
	if (IS_ERR(fp_test_gpu)) {
		fp_test_gpu = NULL;
		KUNIT_FAIL(test, "Cannot register synthetic GPU device");
		return;
	}
	configuration.cpu_count = 1;
	cpu->cpu = 0;
	cpu->scene = (struct fp_limits){ 1000000, 2000000 };
	cpu->game = (struct fp_limits){ 2500000, 4000000 };
	cpu->idle = (struct fp_limits){ 500000, 1000000 };
	strscpy(configuration.gpu_device, "frame-kunit-gpu", sizeof(configuration.gpu_device));
	configuration.gpu_scene = (struct fp_limits){ 300000, 1000000 };
	configuration.gpu_game = (struct fp_limits){ 500000, 1200000 };
	configuration.gpu_idle = (struct fp_limits){ 300000, 600000 };
	display_off = false;

	/* A separate cap models a system/thermal owner that must survive us. */
	error = freq_qos_add_request(&fp_test_policy->constraints, &fp_test_cpu_cap,
				     FREQ_QOS_MAX, 1800000);
	KUNIT_ASSERT_GE(test, error, 0);
	error = dev_pm_qos_add_request(&fp_test_gpu->dev, &fp_test_gpu_cap,
				       DEV_PM_QOS_MAX_FREQUENCY, 800000);
	KUNIT_ASSERT_GE(test, error, 0);

	configuration.flags = FP_CPU;
	KUNIT_ASSERT_EQ(test, add_requests(false), 0);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 1000000U);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
	remove_requests();
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
	KUNIT_ASSERT_EQ(test, add_requests(true), 0);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 2500000U);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
	remove_requests();

	configuration.flags = FP_GPU;
	KUNIT_ASSERT_EQ(test, add_requests(false), 0);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MIN_FREQUENCY), 300000);
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 800000);
	remove_requests();
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 800000);

	/* Two CPU IDs in one shared policy must roll back partial acquisition. */
	configuration.flags = FP_CPU;
	configuration.cpu_count = 2;
	configuration.cpus[1] = *cpu;
	configuration.cpus[1].cpu = 1;
	KUNIT_EXPECT_EQ(test, add_requests(false), -EINVAL);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[1].policy, NULL);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
}

static void frame_test_native_cycle_feedback(struct kunit *test)
{
	u64 before, counts = 0;
	unsigned int attempt, cpu;
	int error;

	error = cpufreq_register_driver(&fp_test_cpufreq_driver);
	KUNIT_ASSERT_EQ(test, error, 0);
	fp_test_driver_registered = true;
	configuration.cpu_count = 1;
	configuration.cpus[0].cpu = 0;
	configuration.cpus[0].scene = (struct fp_limits){ 1000000, 2000000 };
	configuration.cpus[0].idle = (struct fp_limits){ 500000, 1000000 };
	configuration.flags = FP_CPU | FP_OBSERVE;
	configured = true;
	before = READ_ONCE(scene_samples);
	frame_policy_scene("frame.policy.kunit.cycles", false, false);
	flush_work(&control_work);
	KUNIT_ASSERT_EQ(test, last_error, 0);
	KUNIT_ASSERT_NOT_NULL(test, pmus);
	KUNIT_EXPECT_TRUE(test, scene_active);
	KUNIT_EXPECT_TRUE(test, scene_trace);
	for (attempt = 0; attempt < 80 && READ_ONCE(scene_samples) < before + 3; attempt++) {
		udelay(100);
		msleep(25);
	}
	KUNIT_EXPECT_GE(test, READ_ONCE(scene_samples), before + 3);
	mutex_lock(&config_lock);
	for_each_possible_cpu(cpu)
		counts += pmus[cpu].count;
	mutex_unlock(&config_lock);
	KUNIT_EXPECT_GT(test, counts, 0ULL);
	frame_policy_scene(NULL, true, false);
	flush_work(&control_work);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_FALSE(test, scene_trace);
}

static u32 fp_test_gpu_percent;

static ssize_t fp_test_gpu_read(struct kiocb *iocb, struct iov_iter *output)
{
	char buffer[24];
	size_t length, copied;

	length = scnprintf(buffer, sizeof(buffer), "%u\n", READ_ONCE(fp_test_gpu_percent));
	if (iocb->ki_pos >= length)
		return 0;
	copied = copy_to_iter(buffer + iocb->ki_pos, length - iocb->ki_pos, output);
	iocb->ki_pos += copied;
	return copied;
}

static ssize_t fp_test_gpu_write(struct kiocb *iocb, struct iov_iter *input)
{
	char buffer[24];
	size_t length = iov_iter_count(input);
	u32 value;
	int error;

	if (length >= sizeof(buffer))
		return -E2BIG;
	if (!copy_from_iter_full(buffer, length, input))
		return -EFAULT;
	buffer[length] = 0;
	error = kstrtou32(strim(buffer), 10, &value);
	if (error)
		return error;
	WRITE_ONCE(fp_test_gpu_percent, value);
	iocb->ki_pos += length;
	return length;
}

static const struct file_operations fp_test_gpu_file_ops = {
	.owner = THIS_MODULE,
	.read_iter = fp_test_gpu_read,
	.write_iter = fp_test_gpu_write,
	.llseek = noop_llseek,
};

static void frame_test_gpu_governor_lease(struct kunit *test)
{
	struct file *file = anon_inode_getfile("frame-gpu-mode", &fp_test_gpu_file_ops,
					     NULL, O_RDWR);

	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	gpu_governor_file = file;
	gpu_mode_original = 100;
	gpu_mode_last = 100;
	WRITE_ONCE(fp_test_gpu_percent, 100);
	configuration.gpu_scene_percent = 105;
	configuration.gpu_game_percent = 100;
	display_off = false;
	configuration.flags = FP_GPU | FP_OBSERVE;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 100U);
	KUNIT_EXPECT_FALSE(test, gpu_mode_owned);
	configuration.flags = FP_GPU;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 105U);
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(true), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 100U);
	display_off = true;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 100U);
	display_off = false;
	KUNIT_EXPECT_EQ(test, release_gpu_mode(), 0);
	KUNIT_EXPECT_FALSE(test, gpu_mode_owned);
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	WRITE_ONCE(fp_test_gpu_percent, 110); /* A foreign owner replaces our value. */
	KUNIT_EXPECT_EQ(test, release_gpu_mode(), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 110U);
	KUNIT_EXPECT_TRUE(test, gpu_mode_conflict);
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(true), -EBUSY);
}

struct fp_test_fence {
	struct dma_fence base;
	spinlock_t lock;
	atomic_t *released;
};

static const char *fp_test_fence_name(struct dma_fence *fence)
{
	return "frame_policy_kunit";
}

static void fp_test_fence_release(struct dma_fence *fence)
{
	struct fp_test_fence *item = container_of(fence, struct fp_test_fence, base);

	atomic_inc(item->released);
	kfree(item);
}

static const struct dma_fence_ops fp_test_fence_ops = {
	.get_driver_name = fp_test_fence_name,
	.get_timeline_name = fp_test_fence_name,
	.release = fp_test_fence_release,
};

static struct dma_fence *fp_test_new_fence(atomic_t *released)
{
	struct fp_test_fence *item = kzalloc(sizeof(*item), GFP_KERNEL);

	if (!item)
		return NULL;
	spin_lock_init(&item->lock);
	item->released = released;
	dma_fence_init(&item->base, &fp_test_fence_ops, &item->lock,
		       dma_fence_context_alloc(1), 1);
	return &item->base;
}

static void frame_test_fence_signal_and_cancel(struct kunit *test)
{
	atomic_t released = ATOMIC_INIT(0);
	struct dma_fence *fence = fp_test_new_fence(&released);
	u64 before = gpu_samples;

	KUNIT_ASSERT_NOT_NULL(test, fence);
	game_active = true;
	watch_gpu(dma_fence_get(fence), generation, rate_generation, ktime_get_ns(), false);
	KUNIT_EXPECT_EQ(test, dma_fence_signal(fence), 0);
	KUNIT_EXPECT_EQ(test, gpu_samples, before + 1);
	stop_game();
	dma_fence_put(fence);
	KUNIT_EXPECT_EQ(test, atomic_read(&released), 1);

	atomic_set(&released, 0);
	fence = fp_test_new_fence(&released);
	KUNIT_ASSERT_NOT_NULL(test, fence);
	game_active = true;
	before = gpu_samples;
	watch_gpu(dma_fence_get(fence), generation, rate_generation, ktime_get_ns(), false);
	stop_game();
	KUNIT_EXPECT_EQ(test, dma_fence_signal(fence), 0);
	KUNIT_EXPECT_EQ(test, gpu_samples, before);
	dma_fence_put(fence);
	KUNIT_EXPECT_EQ(test, atomic_read(&released), 1);
}

struct fp_test_signaller {
	struct dma_fence *fence;
	struct completion done;
};

static int fp_test_signal_thread(void *data)
{
	struct fp_test_signaller *signaller = data;

	dma_fence_signal(signaller->fence);
	complete(&signaller->done);
	/* Keep the task alive until the parent joins it with kthread_stop(). */
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void frame_test_fence_cancel_race(struct kunit *test)
{
	unsigned int attempt;

	for (attempt = 0; attempt < 64; attempt++) {
		atomic_t released = ATOMIC_INIT(0);
		struct fp_test_signaller signaller;
		struct task_struct *thread;

		signaller.fence = fp_test_new_fence(&released);
		KUNIT_ASSERT_NOT_NULL(test, signaller.fence);
		init_completion(&signaller.done);
		game_active = true;
		watch_gpu(dma_fence_get(signaller.fence), generation, rate_generation, ktime_get_ns(), false);
		thread = kthread_run(fp_test_signal_thread, &signaller, "fp-fence-test");
		if (IS_ERR(thread)) {
			stop_game();
			dma_fence_put(signaller.fence);
			KUNIT_FAIL(test, "Cannot start fence signaller");
			return;
		}
		stop_game();
		wait_for_completion(&signaller.done);
		kthread_stop(thread);
		dma_fence_put(signaller.fence);
		KUNIT_EXPECT_EQ(test, atomic_read(&released), 1);
	}
}

static void frame_test_ready_and_stale_fences(struct kunit *test)
{
	u64 before = gpu_samples, missing = gpu_unknown;

	game_active = true;
	watch_gpu(NULL, generation, rate_generation, ktime_get_ns(), true);
	KUNIT_EXPECT_EQ(test, gpu_samples, before + 1);
	KUNIT_EXPECT_EQ(test, gpu_unknown, missing);
	watch_gpu(NULL, generation + 1, rate_generation, ktime_get_ns(), true);
	KUNIT_EXPECT_EQ(test, gpu_samples, before + 1);
	watch_gpu(NULL, generation, rate_generation, ktime_get_ns(), false);
	KUNIT_EXPECT_EQ(test, gpu_unknown, missing + 1);
}

static void frame_test_cadence_gate_and_qos_release(struct kunit *test)
{
	struct fp_rate rate = {};
	u32 rates[] = { 60, 165 }, high_only[] = { 165 };
	unsigned int i;
	int error;

	error = cpufreq_register_driver(&fp_test_cpufreq_driver);
	KUNIT_ASSERT_EQ(test, error, 0);
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	KUNIT_ASSERT_NOT_NULL(test, fp_test_policy);
	configuration.flags = FP_CPU;
	configuration.cpu_count = 1;
	configuration.cpus[0].cpu = 0;
	configuration.cpus[0].game = (struct fp_limits){ 500000, 4000000 };
	game_active = true;
	last_frame_ns = ktime_get_ns();
	/* A 60 FPS menu under a 165-only profile must leave system policy alone. */
	for (i = 0; i < 120; i++)
		active_fps = fp_rate_sample(&rate, 16666667, high_only, 1);
	apply_frame(&frame_work);
	KUNIT_EXPECT_EQ(test, active_fps, 0U);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	fp_rate_reset(&rate);
	for (i = 0; i < 60; i++)
		active_fps = fp_rate_sample(&rate, 16666667, rates, 2);
	last_frame_ns = ktime_get_ns();
	apply_frame(&frame_work);
	KUNIT_EXPECT_EQ(test, active_fps, 60U);
	KUNIT_EXPECT_NOT_NULL(test, cpu_requests[0].policy);
	/* Losing qualification releases our votes without reviving Scene PMUs. */
	active_fps = fp_rate_sample(&rate, 1000000000ULL, rates, 2);
	apply_frame(&frame_work);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_TRUE(test, game_active);
}

static void frame_test_fence_grade_and_error_isolation(struct kunit *test)
{
	atomic_t released = ATOMIC_INIT(0);
	struct dma_fence *fence;
	u64 epoch = rate_generation;

	game_active = true;
	active_fps = 60;
	/* A delayed old-grade fence must not feed the new budget's controller. */
	rate_generation++;
	record_gpu_wait(generation, epoch, ktime_get_ns(), 15000000);
	KUNIT_EXPECT_EQ(test, gpu_control.samples, 0U);
	record_gpu_wait(generation, rate_generation, ktime_get_ns(), 15000000);
	KUNIT_EXPECT_EQ(test, gpu_control.samples, 1U);
	fp_control_reset(&gpu_control);
	fence = fp_test_new_fence(&released);
	KUNIT_ASSERT_NOT_NULL(test, fence);
	watch_gpu(dma_fence_get(fence), generation, rate_generation, ktime_get_ns(), false);
	dma_fence_set_error(fence, -EIO);
	KUNIT_EXPECT_EQ(test, dma_fence_signal(fence), 0);
	KUNIT_EXPECT_EQ(test, gpu_control.samples, 0U);
	stop_game();
	dma_fence_put(fence);
	KUNIT_EXPECT_EQ(test, atomic_read(&released), 1);
}

static void frame_test_source_loss(struct kunit *test)
{
	configured = true;
	source_valid = true;
	scene_active = true;
	frame_policy_scene(NULL, true, false);
	flush_work(&control_work);
	KUNIT_EXPECT_FALSE(test, scene_active);
	KUNIT_EXPECT_FALSE(test, game_active);
	KUNIT_EXPECT_FALSE(test, scene_trace);
	KUNIT_EXPECT_PTR_EQ(test, target, NULL);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
}

static int fp_test_source_thread(void *data)
{
	struct completion *done = data;

	frame_policy_scene("frame.policy.kunit.source", false, false);
	complete(done);
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void frame_test_nonblocking_source(struct kunit *test)
{
	struct completion done;
	struct task_struct *thread;
	unsigned long responded;

	init_completion(&done);
	mutex_lock(&config_lock);
	configured = true;
	thread = kthread_run(fp_test_source_thread, &done, "fp-source-test");
	if (IS_ERR(thread)) {
		mutex_unlock(&config_lock);
		KUNIT_FAIL(test, "Cannot create scene event sender");
		return;
	}
	/* A provider must return even while the controller mutex is unavailable. */
	responded = wait_for_completion_timeout(&done, HZ);
	mutex_unlock(&config_lock);
	KUNIT_EXPECT_NE(test, responded, 0UL);
	if (!responded)
		wait_for_completion(&done);
	kthread_stop(thread);
	flush_work(&control_work);
}

static struct kunit_case frame_policy_cases[] = {
	KUNIT_CASE(frame_test_sched_switch_handoff),
	KUNIT_CASE(frame_test_qos_ownership),
	KUNIT_CASE(frame_test_native_cycle_feedback),
	KUNIT_CASE(frame_test_gpu_governor_lease),
	KUNIT_CASE(frame_test_fence_signal_and_cancel),
	KUNIT_CASE(frame_test_fence_cancel_race),
	KUNIT_CASE(frame_test_ready_and_stale_fences),
	KUNIT_CASE(frame_test_cadence_gate_and_qos_release),
	KUNIT_CASE(frame_test_fence_grade_and_error_isolation),
	KUNIT_CASE(frame_test_source_loss),
	KUNIT_CASE(frame_test_nonblocking_source),
	{}
};

static struct kunit_suite frame_policy_suite = {
	.name = "frame_policy",
	.init = fp_test_init,
	.exit = fp_test_exit,
	.test_cases = frame_policy_cases,
};
kunit_test_suite(frame_policy_suite);
