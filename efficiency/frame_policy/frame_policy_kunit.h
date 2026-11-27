/* SPDX-License-Identifier: GPL-2.0 */
/* Only compiled into the isolated KUnit kernel, never the device profile. */
#include <kunit/test.h>
#include <linux/anon_inodes.h>
#include <linux/completion.h>
#include <linux/delay.h>
#include <linux/kthread.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/uio.h>

static unsigned int fp_test_frequency = 1000000;
static bool fp_test_driver_registered;
static struct platform_device *fp_test_gpu;
static struct cpufreq_policy *fp_test_policy;
static struct input_dev *fp_test_activity_input;
static struct freq_qos_request fp_test_cpu_cap;
static struct dev_pm_qos_request fp_test_gpu_cap;
static bool fp_test_gpu_credential_check, fp_test_gpu_force_error;
static int fp_test_gpu_read_error;
static struct file *fp_test_gpu_aux_files[3];

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
	u32 i;

	scene_activity_backend_test_resume_worker();
	mutex_lock(&config_lock);
	scene_activity_backend_test_connect_hook(NULL);
	configured = false;
	source_valid = false;
	spin_lock_irqsave(&frame_lock, flags);
	generation++;
	scene_activity_epoch++;
	memset(&pending_scene, 0, sizeof(pending_scene));
	spin_unlock_irqrestore(&frame_lock, flags);
	reconcile_scene_activity();
	irq_work_sync(&activity_control_irq);
	stop_game();
	remove_requests();
	fp_scene_aux_test_reset();
	fp_scene_gpu_aux_exit();
	fp_scene_gpu_aux_commit(NULL);
	gpu_aux_error = 0;
	for (i = 0; i < ARRAY_SIZE(fp_test_gpu_aux_files); i++) {
		if (fp_test_gpu_aux_files[i])
			fput(fp_test_gpu_aux_files[i]);
		fp_test_gpu_aux_files[i] = NULL;
	}
	memset(&scene_core, 0, sizeof(scene_core));
	test_scene_cpu_sample = NULL;
	kvfree(game_rules);
	game_rules = NULL;
	kvfree(scene_table);
	scene_table = NULL;
	release_gpu_mode();
	if (gpu_governor_file) {
		fput(gpu_governor_file);
		gpu_governor_file = NULL;
	}
	gpu_mode_owned = false;
	gpu_mode_conflict = false;
	applied_generation = generation;
	memset(&configuration, 0, sizeof(configuration));
	fas_tuning = (struct fp_fas_tuning){
		.abi = FP_FAS_ABI, .mode = 1, .margin_base_mfps = 1000,
	};
	fas_util_limit_mfps = FP_FAS_UTIL_LIMIT_DEFAULT;
	configuration.flags = FP_OBSERVE;
	configuration.target_fps = 60;
	configuration.fps_count = 1;
	configuration.fps_values[0] = 60;
	last_error = 0;
	last_error_stage = "none";
	fp_test_gpu_credential_check = false;
	fp_test_gpu_force_error = false;
	fp_test_gpu_read_error = 0;
	mutex_unlock(&config_lock);
	cancel_work_sync(&control_work);
	cancel_work_sync(&frame_work);
	cancel_work_sync(&scene_work);
	cancel_delayed_work_sync(&expire_work);
	cancel_delayed_work_sync(&scene_feedback_deadline);
	if (fp_test_activity_input) {
		input_unregister_device(fp_test_activity_input);
		fp_test_activity_input = NULL;
	}
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

static void fp_test_flush_activity_controls(void)
{
	scene_activity_backend_test_flush();
	irq_work_sync(&activity_control_irq);
	flush_work(&control_work);
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

	if (fp_test_gpu_read_error)
		return fp_test_gpu_read_error;
	if (fp_test_gpu_credential_check && current_cred() != iocb->ki_filp->f_cred)
		return -EACCES;
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

	if (fp_test_gpu_credential_check && current_cred() != iocb->ki_filp->f_cred)
		return -EACCES;
	if (fp_test_gpu_force_error)
		return -EIO;
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
	WRITE_ONCE(fp_test_gpu_percent, 110); /* A new owner writes while we hold no lease. */
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 105U);
	display_off = true;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 110U);
	KUNIT_EXPECT_FALSE(test, gpu_mode_owned);
	display_off = false;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	WRITE_ONCE(fp_test_gpu_percent, 110); /* A foreign owner replaces our value. */
	KUNIT_EXPECT_EQ(test, release_gpu_mode(), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 110U);
	KUNIT_EXPECT_TRUE(test, gpu_mode_conflict);
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(true), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 110U);
	KUNIT_EXPECT_FALSE(test, gpu_mode_owned);
	KUNIT_EXPECT_TRUE(test, gpu_mode_conflict);
	display_off = true;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 110U);
	KUNIT_EXPECT_FALSE(test, gpu_mode_owned);
	display_off = false;
	/* A later game whose intent already matches the foreign value can
	 * reacquire without writing, even if a write would currently fail.
	 */
	WRITE_ONCE(fp_test_gpu_percent, 100);
	fp_test_gpu_force_error = true;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(true), 0);
	KUNIT_EXPECT_TRUE(test, gpu_mode_owned);
	KUNIT_EXPECT_FALSE(test, gpu_mode_conflict);
	KUNIT_EXPECT_EQ(test, gpu_mode_original, 100U);
	fp_test_gpu_force_error = false;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(false), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 105U);
	KUNIT_EXPECT_EQ(test, release_gpu_mode(), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 100U);
	/* Only ownership conflicts yield. Real reads, including EBUSY, and
	 * invalid hardware values keep their ordinary error semantics.
	 */
	gpu_mode_conflict = true;
	fp_test_gpu_read_error = -EBUSY;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(true), -EBUSY);
	fp_test_gpu_read_error = -EIO;
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(true), -EIO);
	fp_test_gpu_read_error = 0;
	WRITE_ONCE(fp_test_gpu_percent, 201);
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(true), -ERANGE);
	WRITE_ONCE(fp_test_gpu_percent, 0);
	KUNIT_EXPECT_EQ(test, apply_gpu_mode(true), -ERANGE);
}

static void frame_test_gpu_file_credentials(struct kunit *test)
{
	const struct cred *caller = current_cred(), *saved;
	struct cred *provider = prepare_creds();
	struct file *file;
	u32 value = 0;

	KUNIT_ASSERT_NOT_NULL(test, provider);
	provider->fsuid = make_kuid(&init_user_ns, 1000);
	saved = override_creds(provider);
	file = anon_inode_getfile("frame-credential-test", &fp_test_gpu_file_ops, NULL, O_RDWR);
	revert_creds(saved);
	put_cred(provider);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	gpu_governor_file = file;
	fp_test_gpu_credential_check = true;
	WRITE_ONCE(fp_test_gpu_percent, 100);
	KUNIT_EXPECT_EQ(test, read_gpu_mode(&value), 0);
	KUNIT_EXPECT_EQ(test, value, 100U);
	KUNIT_EXPECT_PTR_EQ(test, current_cred(), caller);
	KUNIT_EXPECT_EQ(test, write_gpu_mode(105), 0);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 105U);
	KUNIT_EXPECT_PTR_EQ(test, current_cred(), caller);
	fp_test_gpu_force_error = true;
	KUNIT_EXPECT_EQ(test, write_gpu_mode(100), -EIO);
	KUNIT_EXPECT_PTR_EQ(test, current_cred(), caller);
	fp_test_gpu_force_error = false;
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
	streams[selected_stream].control.samples = 8;
	streams[selected_stream].control.average_ns = 16666667;
	streams[selected_stream].history.count = FP_FAS_MIN_INTERVALS - 1;
	last_frame_ns = ktime_get_ns();
	apply_frame(&frame_work);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	streams[selected_stream].history.count = FP_FAS_MIN_INTERVALS;
	KUNIT_ASSERT_TRUE(test, enqueue_fas_frame((struct fp_fas_event){
		.ended_ns = ktime_get_ns(), .interval_ns = 16666667,
		.fps = 60, .margin_mfps = 1000,
	}));
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

struct fp_test_game_load {
	struct mm_struct *mm;
	struct completion done;
};

static int fp_test_game_load_thread(void *data)
{
	struct fp_test_game_load *load = data;
	unsigned int i;

	kthread_use_mm(load->mm);
	for (i = 0; i < 80 && !kthread_should_stop(); i++) {
		udelay(500);
		usleep_range(1000, 2000);
	}
	kthread_unuse_mm(load->mm);
	complete(&load->done);
	for (;;) {
		set_current_state(TASK_INTERRUPTIBLE);
		if (kthread_should_stop())
			break;
		schedule();
	}
	__set_current_state(TASK_RUNNING);
	return 0;
}

static void frame_test_game_runtime_events(struct kunit *test)
{
	struct fp_test_game_load load;
	struct task_struct *thread;
	unsigned long flags, completed;
	u64 runtime = 0, before;
	unsigned int cpu;
	int error;

	error = cpufreq_register_driver(&fp_test_cpufreq_driver);
	KUNIT_ASSERT_EQ(test, error, 0);
	fp_test_driver_registered = true;
	configuration.flags = FP_CPU | FP_OBSERVE;
	configuration.cpu_count = 1;
	configuration.cpus[0].cpu = 0;
	target_mm = mm_alloc();
	KUNIT_ASSERT_NOT_NULL(test, target_mm);
	load.mm = target_mm;
	init_completion(&load.done);
	game_active = true;
	before = game_switches;
	error = start_game_accounting();
	KUNIT_ASSERT_EQ(test, error, 0);
	thread = kthread_run(fp_test_game_load_thread, &load, "fp-game-load");
	if (IS_ERR(thread)) {
		KUNIT_FAIL(test, "Cannot start runtime workload");
		return;
	}
	completed = wait_for_completion_timeout(&load.done, 5 * HZ);
	kthread_stop(thread);
	KUNIT_EXPECT_NE(test, completed, 0UL);
	spin_lock_irqsave(&frame_lock, flags);
	sample_game_load(ktime_get_ns());
	for_each_possible_cpu(cpu)
		runtime += game_cpus[cpu].runtime_ns;
	spin_unlock_irqrestore(&frame_lock, flags);
	KUNIT_EXPECT_GT(test, runtime, 0ULL);
	KUNIT_EXPECT_GT(test, game_switches, before);
	KUNIT_EXPECT_GT(test, cpu_loads[0], 0U);
	KUNIT_EXPECT_LE(test, cpu_loads[0], 1000U);
	KUNIT_EXPECT_TRUE(test, game_trace);
	KUNIT_EXPECT_FALSE(test, scene_trace);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	stop_game();
	KUNIT_EXPECT_FALSE(test, game_trace);
	KUNIT_EXPECT_PTR_EQ(test, game_cpus, NULL);
}

static void frame_test_game_caps_and_operating_points(struct kunit *test)
{
	struct fp_limits *limits = &configuration.cpus[0].game;
	int error;

	error = cpufreq_register_driver(&fp_test_cpufreq_driver);
	KUNIT_ASSERT_EQ(test, error, 0);
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	KUNIT_ASSERT_NOT_NULL(test, fp_test_policy);
	configuration.flags = FP_CPU;
	configuration.cpu_count = 1;
	configuration.cpus[0].cpu = 0;
	*limits = (struct fp_limits){ 500000, 4000000 };
	KUNIT_EXPECT_EQ(test, snap_game_ceiling(fp_test_policy, 1400000, limits), 1500000U);
	KUNIT_EXPECT_EQ(test, snap_game_ceiling(fp_test_policy, 999999, limits), 1000000U);
	KUNIT_ASSERT_EQ(test, add_requests(true), 0);
	error = freq_qos_add_request(&fp_test_policy->constraints, &fp_test_cpu_cap,
				     FREQ_QOS_MAX, 1800000);
	KUNIT_ASSERT_GE(test, error, 0);
	/* Critical policies lock to a valid OPP; thermal max remains a separate vote. */
	KUNIT_ASSERT_EQ(test, update_cpu_bounds(0, 3000000, 3000000), 0);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], cpu_ceilings[0]);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
	KUNIT_ASSERT_EQ(test, update_cpu_bounds(0, 1000000, 1000000), 0);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MIN), 1000000);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1000000);
	KUNIT_ASSERT_EQ(test, update_cpu_bounds(0, 500000, 2000000), 0);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 500000U);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
	remove_requests();
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
}

static void frame_test_per_game_relations(struct kunit *test)
{
	struct fp_game_rules *rules = kvzalloc(sizeof(*rules), GFP_KERNEL);
	const struct fp_game_rule *rule;
	struct fp_limits limits[FP_MAX_CPUS] = { {384000, 3532800}, {1017600, 4320000} };
	u32 targets[FP_MAX_CPUS] = { 500000, 2000000 };

	KUNIT_ASSERT_NOT_NULL(test, rules);
	game_rules = rules;
	rules->abi = FP_GAME_RULES_ABI;
	rules->cpu_count = 2;
	rules->cpu_ids[1] = 1;
	rules->count = 2;
	rules->fallback.cpus[0] = (struct fp_relation){ FP_REL_POLICY, 1, -150000, -100000 };
	strscpy(rules->rules[0].package, "com.tencent.tmgp.sgame", 128);
	rules->rules[0].cpus[0] = (struct fp_relation){ FP_REL_POLICY, 1, 0, 0 };
	rules->rules[0].fps_count = 2;
	rules->rules[0].fps_values[0] = 60;
	rules->rules[0].fps_values[1] = 165;
	strscpy(rules->rules[1].package, "com.miHoYo.Yuanshen", 128);
	rules->rules[1].cpus[1] = (struct fp_relation){ FP_REL_POLICY, 0, -400000, -350000 };
	KUNIT_ASSERT_EQ(test, validate_game_rules(rules), 0);
	strscpy(foreground, "com.tencent.tmgp.sgame", sizeof(foreground));
	rule = active_game_rule();
	KUNIT_ASSERT_PTR_EQ(test, rule, &rules->rules[0]);
	KUNIT_EXPECT_EQ(test, rule->fps_count, 2U);
	KUNIT_EXPECT_EQ(test, rule->fps_values[1], 165U);
	KUNIT_EXPECT_TRUE(test, fp_apply_relations(targets, limits, rule->cpus, 2));
	KUNIT_EXPECT_EQ(test, targets[0], 2000000U);
	strscpy(foreground, "com.miHoYo.Yuanshen", sizeof(foreground));
	targets[0] = 2000000;
	targets[1] = 4000000;
	KUNIT_EXPECT_TRUE(test, fp_apply_relations(targets, limits, active_game_rule()->cpus, 2));
	KUNIT_EXPECT_EQ(test, targets[1], 1650000U);
	strscpy(foreground, "game.other", sizeof(foreground));
	targets[0] = 500000;
	targets[1] = 2000000;
	KUNIT_EXPECT_TRUE(test, fp_apply_relations(targets, limits, active_game_rule()->cpus, 2));
	KUNIT_EXPECT_EQ(test, targets[0], 1850000U);
}

static void frame_test_invalid_game_relations(struct kunit *test)
{
	struct fp_game_rules *rules = kunit_kzalloc(test, sizeof(*rules), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, rules);
	rules->abi = FP_GAME_RULES_ABI;
	rules->cpu_count = 2;
	rules->cpu_ids[1] = 1;
	rules->count = 1;
	strscpy(rules->rules[0].package, "game.one", 128);
	KUNIT_ASSERT_EQ(test, validate_game_rules(rules), 0);
	rules->rules[0].fps_count = 2;
	rules->rules[0].fps_values[0] = 165;
	rules->rules[0].fps_values[1] = 60;
	KUNIT_EXPECT_EQ(test, validate_game_rules(rules), -EINVAL);
	rules->rules[0].fps_count = 0;
	memset(rules->rules[0].fps_values, 0, sizeof(rules->rules[0].fps_values));
	rules->rules[0].cpus[0] = (struct fp_relation){ FP_REL_POLICY, 2, 0, 0 };
	KUNIT_EXPECT_EQ(test, validate_game_rules(rules), -EINVAL);
	rules->rules[0].cpus[0].reference = 1;
	rules->rules[0].cpus[1] = (struct fp_relation){ FP_REL_POLICY, 0, 0, 0 };
	KUNIT_EXPECT_EQ(test, validate_game_rules(rules), -EINVAL);
	memset(rules->rules[0].cpus, 0, sizeof(rules->rules[0].cpus));
	rules->count = 2;
	strscpy(rules->rules[1].package, "game.one", 128);
	KUNIT_EXPECT_EQ(test, validate_game_rules(rules), -EINVAL);
	rules->count = 1;
	rules->cpu_ids[1] = 0;
	KUNIT_EXPECT_EQ(test, validate_game_rules(rules), -EINVAL);
}

static void frame_test_fas_task_identity_and_affinity(struct kunit *test)
{
	struct fp_fas_task_sample *previous, *latest;
	struct fp_fas_task_load load;
	unsigned int i;

	previous = kunit_kcalloc(test, 8, sizeof(*previous), GFP_KERNEL);
	latest = kunit_kcalloc(test, 8, sizeof(*latest), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, previous);
	KUNIT_ASSERT_NOT_NULL(test, latest);
	for (i = 0; i < 8; i++) {
		previous[i] = (struct fp_fas_task_sample){
			.tid = 100 + i, .cpu = i % 4, .start_time = i + 1,
			.runtime_ns = 100000, .allowed_mask = i < 6 ? 0xff : 0xc0,
		};
		latest[i] = previous[i];
		latest[i].runtime_ns += (8 - i) * 100000;
	}
	/* One busy thread moving cores is 80% of one CPU, never per-CPU 40%.
	 * Six unrestricted workers do not turn two deliberately bound workers
	 * into an all-core critical mask. */
	latest[0].cpu = 3;
	KUNIT_ASSERT_TRUE(test, fp_fas_task_measure(previous, 8, latest, 8,
						  1000000, &load));
	KUNIT_EXPECT_EQ(test, load.measured, 8U);
	KUNIT_EXPECT_EQ(test, load.util_permille, 800U);
	KUNIT_EXPECT_EQ(test, load.critical_mask, 0xc0ULL);
	latest[1].start_time++; /* PID reuse is an exit and a fresh identity. */
	latest[2].runtime_ns = previous[2].runtime_ns - 1;
	KUNIT_ASSERT_TRUE(test, fp_fas_task_measure(previous, 8, latest, 7,
						  1000000, &load));
	KUNIT_EXPECT_EQ(test, load.new_tasks, 1U);
	KUNIT_EXPECT_EQ(test, load.exited, 2U); /* Reused identity plus final exit. */
	KUNIT_EXPECT_EQ(test, load.rollback, 1U);
	KUNIT_EXPECT_EQ(test, load.measured, 5U);
	KUNIT_EXPECT_EQ(test, load.util_permille, 800U);
	latest[0].cpu = 64;
	KUNIT_EXPECT_FALSE(test, fp_fas_task_measure(previous, 8, latest, 7,
						   1000000, &load));
	KUNIT_EXPECT_EQ(test, load.error, FP_FAS_TASK_INVALID_CPU);
	KUNIT_EXPECT_FALSE(test, load.valid);
}

static int fp_test_prepare_fas(void)
{
	int error = cpufreq_register_driver(&fp_test_cpufreq_driver);

	if (error)
		return error;
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	if (!fp_test_policy)
		return -ENODEV;
	configuration.flags = FP_CPU;
	configuration.cpu_count = 1;
	configuration.cpus[0].cpu = 0;
	configuration.cpus[0].game = (struct fp_limits){500000, 4000000};
	configured = game_active = true;
	applied_generation = generation;
	active_fps = 60;
	selected_stream = 0;
	streams[0].history.count = FP_FAS_MIN_INTERVALS;
	last_frame_ns = ktime_get_ns();
	last_frame_interval_ns = 16666667;
	game_cap_valid[0] = true;
	game_targets[0] = 2010000;
	fas_last_rate_generation = rate_generation;
	fas_feedback.util_sample_ns = last_frame_ns;
	/* Explicit input fixture: isolate the frame-to-QoS path from new task
	 * samples. Collector behavior is not inferred from these cases. */
	fas_task_attempt_ns = U64_MAX;
	fas_util_valid = false;
	fas_critical_mask = 0;
	return 0;
}

static void frame_test_fas_batch_preserves_each_step(struct kunit *test)
{
	u64 before = fas_steps;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, fp_test_prepare_fas(), 0);
	fas_critical_mask = 1;
	for (i = 0; i < 3; i++)
		KUNIT_ASSERT_TRUE(test, enqueue_fas_frame((struct fp_fas_event){
			.ended_ns = last_frame_ns - (3 - i) * 16666667ULL,
			.interval_ns = 16666667, .fps = 60, .margin_mfps = 1000,
		}));
	apply_frame(&frame_work);
	KUNIT_ASSERT_NOT_NULL(test, cpu_requests[0].policy);
	KUNIT_EXPECT_EQ(test, last_error, 0);
	KUNIT_EXPECT_EQ(test, fas_steps, before + 3);
	KUNIT_EXPECT_EQ(test, fas_last_adjustment, -4999LL);
	KUNIT_EXPECT_EQ(test, game_targets[0], 1995003U);
	/* One lost/coalesced P step would round to 3MHz instead of 2MHz. */
	KUNIT_EXPECT_EQ(test, cpu_ceilings[0], 2000000U);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 2000000U);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 2000000);
	KUNIT_EXPECT_EQ(test, fas_events.count, 0U);
}

static void frame_test_fas_long_frame_preserves_thermal_cap(struct kunit *test)
{
	KUNIT_ASSERT_EQ(test, fp_test_prepare_fas(), 0);
	KUNIT_ASSERT_GE(test, freq_qos_add_request(&fp_test_policy->constraints,
						 &fp_test_cpu_cap, FREQ_QOS_MAX, 1800000), 0);
	fas_util_valid = true;
	fas_util_permille = 250;
	KUNIT_ASSERT_TRUE(test, enqueue_fas_frame((struct fp_fas_event){
		.ended_ns = last_frame_ns, .interval_ns = 400000000,
		.fps = 60, .janked = true, .margin_mfps = 0,
	}));
	apply_frame(&frame_work);
	KUNIT_ASSERT_NOT_NULL(test, cpu_requests[0].policy);
	KUNIT_EXPECT_EQ(test, fas_last_adjustment, 6900000LL);
	KUNIT_EXPECT_EQ(test, game_targets[0], 4000000U);
	KUNIT_EXPECT_FALSE(test, fas_util_valid);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
	remove_requests();
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1800000);
}

static void frame_test_fas_tuning_hot_change(struct kunit *test)
{
	struct fp_fas_tuning next = {.abi = FP_FAS_ABI, .mode = 3};
	struct fp_fas_tuning saved;
	u64 before = fas_steps, epoch = generation;
	struct cpufreq_policy *owned;

	KUNIT_ASSERT_EQ(test, fp_test_prepare_fas(), 0);
	KUNIT_ASSERT_EQ(test, add_requests(true), 0);
	owned = cpu_requests[0].policy;
	KUNIT_ASSERT_TRUE(test, enqueue_fas_frame((struct fp_fas_event){
		.ended_ns = last_frame_ns, .interval_ns = 16666667,
		.fps = 60, .margin_mfps = 1000,
	}));
	KUNIT_EXPECT_EQ(test, sizeof(next), (size_t)80);
	KUNIT_EXPECT_EQ(test, _IOC_SIZE(FP_FAS_TUNING), 80U);
	KUNIT_ASSERT_EQ(test, set_fas_tuning(&next, FP_FAS_UTIL_LIMIT_DEFAULT), 0);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, owned);
	KUNIT_EXPECT_EQ(test, generation, epoch);
	KUNIT_EXPECT_EQ(test, game_targets[0], 2010000U);
	KUNIT_EXPECT_EQ(test, fas_events.count, 1U);
	saved = fas_tuning;
	next.mode = FP_FAS_MODES;
	KUNIT_EXPECT_EQ(test, set_fas_tuning(&next, FP_FAS_UTIL_LIMIT_DEFAULT), -EINVAL);
	KUNIT_EXPECT_EQ(test, memcmp(&fas_tuning, &saved, sizeof(saved)), 0);
	next = saved;
	next.overrides[0].fps = 60; /* Unused tail cannot smuggle an active override. */
	KUNIT_EXPECT_EQ(test, set_fas_tuning(&next, FP_FAS_UTIL_LIMIT_DEFAULT), -EINVAL);
	KUNIT_EXPECT_EQ(test, memcmp(&fas_tuning, &saved, sizeof(saved)), 0);
	KUNIT_ASSERT_TRUE(test, enqueue_fas_frame((struct fp_fas_event){
		.ended_ns = last_frame_ns + 16666667, .interval_ns = 16666667,
		.fps = 60, .margin_mfps = fp_fas_margin(&fas_tuning, 60),
	}));
	apply_frame(&frame_work);
	KUNIT_EXPECT_EQ(test, fas_steps, before + 2);
	/* Old queued event keeps its original margin, the next uses fast mode. */
	KUNIT_EXPECT_EQ(test, game_targets[0], 2005001U);
	KUNIT_EXPECT_EQ(test, fas_last_adjustment, 0LL);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 3000000);
}

static void frame_test_fas_adaptive_limit(struct kunit *test)
{
	struct fp_fas_tuning next = {.abi = FP_FAS_ABI, .mode = 1, .margin_base_mfps = 1000};
	struct fp_fas_tuning saved;
	u64 epoch;

	KUNIT_ASSERT_EQ(test, fp_test_prepare_fas(), 0);
	epoch = generation;
	fas_feedback.util_offset_mfps = -3000;
	KUNIT_EXPECT_EQ(test, sizeof(struct fp_fas_tuning_ext), (size_t)88);
	KUNIT_EXPECT_EQ(test, _IOC_NR(FP_FAS_TUNING_EXT), 0x86U);
	KUNIT_EXPECT_EQ(test, _IOC_SIZE(FP_FAS_TUNING_EXT), 88U);
	KUNIT_ASSERT_EQ(test, set_fas_tuning(&next, 501), 0);
	KUNIT_EXPECT_EQ(test, fas_util_limit_mfps, 501U);
	KUNIT_EXPECT_EQ(test, fas_feedback.util_offset_mfps, -501);
	KUNIT_ASSERT_EQ(test, set_fas_tuning(&next, 0), 0);
	KUNIT_EXPECT_EQ(test, fas_feedback.util_offset_mfps, 0);
	KUNIT_EXPECT_EQ(test, generation, epoch);
	saved = fas_tuning;
	KUNIT_EXPECT_EQ(test, set_fas_tuning(&next, 3001), -EINVAL);
	KUNIT_EXPECT_EQ(test, memcmp(&fas_tuning, &saved, sizeof(saved)), 0);
	KUNIT_EXPECT_EQ(test, fas_util_limit_mfps, 0U);
	/* The original ioctl/old generations retain their 3 FPS behavior. */
	KUNIT_ASSERT_EQ(test, set_fas_tuning(&next, FP_FAS_UTIL_LIMIT_DEFAULT), 0);
	KUNIT_EXPECT_EQ(test, fas_util_limit_mfps, 3000U);
}

static void frame_test_fas_overflow_releases_votes(struct kunit *test)
{
	u64 before = fas_overflows;
	unsigned int i;

	KUNIT_ASSERT_EQ(test, fp_test_prepare_fas(), 0);
	KUNIT_ASSERT_EQ(test, add_requests(true), 0);
	KUNIT_ASSERT_NOT_NULL(test, cpu_requests[0].policy);
	for (i = 0; i < FP_FAS_EVENTS; i++)
		KUNIT_ASSERT_TRUE(test, enqueue_fas_frame((struct fp_fas_event){
			.ended_ns = last_frame_ns, .interval_ns = 16666667, .fps = 60,
		}));
	KUNIT_EXPECT_FALSE(test, enqueue_fas_frame((struct fp_fas_event){.fps = 60}));
	KUNIT_EXPECT_TRUE(test, fas_overflowed);
	KUNIT_EXPECT_EQ(test, fas_overflows, before + 1);
	apply_frame(&frame_work);
	KUNIT_EXPECT_EQ(test, last_error, -EOVERFLOW);
	KUNIT_EXPECT_STREQ(test, last_error_stage, "fas_queue_overflow");
	KUNIT_EXPECT_FALSE(test, configured);
	KUNIT_EXPECT_FALSE(test, game_active);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	KUNIT_EXPECT_EQ(test, fas_events.count, 0U);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&expire_work));
}

static void frame_test_fas_stale_generation_has_no_votes(struct kunit *test)
{
	u64 before = fas_steps;
	unsigned long flags;

	KUNIT_ASSERT_EQ(test, fp_test_prepare_fas(), 0);
	KUNIT_ASSERT_TRUE(test, enqueue_fas_frame((struct fp_fas_event){
		.ended_ns = last_frame_ns, .interval_ns = 20000000, .fps = 60,
	}));
	spin_lock_irqsave(&frame_lock, flags);
	generation++;
	spin_unlock_irqrestore(&frame_lock, flags);
	apply_frame(&frame_work);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_EQ(test, fas_steps, before);
	KUNIT_EXPECT_EQ(test, game_targets[0], 2010000U);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&expire_work));
}

static void frame_test_fas_late_deadline_is_once(struct kunit *test)
{
	struct fp_fas_event event = {};
	u64 anchor, deadline, first_deadline, fresh;
	unsigned long flags;
	bool early, due, twice, old_frame, rearmed, old_rate, old_epoch;
	u32 queued;

	KUNIT_ASSERT_EQ(test, fp_test_prepare_fas(), 0);
	anchor = last_frame_ns;
	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	late_frame_ns = anchor;
	late_rate_generation = rate_generation;
	late_sent = false;
	deadline = anchor + FP_INACTIVITY_NS;
	early = queue_late_frame(anchor + FP_LATE_MIN_NS - 1, &deadline);
	first_deadline = deadline;
	due = queue_late_frame(anchor + 60000000, &deadline);
	twice = queue_late_frame(anchor + 90000000, &deadline);
	queued = fas_events.count;
	fp_fas_event_pop(&fas_events, &event);
	/* Future fixture anchor keeps the actual delayed work safely pending
	 * until cancellation; logical due times below are explicit test inputs. */
	fresh = ktime_get_ns() + NSEC_PER_SEC;
	last_frame_ns = fresh;
	late_sent = false;
	old_frame = queue_late_frame(fresh + 60000000, &deadline);
	spin_unlock_irqrestore(&frame_lock, flags);
	/* Exercise the same arming function used after a new real submission. */
	arm_frame_expiry(generation, rate_generation, fresh);
	mutex_unlock(&config_lock);
	cancel_delayed_work_sync(&expire_work);
	mutex_lock(&config_lock);
	spin_lock_irqsave(&frame_lock, flags);
	rearmed = queue_late_frame(fresh + 60000000, &deadline);
	rate_generation++;
	late_sent = false;
	old_rate = queue_late_frame(fresh + 90000000, &deadline);
	late_rate_generation = rate_generation;
	generation++;
	old_epoch = queue_late_frame(fresh + 90000000, &deadline);
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	KUNIT_EXPECT_FALSE(test, early);
	KUNIT_EXPECT_EQ(test, first_deadline, anchor + FP_LATE_MIN_NS);
	KUNIT_EXPECT_TRUE(test, due);
	KUNIT_EXPECT_FALSE(test, twice);
	KUNIT_EXPECT_EQ(test, queued, 1U);
	KUNIT_EXPECT_TRUE(test, event.late);
	KUNIT_EXPECT_EQ(test, event.interval_ns, 60000000ULL);
	KUNIT_EXPECT_FALSE(test, old_frame);
	KUNIT_EXPECT_TRUE(test, rearmed);
	KUNIT_EXPECT_FALSE(test, old_rate);
	KUNIT_EXPECT_FALSE(test, old_epoch);
}

static void frame_test_fas_inactivity_releases_without_poll(struct kunit *test)
{
	u64 before = fas_steps;
	unsigned long flags;

	KUNIT_ASSERT_EQ(test, fp_test_prepare_fas(), 0);
	KUNIT_ASSERT_EQ(test, add_requests(true), 0);
	KUNIT_ASSERT_NOT_NULL(test, cpu_requests[0].policy);
	spin_lock_irqsave(&frame_lock, flags);
	last_frame_ns = ktime_get_ns() - FP_INACTIVITY_NS - 1;
	spin_unlock_irqrestore(&frame_lock, flags);
	expire_frame(&expire_work.work);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	KUNIT_EXPECT_EQ(test, active_fps, 0U);
	KUNIT_EXPECT_EQ(test, streams[selected_stream].history.count, 0U);
	KUNIT_EXPECT_EQ(test, fas_events.count, 0U);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&expire_work));
	KUNIT_EXPECT_EQ(test, fas_steps, before);
	expire_frame(&expire_work.work);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&expire_work));
	KUNIT_EXPECT_EQ(test, fas_steps, before);
}

static void frame_test_source_loss(struct kunit *test)
{
	configured = true;
	/* Establish real published state before testing source disconnection. */
	frame_policy_scene("frame.policy.kunit.live-source", false, false);
	flush_work(&control_work);
	KUNIT_ASSERT_TRUE(test, source_valid);
	KUNIT_ASSERT_TRUE(test, scene_active);
	frame_policy_scene(NULL, true, false);
	flush_work(&control_work);
	KUNIT_EXPECT_FALSE(test, scene_active);
	KUNIT_EXPECT_FALSE(test, game_active);
	KUNIT_EXPECT_FALSE(test, scene_trace);
	KUNIT_EXPECT_PTR_EQ(test, target, NULL);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
}

static void frame_test_source_deduplication(struct kunit *test)
{
	u64 epoch;

	/* Duplicate accessibility/window notifications must not restart sampling. */
	frame_policy_scene("frame.policy.kunit.dedup", false, false);
	epoch = generation;
	frame_policy_scene("frame.policy.kunit.dedup", false, false);
	KUNIT_EXPECT_EQ(test, generation, epoch);
	frame_policy_scene("frame.policy.kunit.dedup", true, false);
	KUNIT_EXPECT_EQ(test, generation, epoch + 1);
	frame_policy_scene("frame.policy.kunit.dedup", false, true);
	KUNIT_EXPECT_EQ(test, generation, epoch + 2);
	/* Visible/windowed-only epochs do not restart a continuing FAS owner. */
	streams[0].history.count = 61;
	streams[0].history.sum_ns = 508333333;
	streams[0].surface = 123;
	frame_policy_scene_full("frame.policy.kunit.dedup", false, true, 47);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, generation, epoch + 2);
	KUNIT_EXPECT_EQ(test, pending_scene.provider_epoch, 47ULL);
	KUNIT_EXPECT_EQ(test, streams[0].history.count, 61U);
	KUNIT_EXPECT_EQ(test, streams[0].history.sum_ns, 508333333ULL);
	KUNIT_EXPECT_EQ(test, streams[0].surface, 123ULL);
	frame_policy_scene(NULL, true, false);
	KUNIT_EXPECT_EQ(test, generation, epoch + 3);
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

#include "scene_policy_kunit.h"

static struct kunit_case frame_policy_cases[] = {
	FP_SCENE_KUNIT_CASES,
	KUNIT_CASE(frame_test_sched_switch_handoff),
	KUNIT_CASE(frame_test_qos_ownership),
	KUNIT_CASE(frame_test_native_cycle_feedback),
	KUNIT_CASE(frame_test_gpu_governor_lease),
	KUNIT_CASE(frame_test_gpu_file_credentials),
	KUNIT_CASE(frame_test_fence_signal_and_cancel),
	KUNIT_CASE(frame_test_fence_cancel_race),
	KUNIT_CASE(frame_test_ready_and_stale_fences),
	KUNIT_CASE(frame_test_cadence_gate_and_qos_release),
	KUNIT_CASE(frame_test_fence_grade_and_error_isolation),
	KUNIT_CASE(frame_test_game_runtime_events),
	KUNIT_CASE(frame_test_game_caps_and_operating_points),
	KUNIT_CASE(frame_test_per_game_relations),
	KUNIT_CASE(frame_test_invalid_game_relations),
	KUNIT_CASE(frame_test_fas_task_identity_and_affinity),
	KUNIT_CASE(frame_test_fas_batch_preserves_each_step),
	KUNIT_CASE(frame_test_fas_long_frame_preserves_thermal_cap),
	KUNIT_CASE(frame_test_fas_tuning_hot_change),
	KUNIT_CASE(frame_test_fas_adaptive_limit),
	KUNIT_CASE(frame_test_fas_overflow_releases_votes),
	KUNIT_CASE(frame_test_fas_stale_generation_has_no_votes),
	KUNIT_CASE(frame_test_fas_late_deadline_is_once),
	KUNIT_CASE(frame_test_fas_inactivity_releases_without_poll),
	KUNIT_CASE(frame_test_source_loss),
	KUNIT_CASE(frame_test_source_deduplication),
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
