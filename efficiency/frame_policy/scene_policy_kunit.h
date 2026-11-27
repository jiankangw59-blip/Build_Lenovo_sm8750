/* SPDX-License-Identifier: GPL-2.0 */
/* Include before frame_policy_cases[], after <kunit/test.h>. */
#include "scene_policy.h"

static struct fp_scene_table *fp_scene_test_table(struct kunit *test)
{
	struct fp_scene_table *table = kunit_kzalloc(test, sizeof(*table), GFP_KERNEL);
	u32 i;

	if (!table)
		return NULL;
	table->abi = FP_SCENE_ABI;
	table->cpu_count = 1;
	table->cpu_ids[0] = 0;
	table->profile_count = 3;
	table->defaults[FP_SCENE_ACTIVE] = 0;
	table->defaults[FP_SCENE_INACTIVE] = 1;
	table->defaults[FP_SCENE_OFF] = 2; /* Explicit ignored/off profile. */
	for (i = 0; i < 2; i++) {
		struct fp_scene_profile *profile = &table->profiles[i];

		profile->flags = FP_SCENE_ENABLED;
		profile->gpu_percent = 105;
		profile->gpu = (struct fp_limits){330000, 1083000};
		profile->cpus[0].limits = (struct fp_limits){500000, 3000000};
	}
	table->profiles[0].cpus[0].margin_count = 2;
	table->profiles[0].cpus[0].margins[0].percent = 200;
	table->profiles[0].cpus[0].margins[1].threshold_khz = 1500000;
	table->profiles[0].cpus[0].margins[1].percent = 90;
	return table;
}

static void frame_test_scene_selection_and_game_exclusion(struct kunit *test)
{
	struct fp_scene_table *table = fp_scene_test_table(test);

	KUNIT_ASSERT_NOT_NULL(test, table);
	KUNIT_ASSERT_TRUE(test, fp_scene_table_valid(table));
	KUNIT_EXPECT_PTR_EQ(test, fp_scene_select(table, "com.example.app", false, false, false),
			    &table->profiles[0]);
	KUNIT_EXPECT_PTR_EQ(test, fp_scene_select(table, "com.example.app", false, false, true),
			    &table->profiles[1]);
	KUNIT_EXPECT_PTR_EQ(test, fp_scene_select(table, "com.example.app", true, false, false), NULL);
	KUNIT_EXPECT_PTR_EQ(test, fp_scene_select(table, "com.example.app", false, true, false), NULL);
	table->count = 1;
	strscpy(table->rules[0].package, "com.example.camera", sizeof(table->rules[0].package));
	table->rules[0].profiles[0] = 1;
	KUNIT_ASSERT_TRUE(test, fp_scene_table_valid(table));
	KUNIT_EXPECT_PTR_EQ(test, fp_scene_select(table, "com.example.camera", false, false, false),
			    &table->profiles[1]);
	KUNIT_EXPECT_PTR_EQ(test, fp_scene_select(table, "com.example.camera.other", false, false, false),
			    &table->profiles[0]);
}

static void frame_test_scene_validation_boundaries(struct kunit *test)
{
	struct fp_scene_table *table = fp_scene_test_table(test);

	KUNIT_ASSERT_NOT_NULL(test, table);
	table->defaults[0] = table->profile_count;
	KUNIT_EXPECT_FALSE(test, fp_scene_table_valid(table));
	table->defaults[0] = 0;
	table->profiles[0].cpus[0].margin_count = FP_MAX_MARGINS + 1;
	KUNIT_EXPECT_FALSE(test, fp_scene_table_valid(table));
	table->profiles[0].cpus[0].margin_count = 2;
	table->profiles[0].cpus[0].margins[1].threshold_khz = 0;
	KUNIT_EXPECT_FALSE(test, fp_scene_table_valid(table));
	table->profiles[0].cpus[0].margins[1].threshold_khz = 1500000;
	table->profiles[0].gpu_percent = 201;
	KUNIT_EXPECT_FALSE(test, fp_scene_table_valid(table));
	table->profiles[0].gpu_percent = 105;
	table->profiles[2].gpu_percent = 105; /* Ignored profile cannot retain a lease. */
	KUNIT_EXPECT_FALSE(test, fp_scene_table_valid(table));
	table->profiles[2].gpu_percent = 0;
	table->count = 2;
	strscpy(table->rules[0].package, "com.example.app", sizeof(table->rules[0].package));
	strscpy(table->rules[1].package, "com.example.app", sizeof(table->rules[1].package));
	KUNIT_EXPECT_FALSE(test, fp_scene_table_valid(table));
	table->count = 0;
	KUNIT_EXPECT_TRUE(test, fp_scene_table_valid(table));
}

static void frame_test_scene_static_and_dynamic_feedback(struct kunit *test)
{
	struct fp_scene_table *table = fp_scene_test_table(test);
	const struct fp_scene_cpu *cpu;

	KUNIT_ASSERT_NOT_NULL(test, table);
	cpu = &table->profiles[0].cpus[0];
	KUNIT_EXPECT_TRUE(test, fp_scene_uses_feedback(&table->profiles[0], 1));
	KUNIT_EXPECT_FALSE(test, fp_scene_uses_feedback(&table->profiles[1], 1));
	KUNIT_EXPECT_FALSE(test, fp_scene_uses_feedback(NULL, 1));
	KUNIT_EXPECT_EQ(test, fp_scene_profile_ceiling(0, cpu), 500000U);
	KUNIT_EXPECT_EQ(test, fp_scene_profile_ceiling(1000000, cpu), 2000000U);
	KUNIT_EXPECT_EQ(test, fp_scene_profile_ceiling(1500000, cpu), 1350000U);
	KUNIT_EXPECT_EQ(test, fp_scene_profile_ceiling(10000000, cpu), 3000000U);
	KUNIT_EXPECT_EQ(test, fp_scene_profile_ceiling(0, &table->profiles[1].cpus[0]), 3000000U);
}

static void frame_test_scene_table_topology(struct kunit *test)
{
	struct fp_scene_table *table = fp_scene_test_table(test);
	struct fp_config *config = kunit_kzalloc(test, sizeof(*config), GFP_KERNEL);

	KUNIT_ASSERT_NOT_NULL(test, table);
	KUNIT_ASSERT_NOT_NULL(test, config);
	config->cpu_count = 1;
	config->cpus[0].cpu = 0;
	KUNIT_EXPECT_TRUE(test, fp_scene_topology_matches(table, config));
	config->cpus[0].cpu = 1;
	KUNIT_EXPECT_FALSE(test, fp_scene_topology_matches(table, config));
	config->cpus[0].cpu = 0;
	config->cpu_count = 2;
	KUNIT_EXPECT_FALSE(test, fp_scene_topology_matches(table, config));
	KUNIT_EXPECT_EQ(test, sizeof(struct fp_scene_table), (size_t)14384);
	KUNIT_EXPECT_EQ(test, sizeof(struct fp_config), (size_t)592);
}

static void frame_test_scene_control_ownership(struct kunit *test)
{
	struct fp_scene_table *table;
	struct fp_scene_profile *normal, *off;
	struct file *file;
	unsigned long flags;
	int error;

	error = cpufreq_register_driver(&fp_test_cpufreq_driver);
	KUNIT_ASSERT_EQ(test, error, 0);
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	KUNIT_ASSERT_NOT_NULL(test, fp_test_policy);
	fp_test_gpu = platform_device_register_simple("frame-kunit-gpu", -1, NULL, 0);
	if (IS_ERR(fp_test_gpu)) {
		fp_test_gpu = NULL;
		KUNIT_FAIL(test, "Cannot register Scene synthetic GPU device");
		return;
	}
	/* Published tables are owned by fp_test_reset(), not KUnit allocation. */
	table = kvzalloc(sizeof(*table), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, table);
	scene_table = table;
	table->abi = FP_SCENE_ABI;
	table->cpu_count = 1;
	table->cpu_ids[0] = 0;
	table->profile_count = 3;
	table->defaults[FP_SCENE_ACTIVE] = 0;
	table->defaults[FP_SCENE_INACTIVE] = 0;
	table->defaults[FP_SCENE_OFF] = 2;
	table->count = 1;
	strscpy(table->rules[0].package, "frame.policy.kunit.ignored",
		 sizeof(table->rules[0].package));
	table->rules[0].profiles[0] = 1;
	table->rules[0].profiles[1] = 1;
	table->rules[0].profiles[2] = 1;
	normal = &table->profiles[0];
	normal->flags = FP_SCENE_ENABLED;
	normal->gpu_percent = 105;
	normal->gpu = (struct fp_limits){330000, 1083000};
	normal->cpus[0].limits = (struct fp_limits){750000, 2000000};
	off = &table->profiles[2];
	off->flags = FP_SCENE_ENABLED;
	off->gpu = (struct fp_limits){300000, 600000};
	off->cpus[0].limits = (struct fp_limits){500000, 1000000};
	KUNIT_ASSERT_TRUE(test, fp_scene_table_valid(table));
	configuration.cpu_count = 1;
	configuration.cpus[0].cpu = 0;
	/* Deliberately different legacy bounds detect accidental fallback. */
	configuration.cpus[0].scene = (struct fp_limits){1500000, 3000000};
	configuration.cpus[0].idle = (struct fp_limits){1000000, 1500000};
	configuration.cpus[0].game = (struct fp_limits){2500000, 4000000};
	strscpy(configuration.gpu_device, "frame-kunit-gpu", sizeof(configuration.gpu_device));
	configuration.gpu_scene = (struct fp_limits){500000, 1200000};
	configuration.gpu_idle = (struct fp_limits){500000, 1000000};
	configuration.gpu_game = (struct fp_limits){500000, 1200000};
	configuration.gpu_game_percent = 100;
	configuration.flags = FP_CPU | FP_GPU;
	file = anon_inode_getfile("scene-gpu-lease", &fp_test_gpu_file_ops, NULL, O_RDWR);
	KUNIT_ASSERT_FALSE(test, IS_ERR(file));
	gpu_governor_file = file;
	WRITE_ONCE(fp_test_gpu_percent, 100);
	configured = true;

	/* Exercise the real event publisher, ordered worker and actual QoS lists. */
	frame_policy_scene("frame.policy.kunit.scene", false, false);
	flush_work(&control_work);
	KUNIT_ASSERT_EQ(test, last_error, 0);
	KUNIT_EXPECT_TRUE(test, scene_active);
	KUNIT_EXPECT_FALSE(test, game_active);
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, normal);
	KUNIT_EXPECT_NOT_NULL(test, cpu_requests[0].policy);
	KUNIT_EXPECT_NOT_NULL(test, gpu_device);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_FALSE(test, scene_trace);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MIN), 750000);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 2000000);
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 1083000);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 105U);
	KUNIT_EXPECT_TRUE(test, gpu_mode_owned);
	/* Reproduce the live fault: platform replaces Scene105 with100, while
	 * Scene still desires105. Preserve100 without disabling either QoS axis
	 * or freezing subsequent real foreground events behind configured=false.
	 */
	WRITE_ONCE(fp_test_gpu_percent, 100);
	fp_test_gpu_force_error = true;
	frame_policy_scene("frame.policy.kunit.scene.foreign-owner", false, false);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, last_error, 0);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_TRUE(test, scene_active);
	KUNIT_EXPECT_NOT_NULL(test, cpu_requests[0].policy);
	KUNIT_EXPECT_NOT_NULL(test, gpu_device);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 2000000);
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 1083000);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 100U);
	KUNIT_EXPECT_FALSE(test, gpu_mode_owned);
	KUNIT_EXPECT_TRUE(test, gpu_mode_conflict);

	frame_policy_scene("frame.policy.kunit.ignored", false, false);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, last_error, 0);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_FALSE(test, scene_active);
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, NULL);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 100U);
	KUNIT_EXPECT_FALSE(test, gpu_mode_owned);
	KUNIT_EXPECT_TRUE(test, gpu_mode_conflict);
	KUNIT_EXPECT_FALSE(test, freq_qos_request_active(&cpu_requests[0].min));
	KUNIT_EXPECT_FALSE(test, freq_qos_request_active(&cpu_requests[0].max));

	frame_policy_scene("frame.policy.kunit.scene", true, false);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, last_error, 0);
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, off);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_FALSE(test, scene_trace);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1000000);
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 600000);

	/* A game with no live process must still retire all Scene ownership. */
	frame_policy_scene("frame.policy.kunit.missing-game", false, true);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, last_error, 0);
	KUNIT_EXPECT_FALSE(test, scene_active);
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, NULL);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 100U);
	KUNIT_EXPECT_TRUE(test, gpu_mode_owned);
	KUNIT_EXPECT_FALSE(test, gpu_mode_conflict);

	/* Model an already-qualified game; the outgoing requests themselves are
	 * real QoS requests. Android libgui/uprobes are outside this QEMU test.
	 */
	mutex_lock(&config_lock);
	error = add_requests(true);
	spin_lock_irqsave(&frame_lock, flags);
	game_active = !error;
	spin_unlock_irqrestore(&frame_lock, flags);
	mutex_unlock(&config_lock);
	KUNIT_ASSERT_EQ(test, error, 0);
	KUNIT_ASSERT_TRUE(test, game_active);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 2500000U);
	KUNIT_EXPECT_NOT_NULL(test, gpu_device);
	fp_test_gpu_force_error = false;
	frame_policy_scene("frame.policy.kunit.scene", false, false);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, last_error, 0);
	KUNIT_EXPECT_FALSE(test, game_active);
	KUNIT_EXPECT_TRUE(test, scene_active);
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, normal);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_FALSE(test, game_trace);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 750000U);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 2000000);
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 1083000);
	KUNIT_EXPECT_EQ(test, READ_ONCE(fp_test_gpu_percent), 105U);
	KUNIT_EXPECT_TRUE(test, configured);
	/* A real mode-file write failure still tears down control safely. */
	fp_test_gpu_force_error = true;
	frame_policy_scene("frame.policy.kunit.missing-game.io-failure", false, true);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, last_error, -EIO);
	KUNIT_EXPECT_STREQ(test, last_error_stage, "gpu_mode_write");
	KUNIT_EXPECT_FALSE(test, configured);
	KUNIT_EXPECT_FALSE(test, scene_active);
	KUNIT_EXPECT_FALSE(test, game_active);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
}

static void fp_test_connect_before_open(struct input_dev *dev)
{
	if (dev == fp_test_activity_input) {
		/* The handle is still closed: no handler callback can report this key. */
		input_report_key(dev, BTN_TOUCH, 1);
		input_sync(dev);
	}
}

struct fp_test_aux_value {
	int value, error, read_error;
	u32 writes;
	bool check_credentials;
	struct fp_test_aux_value *foreign;
	u64 *advance_epoch;
};

static ssize_t fp_test_aux_read(struct kiocb *iocb, struct iov_iter *output)
{
	struct fp_test_aux_value *value = iocb->ki_filp->private_data;
	char text[32];
	int length = scnprintf(text, sizeof(text), "%d\n", value->value);

	if (value->check_credentials && current_cred() != iocb->ki_filp->f_cred)
		return -EACCES;
	if (value->read_error)
		return value->read_error;
	return copy_to_iter(text, length, output) == length ? length : -EFAULT;
}

static ssize_t fp_test_aux_write(struct kiocb *iocb, struct iov_iter *input)
{
	struct fp_test_aux_value *value = iocb->ki_filp->private_data;
	char text[32];
	size_t length = iov_iter_count(input);
	int desired;

	if (value->check_credentials && current_cred() != iocb->ki_filp->f_cred)
		return -EACCES;
	if (value->error) {
		if (value->foreign)
			value->foreign->value = 7777;
		return value->error;
	}
	if (length >= sizeof(text) || copy_from_iter(text, length, input) != length)
		return -EFAULT;
	text[length] = 0;
	if (kstrtoint(strim(text), 10, &desired))
		return -EINVAL;
	value->value = desired;
	value->writes++;
	if (value->advance_epoch)
		(*value->advance_epoch)++;
	return length;
}

static const struct file_operations fp_test_aux_ops = {
	.read_iter = fp_test_aux_read,
	.write_iter = fp_test_aux_write,
	.llseek = noop_llseek,
};

static int fp_test_install_aux_metadata(struct kunit *test, struct fp_test_aux_value *values,
		u32 mask, const struct fp_scene_aux_cpu *metadata)
{
	struct file *files[FP_MAX_CPUS][FP_SCENE_AUX_KNOBS] = {};
	struct fp_scene_aux_table *table;
	struct fp_scene_aux_cpu *cpu;
	u32 i, j;
	int error = 0;

	table = kvzalloc(sizeof(*table), GFP_KERNEL);
	if (!table)
		return -ENOMEM;
	table->abi = FP_SCENE_AUX_ABI;
	table->cpu_count = 1;
	table->profile_count = 3;
	table->defaults[FP_SCENE_INACTIVE] = 1;
	table->defaults[FP_SCENE_OFF] = 2;
	table->expected_scene_epoch = scene_table_epoch;
	for (i = 0; i < FP_MAX_CPUS; i++)
		for (j = 0; j < FP_SCENE_AUX_KNOBS; j++) table->fds[i][j] = -1;
	cpu = &table->profiles[0].cpus[0];
	cpu->valid_mask = mask;
	if (mask & FP_AUX_UP_RATE) cpu->up_us = 1000;
	if (mask & FP_AUX_DOWN_RATE) cpu->down_us = 1000;
	if (mask & FP_AUX_HISPEED) cpu->hispeed_khz = 748800;
	if (mask & FP_AUX_BOOST) cpu->walt_boost = -2;
	if (mask & FP_AUX_CORE_CTL) { cpu->core_enable = 1; cpu->max_cpus = 2; }
	if (metadata) {
		size_t offset = offsetof(struct fp_scene_aux_cpu, excludes_mask);

		memcpy((char *)cpu + offset, (const char *)metadata + offset, sizeof(*cpu) - offset);
		table->options.valid_mask = FP_AUX_OPT_SOURCE | FP_AUX_OPT_STAT_SOURCE |
			FP_AUX_OPT_GESTURE | FP_AUX_OPT_GESTURE_DURATION | FP_AUX_OPT_PANEL_PEAK;
		table->options.source = FP_AUX_SOURCE_ONLINE;
		table->options.stat_source = 1;
		table->options.gesture_boost = 1;
		table->options.panel_peak_hz = 185;
	}
	table->profiles[1] = table->profiles[0];
	if (mask & FP_AUX_UP_RATE) table->profiles[1].cpus[0].up_us = 2000;
	if (mask & FP_AUX_BOOST) table->profiles[1].cpus[0].walt_boost = -5;
	for (j = 0; j < FP_SCENE_AUX_KNOBS; j++) {
		if (!(fp_scene_aux_required(table, 0) & fp_scene_aux_knob_bit(j)))
			continue;
		files[0][j] = anon_inode_getfile("scene-aux-kunit", &fp_test_aux_ops, &values[j], O_RDWR);
		if (IS_ERR(files[0][j])) {
			error = PTR_ERR(files[0][j]); files[0][j] = NULL; break;
		}
		table->fds[0][j] = j; /* Actual private files passed to the test-only gate. */
	}
	if (!error) {
		mutex_lock(&config_lock);
		error = fp_scene_aux_test_replace(table, scene_table, scene_table_epoch, files);
		mutex_unlock(&config_lock);
	}
	for (j = 0; j < FP_SCENE_AUX_KNOBS; j++)
		if (files[0][j]) fput(files[0][j]);
	if (error)
		kvfree(table);
	(void)test;
	return error;
}

static int fp_test_install_aux(struct kunit *test, struct fp_test_aux_value *values, u32 mask)
{
	return fp_test_install_aux_metadata(test, values, mask, NULL);
}

static void fp_test_aux_apply(const char *package, bool game, bool off, bool inactive, bool allowed)
{
	fp_scene_aux_apply(package, game, off, inactive, allowed, 0, NULL);
}

static void frame_test_scene_aux_leases_and_rollback(struct kunit *test)
{
	struct fp_scene_aux_cpu metadata = {
		.feedback_mask = FP_AUX_FEEDBACK_CORE_CTL,
		.limiter_cluster_count = 1,
	};
	struct fp_test_aux_value *values;
	struct fp_scene_table *scene;
	struct fp_aux_status aux;
	u32 writes;
	u32 map, retained, n;
	u64 epoch = 7;
	int error;

	KUNIT_ASSERT_EQ(test, cpufreq_register_driver(&fp_test_cpufreq_driver), 0);
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	KUNIT_ASSERT_NOT_NULL(test, fp_test_policy);
	scene = kvzalloc(sizeof(*scene), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, scene);
	scene_table = scene;
	scene->cpu_count = 1;
	scene_table_epoch++;
	values = kunit_kcalloc(test, FP_SCENE_AUX_KNOBS, sizeof(*values), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, values);
	values[FP_AUX_KNOB_HISPEED].value = 1344000;
	values[FP_AUX_KNOB_CORE_MIN].value = cpumask_weight(fp_test_policy->related_cpus);
	values[FP_AUX_KNOB_CORE_MAX].value = values[FP_AUX_KNOB_CORE_MIN].value;
	KUNIT_ASSERT_EQ(test, fp_test_install_aux(test, values, FP_AUX_DIRECT_MASK), 0);
	mutex_lock(&config_lock);
	configured = true;
	fp_test_aux_apply("test.scene", false, false, false, true);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_BOOST].value, -2);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MIN].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MAX].value, 2);
	writes = values[FP_AUX_KNOB_UP_RATE].writes;
	fp_test_aux_apply("test.scene", false, false, false, true);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_UP_RATE].writes, writes);
	values[FP_AUX_KNOB_UP_RATE].value = 7777; /* Foreign owner must survive. */
	fp_test_aux_apply("test.scene", false, false, true, true);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, 0);
	KUNIT_EXPECT_EQ(test, aux.conflicts, 1U);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_UP_RATE].value, 7777);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_BOOST].value, -5);
	fp_test_aux_apply("test.game", true, false, false, true);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_UP_RATE].value, 7777);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_BOOST].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_ENABLE].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MIN].value,
		(int)cpumask_weight(fp_test_policy->related_cpus));
	fp_test_aux_apply("test.scene", false, false, false, true);
	values[FP_AUX_KNOB_CORE_MAX].value = 3;
	fp_test_aux_apply("test.scene", false, false, true, true);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, 0);
	KUNIT_EXPECT_EQ(test, aux.conflicts, 2U);
	fp_test_aux_apply("test.game", true, false, false, true);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_ENABLE].value, 1);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MIN].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MAX].value, 3);
	KUNIT_EXPECT_EQ(test, fp_scene_aux_clear(), 0);
	mutex_unlock(&config_lock);
	values[FP_AUX_KNOB_UP_RATE].value = 0;
	KUNIT_ASSERT_EQ(test, fp_test_install_aux(test, values, FP_AUX_DIRECT_MASK), 0);
	values[FP_AUX_KNOB_BOOST].error = -EIO;
	values[FP_AUX_KNOB_BOOST].foreign = &values[FP_AUX_KNOB_UP_RATE];
	mutex_lock(&config_lock);
	fp_test_aux_apply("test.scene", false, false, false, true);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, -EIO);
	KUNIT_EXPECT_STREQ(test, aux.stage, "aux_write");
	KUNIT_EXPECT_EQ(test, aux.profile, -1);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_UP_RATE].value, 7777);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_DOWN_RATE].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_HISPEED].value, 1344000);
	values[FP_AUX_KNOB_BOOST].error = 0;
	values[FP_AUX_KNOB_BOOST].foreign = NULL;
	fp_test_aux_apply("test.scene.retry", false, false, false, true);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_BOOST].value, -2);
	values[FP_AUX_KNOB_UP_RATE].read_error = -EIO;
	fp_test_aux_apply("test.scene.read-failure", false, false, false, true);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, -EIO);
	KUNIT_EXPECT_STREQ(test, aux.stage, "aux_read");
	KUNIT_EXPECT_TRUE(test, configured);
	values[FP_AUX_KNOB_UP_RATE].read_error = 0;
	KUNIT_EXPECT_EQ(test, fp_scene_aux_clear(), 0);
	mutex_unlock(&config_lock);
	values[FP_AUX_KNOB_UP_RATE].value = 0;
	KUNIT_ASSERT_EQ(test, fp_test_install_aux(test, values, FP_AUX_DIRECT_MASK), 0);
	values[FP_AUX_KNOB_UP_RATE].advance_epoch = &epoch;
	writes = values[FP_AUX_KNOB_DOWN_RATE].writes;
	mutex_lock(&config_lock);
	fp_scene_aux_apply("test.scene.cancel", false, false, false, true, 7, &epoch);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, 0);
	KUNIT_EXPECT_STREQ(test, aux.stage, "aux_stale");
	KUNIT_EXPECT_EQ(test, aux.profile, -1);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_UP_RATE].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_DOWN_RATE].writes, writes);
	KUNIT_EXPECT_TRUE(test, configured);
	values[FP_AUX_KNOB_UP_RATE].advance_epoch = NULL;
	mutex_unlock(&config_lock);

	/* Dynamic original ordered-core path uses the same real file leases.
	 * Its force branch must never transiently enable an externally-disabled
	 * vendor controller. A foreign core member yields the whole group only.
	 */
	mutex_lock(&config_lock);
	error = fp_scene_aux_clear();
	mutex_unlock(&config_lock);
	KUNIT_ASSERT_EQ(test, error, 0);
	memset(values, 0, sizeof(*values) * FP_SCENE_AUX_KNOBS);
	n = cpumask_weight(fp_test_policy->related_cpus);
	values[FP_AUX_KNOB_CORE_MIN].value = n > 1 ? n - 1 : 0;
	values[FP_AUX_KNOB_CORE_MAX].value = n;
	KUNIT_ASSERT_EQ(test, fp_test_install_aux_metadata(test, values, FP_AUX_UP_RATE, &metadata), 0);
	mutex_lock(&config_lock);
	fp_test_aux_apply("test.native.core", false, false, false, true);
	KUNIT_EXPECT_EQ(test, fp_scene_aux_core_seed(&map, &retained), 0);
	KUNIT_EXPECT_EQ(test, map, 0U);
	KUNIT_EXPECT_EQ(test, retained, 0U);
	fp_scene_aux_core_update(1, true, 0, NULL);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_ENABLE].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_ENABLE].writes, 0U);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MIN].value, (int)n);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MAX].value, (int)n);
	fp_scene_aux_core_update(0, false, 0, NULL);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_ENABLE].value, 1);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MIN].value, (int)(n > 4 ? 4 : n > 3 ? 2 : n > 2 ? 1 : 0));
	values[FP_AUX_KNOB_CORE_MAX].value = n;
	writes = values[FP_AUX_KNOB_CORE_ENABLE].writes;
	fp_scene_aux_core_update(1, false, 0, NULL);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, 0);
	KUNIT_EXPECT_EQ(test, aux.conflicts, 1U);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_ENABLE].writes, writes);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MAX].value, (int)n);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_EQ(test, fp_scene_aux_clear(), 0);
	mutex_unlock(&config_lock);
	memset(values, 0, sizeof(*values) * FP_SCENE_AUX_KNOBS);
	values[FP_AUX_KNOB_CORE_MIN].value = values[FP_AUX_KNOB_CORE_MAX].value = n;
	KUNIT_ASSERT_EQ(test, fp_test_install_aux_metadata(test, values, FP_AUX_UP_RATE, &metadata), 0);
	mutex_lock(&config_lock);
	fp_test_aux_apply("test.native.core", false, false, false, true);
	values[FP_AUX_KNOB_CORE_MAX].error = -EIO;
	fp_scene_aux_core_update(0, false, 0, NULL);
	fp_scene_aux_status(&aux);
	KUNIT_EXPECT_EQ(test, aux.error, -EIO);
	KUNIT_EXPECT_STREQ(test, aux.stage, "aux_core_write");
	KUNIT_EXPECT_EQ(test, aux.profile, -1);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_UP_RATE].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_ENABLE].value, 0);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MIN].value, (int)n);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_CORE_MAX].value, (int)n);
	KUNIT_EXPECT_TRUE(test, configured);
	values[FP_AUX_KNOB_CORE_MAX].error = 0;
	mutex_unlock(&config_lock);
}

static void frame_test_scene_input_inactive_and_epoch(struct kunit *test)
{
	struct fp_test_aux_value *aux_values;
	struct fp_scene_table *table;
	struct fp_scene_profile *active, *inactive;
	struct sab_status status;
	u64 previous_epoch;
	int error;

	error = cpufreq_register_driver(&fp_test_cpufreq_driver);
	KUNIT_ASSERT_EQ(test, error, 0);
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	KUNIT_ASSERT_NOT_NULL(test, fp_test_policy);
	fp_test_gpu = platform_device_register_simple("frame-kunit-gpu", -1, NULL, 0);
	if (IS_ERR(fp_test_gpu)) {
		fp_test_gpu = NULL;
		KUNIT_FAIL(test, "Cannot register activity GPU fixture");
		return;
	}
	fp_test_activity_input = input_allocate_device();
	KUNIT_ASSERT_NOT_NULL(test, fp_test_activity_input);
	fp_test_activity_input->name = "frame-policy-scene-touch";
	input_set_capability(fp_test_activity_input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(fp_test_activity_input, ABS_X, 0, 100, 0, 0);
	error = input_register_device(fp_test_activity_input);
	if (error) {
		input_free_device(fp_test_activity_input);
		fp_test_activity_input = NULL;
		KUNIT_FAIL(test, "Cannot register real kernel input fixture");
		return;
	}
	table = kvzalloc(sizeof(*table), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, table);
	scene_table = table;
	table->abi = FP_SCENE_ABI;
	table->cpu_count = 1;
	table->cpu_ids[0] = 0;
	table->profile_count = 2;
	table->defaults[FP_SCENE_ACTIVE] = 0;
	table->defaults[FP_SCENE_INACTIVE] = 1;
	table->defaults[FP_SCENE_OFF] = 1;
	active = &table->profiles[0]; inactive = &table->profiles[1];
	active->flags = inactive->flags = FP_SCENE_ENABLED;
	active->gpu = (struct fp_limits){330000, 1083000};
	inactive->gpu = (struct fp_limits){300000, 600000};
	active->cpus[0].limits = (struct fp_limits){750000, 2000000};
	inactive->cpus[0].limits = (struct fp_limits){500000, 1000000};
	KUNIT_ASSERT_TRUE(test, fp_scene_table_valid(table));
	configuration.cpu_count = 1;
	configuration.cpus[0].cpu = 0;
	strscpy(configuration.gpu_device, "frame-kunit-gpu", sizeof(configuration.gpu_device));
	configuration.flags = FP_CPU | FP_GPU;
	configured = true;
	scene_activity_backend_test_connect_hook(fp_test_connect_before_open);
	frame_policy_scene_full("frame.policy.kunit.scene", false, false, 100);
	flush_work(&control_work);
	scene_activity_backend_test_flush();
	scene_activity_backend_test_connect_hook(NULL);
	scene_activity_backend_status(&status);
	KUNIT_ASSERT_EQ(test, last_error, 0);
	KUNIT_ASSERT_TRUE(test, status.registered);
	KUNIT_ASSERT_TRUE(test, status.reliable);
	KUNIT_EXPECT_EQ(test, status.contacts, 1U);
	KUNIT_EXPECT_FALSE(test, status.deadline_valid);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 750000U);
	input_report_key(fp_test_activity_input, BTN_TOUCH, 1); input_sync(fp_test_activity_input);
	scene_activity_backend_test_flush();
	scene_activity_backend_status(&status);
	KUNIT_EXPECT_EQ(test, status.contacts, 1U);
	KUNIT_EXPECT_FALSE(test, status.deadline_valid);
	input_report_key(fp_test_activity_input, BTN_TOUCH, 0); input_sync(fp_test_activity_input);
	scene_activity_backend_test_flush();
	scene_activity_backend_status(&status);
	KUNIT_ASSERT_TRUE(test, status.deadline_valid);
	/* Deterministic clock/load fixture; the input->IRQ->worker and QoS are real. */
	KUNIT_ASSERT_EQ(test, scene_activity_backend_test_deadline(650), 0);
	fp_test_flush_activity_controls();
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, inactive);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 500000U);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1000000);
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 600000);
	/* Stop the backend worker: input must invalidate inactive synchronously,
	 * even while its usual input->worker path cannot run. Keep core QoS blocked
	 * too, proving the pending intent changes before the next core consumption.
	 */
	KUNIT_ASSERT_EQ(test, scene_activity_backend_test_pause_worker(), 0);
	mutex_lock(&config_lock);
	input_report_key(fp_test_activity_input, BTN_TOUCH, 1); input_sync(fp_test_activity_input);
	KUNIT_EXPECT_FALSE(test, READ_ONCE(pending_scene.inactive));
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, inactive);
	mutex_unlock(&config_lock);
	scene_activity_backend_test_resume_worker();
	fp_test_flush_activity_controls();
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, active);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 750000U);
	scene_activity_backend_status(&status); previous_epoch = status.epoch;
	/* A windowed/visible-only source change gets a new epoch despite same fg. */
	frame_policy_scene_full("frame.policy.kunit.scene", false, false, 101);
	flush_work(&control_work);
	scene_activity_changed(previous_epoch, true); fp_test_flush_activity_controls();
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, active);
	scene_activity_backend_status(&status);
	KUNIT_EXPECT_NE(test, status.epoch, previous_epoch);
	/* GPU-only Scene differences must still get real input/inactive events. */
	configuration.flags = FP_GPU;
	inactive->cpus[0] = active->cpus[0];
	frame_policy_scene_full("frame.policy.kunit.scene", false, false, 102);
	flush_work(&control_work);
	scene_activity_backend_status(&status);
	KUNIT_EXPECT_TRUE(test, status.registered);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	input_report_key(fp_test_activity_input, BTN_TOUCH, 0); input_sync(fp_test_activity_input);
	scene_activity_backend_test_flush();
	KUNIT_ASSERT_EQ(test, scene_activity_backend_test_deadline(650), 0);
	fp_test_flush_activity_controls();
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 600000);
	test_activity_present_at_game_start = true;
	frame_policy_scene_full("frame.policy.kunit.missing-game", false, true, 103);
	flush_work(&control_work);
	KUNIT_EXPECT_FALSE(test, test_activity_present_at_game_start);
	scene_activity_backend_status(&status);
	KUNIT_EXPECT_FALSE(test, status.registered);
	KUNIT_EXPECT_FALSE(test, scene_active);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	frame_policy_scene_full("frame.policy.kunit.scene", true, false, 104);
	flush_work(&control_work);
	scene_activity_backend_status(&status);
	KUNIT_EXPECT_FALSE(test, status.registered);
	KUNIT_EXPECT_FALSE(test, status.deadline_valid);
	/* Static policies consume zero input handles and zero deadlines. */
	table->defaults[FP_SCENE_INACTIVE] = 0;
	frame_policy_scene_full("frame.policy.kunit.scene", false, false, 105);
	flush_work(&control_work);
	scene_activity_backend_status(&status);
	KUNIT_EXPECT_FALSE(test, status.registered);
	KUNIT_EXPECT_FALSE(test, status.deadline_valid);
	KUNIT_EXPECT_TRUE(test, scene_active);
	/* Same frequency profile, different aux only: actual input IRQ/work must
	 * still choose inactive, while no static profile invents a PMU consumer.
	 */
	aux_values = kunit_kcalloc(test, FP_SCENE_AUX_KNOBS, sizeof(*aux_values), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, aux_values);
	aux_values[FP_AUX_KNOB_UP_RATE].value = 41;
	scene_table_epoch++;
	KUNIT_ASSERT_EQ(test, fp_test_install_aux(test, aux_values, FP_AUX_UP_RATE), 0);
	configuration.flags = FP_CPU | FP_GPU;
	frame_policy_scene_full("frame.policy.kunit.scene", false, false, 106);
	flush_work(&control_work);
	scene_activity_backend_status(&status);
	KUNIT_ASSERT_TRUE(test, status.registered);
	KUNIT_EXPECT_EQ(test, aux_values[FP_AUX_KNOB_UP_RATE].value, 1000);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	input_report_key(fp_test_activity_input, BTN_TOUCH, 1); input_sync(fp_test_activity_input);
	input_report_key(fp_test_activity_input, BTN_TOUCH, 0); input_sync(fp_test_activity_input);
	scene_activity_backend_test_flush();
	KUNIT_ASSERT_EQ(test, scene_activity_backend_test_deadline(650), 0);
	fp_test_flush_activity_controls();
	KUNIT_EXPECT_EQ(test, aux_values[FP_AUX_KNOB_UP_RATE].value, 2000);
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, active);
	KUNIT_EXPECT_EQ(test, cpu_floors[0], 750000U);
	input_report_key(fp_test_activity_input, BTN_TOUCH, 1); input_sync(fp_test_activity_input);
	fp_test_flush_activity_controls();
	KUNIT_EXPECT_EQ(test, aux_values[FP_AUX_KNOB_UP_RATE].value, 1000);
	configuration.flags = FP_GPU;
	frame_policy_scene_full("frame.policy.kunit.scene", false, false, 107);
	flush_work(&control_work);
	scene_activity_backend_status(&status);
	KUNIT_EXPECT_FALSE(test, status.registered);
	KUNIT_EXPECT_EQ(test, aux_values[FP_AUX_KNOB_UP_RATE].value, 41);
	KUNIT_EXPECT_NOT_NULL(test, gpu_device);
}

static u32 fp_test_scene_demand = 850000;

static bool fp_test_scene_sample(unsigned int cpu, u32 *khz)
{
	/* CPU0 would force a 3GHz cap if the production exclusion filter failed. */
	*khz = !cpu ? 4000000 : cpu == 1 ? fp_test_scene_demand : 0;
	return true;
}

static void fp_test_mt_contact(bool down, int x, int y)
{
	input_mt_slot(fp_test_activity_input, 0);
	input_mt_report_slot_state(fp_test_activity_input, MT_TOOL_FINGER, down);
	if (down) {
		input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, x);
		input_report_abs(fp_test_activity_input, ABS_MT_POSITION_Y, y);
	}
	input_report_key(fp_test_activity_input, BTN_TOUCH, down);
	input_sync(fp_test_activity_input);
}

static void fp_test_mt_replace_during_handoff(void)
{
	fp_test_mt_contact(false, 0, 0);
	fp_test_mt_contact(true, 10, 730);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 70);
	input_sync(fp_test_activity_input);
}

static void frame_test_scene_native_additive_and_physical_gesture(struct kunit *test)
{
	struct fp_scene_aux_cpu metadata = {
		.feedback_mask = FP_AUX_FEEDBACK_EXCLUDES | FP_AUX_FEEDBACK_PREFER |
			FP_AUX_FEEDBACK_PERFECT | FP_AUX_FEEDBACK_SMOOTHNESS |
			FP_AUX_FEEDBACK_MT | FP_AUX_FEEDBACK_ADDITIVE,
		.excludes_mask = 1, .prefer = 1, .additive_count = 3,
		.additive = {{0, 150000}, {1152000, 100000}, {1785600, 70000}},
	};
	struct fp_test_aux_value *values;
	struct fp_scene_table *table;
	struct fp_scene_event_snapshot events;
	struct fp_scene_event_config changed = {
		.gesture_enabled = true, .gesture_duration_ms = 700,
	};
	struct sab_status activity;
	u64 before, epoch, deadline;
	int error;

	KUNIT_ASSERT_EQ(test, cpufreq_register_driver(&fp_test_cpufreq_driver), 0);
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	KUNIT_ASSERT_NOT_NULL(test, fp_test_policy);
	KUNIT_ASSERT_GE(test, cpumask_weight(fp_test_policy->related_cpus), 2);
	table = kvzalloc(sizeof(*table), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, table);
	scene_table = table;
	table->abi = FP_SCENE_ABI; table->cpu_count = 1; table->profile_count = 1;
	table->profiles[0].flags = FP_SCENE_ENABLED;
	table->profiles[0].cpus[0].limits = (struct fp_limits){500000, 3000000};
	/* Legacy margins are deliberately empty. Only the native typed path can
	 * start PMUs and issue the tested dynamic QoS request.
	 */
	scene_table_epoch++;
	configuration.cpu_count = 1;
	configuration.cpus[0].cpu = 0;
	configuration.flags = FP_CPU;
	values = kunit_kcalloc(test, FP_SCENE_AUX_KNOBS, sizeof(*values), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, values);
	values[FP_AUX_KNOB_UP_RATE].value = 41;
	KUNIT_ASSERT_EQ(test, fp_test_install_aux_metadata(test, values, FP_AUX_UP_RATE, &metadata), 0);
	fp_test_activity_input = input_allocate_device();
	KUNIT_ASSERT_NOT_NULL(test, fp_test_activity_input);
	fp_test_activity_input->name = "frame-native-physical-mt";
	__set_bit(INPUT_PROP_DIRECT, fp_test_activity_input->propbit);
	input_set_capability(fp_test_activity_input, EV_KEY, BTN_TOUCH);
	input_set_abs_params(fp_test_activity_input, ABS_MT_POSITION_X, 0, 1000, 0, 0);
	input_set_abs_params(fp_test_activity_input, ABS_MT_POSITION_Y, 0, 1800, 0, 0);
	error = input_mt_init_slots(fp_test_activity_input, 2, INPUT_MT_DIRECT);
	KUNIT_ASSERT_EQ(test, error, 0);
	error = input_register_device(fp_test_activity_input);
	if (error) {
		input_free_device(fp_test_activity_input); fp_test_activity_input = NULL;
		KUNIT_FAIL(test, "Cannot register real MT event fixture");
		return;
	}
	test_scene_cpu_sample = fp_test_scene_sample;
	fp_test_scene_demand = 850000;
	configured = true;
	frame_policy_scene_full("frame.policy.kunit.native", false, false, 400);
	flush_work(&control_work);
	KUNIT_ASSERT_EQ(test, last_error, 0);
	KUNIT_ASSERT_NOT_NULL(test, pmus);
	scene_activity_backend_status(&activity);
	KUNIT_ASSERT_TRUE(test, activity.registered);
	fp_test_mt_contact(true, 500, 500); /* real input dispatch, non-edge origin */
	scene_activity_backend_test_flush();
	before = scene_native_samples;
	apply_scene(&scene_work);
	KUNIT_EXPECT_GT(test, scene_native_samples, before);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1000000);
	/* Native 1,002,000+100,000 ->1.5GHz OPP. The old percent path would
	 * produce 1,503,000 and snap to2GHz, so this is a negative control.
	 */
	fp_test_scene_demand = 1002000;
	apply_scene(&scene_work);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 1500000);
	fp_test_mt_contact(false, 0, 0);
	fp_test_mt_contact(true, 10, 400);
	/* A release/replacement in one report still looks like one contact. */
	input_mt_report_slot_state(fp_test_activity_input, MT_TOOL_FINGER, false);
	input_mt_report_slot_state(fp_test_activity_input, MT_TOOL_FINGER, true);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 540);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_Y, 480);
	input_sync(fp_test_activity_input);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	fp_test_mt_contact(false, 0, 0);
	fp_test_mt_contact(true, 10, 410);
	epoch = scene_activity_epoch;
	mutex_lock(&config_lock);
	frame_policy_scene_full("frame.policy.kunit.native.next", false, false, 401);
	frame_policy_scene_full("frame.policy.kunit.native.coalesced", false, false, 402);
	mutex_unlock(&config_lock);
	flush_work(&control_work);
	KUNIT_ASSERT_EQ(test, scene_activity_epoch, epoch + 2);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 69);
	input_sync(fp_test_activity_input);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 70);
	input_sync(fp_test_activity_input);
	scene_activity_backend_test_flush();
	apply_scene(&scene_work);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_TRUE(test, events.gesture_active);
	KUNIT_EXPECT_TRUE(test, events.force_core);
	KUNIT_EXPECT_FALSE(test, events.notify_active);
	KUNIT_EXPECT_GT(test, events.next_deadline_ns, ktime_get_ns());
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&scene_feedback_deadline));
	deadline = events.next_deadline_ns;
	epoch = scene_activity_epoch;
	mutex_lock(&config_lock);
	frame_policy_scene_full("frame.policy.kunit.native.last", false, false, 403);
	/* Reproduce the end of a control round overtaken by a normal foreground. */
	reconcile_scene_activity();
	scene_activity_backend_feedback(epoch, ktime_get_ns(), &events);
	mutex_unlock(&config_lock);
	KUNIT_EXPECT_TRUE(test, events.gesture_active);
	KUNIT_EXPECT_EQ(test, events.next_deadline_ns, deadline);
	flush_work(&control_work);
	scene_activity_backend_test_flush();
	irq_work_sync(&scene_irq_work);
	flush_work(&scene_work);
	KUNIT_ASSERT_EQ(test, scene_activity_epoch, epoch + 1);
	scene_activity_backend_feedback(epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_ASSERT_TRUE(test, events.gesture_active);
	KUNIT_EXPECT_EQ(test, events.next_deadline_ns, deadline);
	KUNIT_EXPECT_EQ(test, scene_feedback_at, deadline);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&scene_feedback_deadline));
	/* A different slot moving and disappearing inside one SYN must not
	 * queue a successor while the original slot remains the sole contact.
	 */
	input_mt_slot(fp_test_activity_input, 1);
	input_mt_report_slot_state(fp_test_activity_input, MT_TOOL_FINGER, true);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 300);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_Y, 600);
	input_mt_report_slot_state(fp_test_activity_input, MT_TOOL_FINGER, false);
	input_sync(fp_test_activity_input);
	fp_test_mt_contact(false, 0, 0);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_TRUE(test, events.gesture_active); /* release does not revoke the pulse */
	epoch = scene_activity_epoch;
	scene_activity_backend_test_flush();
	scene_activity_changed(epoch, true);
	irq_work_sync(&activity_control_irq);
	flush_work(&control_work);
	irq_work_sync(&scene_irq_work);
	flush_work(&scene_work);
	KUNIT_EXPECT_TRUE(test, scene_inactive);
	KUNIT_EXPECT_EQ(test, scene_activity_epoch, epoch);
	KUNIT_EXPECT_EQ(test, scene_feedback_at, deadline);
	KUNIT_EXPECT_TRUE(test, delayed_work_pending(&scene_feedback_deadline));
	scene_activity_backend_feedback(scene_activity_epoch, deadline, &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	/* A different finger begins and crosses the threshold while the
	 * activity worker is being reconfigured for a normal foreground.
	 */
	fp_test_mt_contact(true, 540, 500);
	scene_activity_backend_test_configure_hook(fp_test_mt_replace_during_handoff);
	frame_policy_scene_full("frame.policy.kunit.native.paused", false, false, 404);
	flush_work(&control_work);
	scene_activity_backend_test_configure_hook(NULL);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_ASSERT_TRUE(test, events.gesture_active);
	fp_test_mt_contact(false, 0, 0);
	scene_activity_backend_feedback(scene_activity_epoch, events.next_deadline_ns, &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	fp_test_mt_contact(true, 10, 510);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 70);
	input_sync(fp_test_activity_input);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_ASSERT_TRUE(test, events.gesture_active);
	scene_activity_backend_set_feedback(scene_activity_epoch, &changed, scene_feedback_changed);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 150);
	input_sync(fp_test_activity_input);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active); /* held input cannot revive the old anchor */
	fp_test_mt_contact(false, 0, 0);
	fp_test_mt_contact(true, 10, 700);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 70);
	input_sync(fp_test_activity_input);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_ASSERT_TRUE(test, events.gesture_active);
	mutex_lock(&config_lock);
	frame_policy_scene_full("frame.policy.kunit.native.off", true, false, 405);
	frame_policy_scene_full("frame.policy.kunit.native.resume", false, false, 406);
	mutex_unlock(&config_lock);
	flush_work(&control_work);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 150);
	input_sync(fp_test_activity_input);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	fp_test_mt_contact(false, 0, 0);
	fp_test_mt_contact(true, 10, 710);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 70);
	input_sync(fp_test_activity_input);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_ASSERT_TRUE(test, events.gesture_active);
	mutex_lock(&config_lock);
	frame_policy_scene_full("frame.policy.kunit.native-game", false, true, 407);
	frame_policy_scene_full("frame.policy.kunit.native.resume", false, false, 408);
	mutex_unlock(&config_lock);
	flush_work(&control_work);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	fp_test_mt_contact(false, 0, 0);
	fp_test_mt_contact(true, 10, 720);
	input_report_abs(fp_test_activity_input, ABS_MT_POSITION_X, 70);
	input_sync(fp_test_activity_input);
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_ASSERT_TRUE(test, events.gesture_active);
	frame_policy_scene_full("frame.policy.kunit.native-game", true, true, 409);
	flush_work(&control_work);
	KUNIT_EXPECT_TRUE(test, scene_active); /* game screen-off is Scene off, never FAS */
	KUNIT_EXPECT_FALSE(test, game_active);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&scene_feedback_deadline));
	scene_activity_backend_feedback(scene_activity_epoch, ktime_get_ns(), &events);
	KUNIT_EXPECT_FALSE(test, events.gesture_active);
	frame_policy_scene_full("frame.policy.kunit.native-game", false, true, 410);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, last_error, 0);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_FALSE(test, delayed_work_pending(&scene_feedback_deadline));
	scene_activity_backend_status(&activity);
	KUNIT_EXPECT_FALSE(test, activity.registered);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_UP_RATE].value, 41);
	configuration.flags = 0;
	frame_policy_scene_full("frame.policy.kunit.native", false, false, 411);
	flush_work(&control_work);
	scene_activity_backend_status(&activity);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
	KUNIT_EXPECT_FALSE(test, activity.registered);
	KUNIT_EXPECT_EQ(test, values[FP_AUX_KNOB_UP_RATE].value, 41);
}

static int fp_test_gpu_aux_result;
static bool fp_test_gpu_aux_creds_restored;

static void fp_test_gpu_aux_worker(struct work_struct *work)
{
	const struct cred *before = current_cred();

	mutex_lock(&config_lock);
	fp_test_gpu_aux_result = fp_scene_gpu_aux_apply(gpu_aux_allowed(), generation,
		gpu_aux_allowed() ? &generation : NULL);
	mutex_unlock(&config_lock);
	fp_test_gpu_aux_creds_restored = current_cred() == before;
}
static DECLARE_WORK(fp_test_gpu_aux_work, fp_test_gpu_aux_worker);

static void fp_test_run_gpu_aux(void)
{
	queue_work(policy_wq, &fp_test_gpu_aux_work);
	flush_work(&fp_test_gpu_aux_work);
}

static void frame_test_scene_gpu_aux_credentials_and_isolation(struct kunit *test)
{
	struct fp_test_aux_value *values;
	struct fp_scene_gpu_aux *candidate = NULL;
	struct fp_scene_gpu_aux_status status;
	const struct cred *saved, *before = current_cred();
	struct cred *credentials;
	u32 i;
	int error;

	KUNIT_ASSERT_EQ(test, cpufreq_register_driver(&fp_test_cpufreq_driver), 0);
	fp_test_driver_registered = true;
	fp_test_policy = cpufreq_cpu_get(0);
	KUNIT_ASSERT_NOT_NULL(test, fp_test_policy);
	fp_test_gpu = platform_device_register_simple("frame-kunit-gpu", -1, NULL, 0);
	if (IS_ERR(fp_test_gpu)) {
		fp_test_gpu = NULL; KUNIT_FAIL(test, "Cannot register auxiliary GPU QoS fixture"); return;
	}
	configuration.cpu_count = 1; configuration.cpus[0].cpu = 0;
	configuration.cpus[0].game = (struct fp_limits){500000, 3000000};
	configuration.gpu_game = (struct fp_limits){330000, 1083000};
	configuration.flags = FP_CPU | FP_GPU;
	strscpy(configuration.gpu_device, "frame-kunit-gpu", sizeof(configuration.gpu_device));
	KUNIT_ASSERT_EQ(test, add_requests(true), 0);
	/* The outgoing frequency votes are actual QoS requests. Qualification
	 * through Android libgui is separate, already covered by game fixtures.
	 */
	configured = source_valid = source_game = game_active = true;
	display_off = false;
	values = kunit_kcalloc(test, 3, sizeof(*values), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, values);
	values[0].value = 12; values[1].value = 5; values[2].value = 7;
	credentials = prepare_creds();
	KUNIT_ASSERT_NOT_NULL(test, credentials);
	saved = override_creds(credentials);
	for (i = 0; i < 3; i++) {
		values[i].check_credentials = true;
		fp_test_gpu_aux_files[i] = anon_inode_getfile("scene-gpu-prepared-cred",
			&fp_test_aux_ops, &values[i], O_RDWR);
		if (IS_ERR(fp_test_gpu_aux_files[i])) {
			fp_test_gpu_aux_files[i] = NULL;
			break;
		}
	}
	revert_creds(saved);
	put_cred(credentials);
	KUNIT_ASSERT_EQ(test, i, 3U);
	KUNIT_EXPECT_PTR_NE(test, current_cred(), fp_test_gpu_aux_files[1]->f_cred);
	error = fp_scene_gpu_aux_test_prepare(fp_test_gpu_aux_files[0], fp_test_gpu_aux_files[1],
		fp_test_gpu_aux_files[2], &candidate);
	KUNIT_ASSERT_EQ(test, error, 0);
	KUNIT_EXPECT_PTR_EQ(test, current_cred(), before);
	mutex_lock(&config_lock);
	KUNIT_EXPECT_EQ(test, fp_scene_gpu_aux_release(), 0);
	fp_scene_gpu_aux_commit(candidate);
	mutex_unlock(&config_lock);
	fp_test_run_gpu_aux();
	KUNIT_EXPECT_EQ(test, fp_test_gpu_aux_result, 0);
	KUNIT_EXPECT_TRUE(test, fp_test_gpu_aux_creds_restored);
	KUNIT_EXPECT_EQ(test, values[1].value, 11);
	KUNIT_EXPECT_EQ(test, values[2].value, 11);
	values[1].value = 4; /* A separate owner changes only the minimum. */
	fp_test_run_gpu_aux();
	fp_scene_gpu_aux_status(&status);
	KUNIT_EXPECT_EQ(test, status.conflict_mask, 1U);
	KUNIT_EXPECT_EQ(test, values[1].value, 4);
	KUNIT_EXPECT_EQ(test, values[2].value, 11);
	display_off = true;
	fp_test_run_gpu_aux();
	KUNIT_EXPECT_EQ(test, values[1].value, 4);
	KUNIT_EXPECT_EQ(test, values[2].value, 7);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_TRUE(test, game_active);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 3000000);
	KUNIT_EXPECT_EQ(test, dev_pm_qos_read_value(&fp_test_gpu->dev, DEV_PM_QOS_MAX_FREQUENCY), 1083000);
	mutex_lock(&config_lock);
	KUNIT_EXPECT_EQ(test, fp_scene_gpu_aux_release(), 0);
	fp_scene_gpu_aux_commit(NULL);
	mutex_unlock(&config_lock);
	values[1].value = 5; values[2].value = 7;
	KUNIT_ASSERT_EQ(test, fp_scene_gpu_aux_test_prepare(fp_test_gpu_aux_files[0],
		fp_test_gpu_aux_files[1], fp_test_gpu_aux_files[2], &candidate), 0);
	mutex_lock(&config_lock);
	fp_scene_gpu_aux_commit(candidate);
	mutex_unlock(&config_lock);
	values[2].error = -EIO;
	display_off = false;
	fp_test_run_gpu_aux();
	fp_scene_gpu_aux_status(&status);
	KUNIT_EXPECT_EQ(test, fp_test_gpu_aux_result, -EIO);
	KUNIT_EXPECT_EQ(test, status.error, -EIO);
	KUNIT_EXPECT_EQ(test, status.rollback_error, 0);
	KUNIT_EXPECT_EQ(test, values[1].value, 5); /* Roll back only this acquired minimum. */
	KUNIT_EXPECT_EQ(test, values[2].value, 7);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_TRUE(test, game_active);
	KUNIT_EXPECT_TRUE(test, fp_test_gpu_aux_creds_restored);
	KUNIT_EXPECT_EQ(test, freq_qos_read_value(&fp_test_policy->constraints, FREQ_QOS_MAX), 3000000);
	values[2].error = 0;
	fp_test_run_gpu_aux(); /* A new explicit control action can retry. */
	KUNIT_EXPECT_EQ(test, values[1].value, 11);
	configuration.flags |= FP_OBSERVE;
	fp_test_run_gpu_aux();
	KUNIT_EXPECT_EQ(test, values[1].value, 5);
	KUNIT_EXPECT_EQ(test, values[2].value, 7);
	KUNIT_EXPECT_TRUE(test, fp_test_gpu_aux_creds_restored);
	configuration.flags &= ~FP_OBSERVE;
	fp_test_run_gpu_aux();
	KUNIT_EXPECT_EQ(test, values[1].value, 11);
	scene_table = kvzalloc(sizeof(*scene_table), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, scene_table);
	scene_profile = NULL;
	source_game = false; /* Explicit ignored Scene has no frequency profile. */
	fp_test_run_gpu_aux();
	KUNIT_EXPECT_EQ(test, values[1].value, 5);
	KUNIT_EXPECT_EQ(test, values[2].value, 7);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_TRUE(test, fp_test_gpu_aux_creds_restored);
}

static void frame_test_scene_status_preserves_error_tail(struct kunit *test)
{
	u64 *counters[] = { &generation, &applied_generation, &frames_seen, &frames_used,
		&frame_errors, &gpu_samples, &gpu_unknown, &fas_overflows, &fas_steps,
		&fas_late_steps, &fas_task_samples, &fas_task_sample_errors, &fas_task_rollbacks,
		&switches, &scene_samples, &scene_inactive_events };
	u32 *limits[] = { &cpu_floors[0], &cpu_floors[1], &gpu_floor,
		&cpu_ceilings[0], &cpu_ceilings[1], &cpu_loads[0], &cpu_loads[1], &active_fps };
	u32 saved_limits[ARRAY_SIZE(limits)];
	struct sab_status activity = {
		.epoch = U64_MAX, .inputs = U64_MAX, .transitions = U64_MAX,
		.load_samples = U64_MAX, .load_errors = U64_MAX, .deadline_ms = U64_MAX,
		.devices = U32_MAX, .contacts = U32_MAX, .load_permille = U32_MAX,
		.phase = U32_MAX, .registered = true, .reliable = true,
		.deadline_valid = true, .error = -EIO,
	};
	u64 *saved;
	char *text, *old_foreground;
	const char *old_stage = last_error_stage;
	int old_error = last_error, length;
	unsigned int i;

	text = kunit_kzalloc(test, FP_STATUS_SIZE, GFP_KERNEL);
	saved = kunit_kcalloc(test, ARRAY_SIZE(counters), sizeof(*saved), GFP_KERNEL);
	old_foreground = kunit_kzalloc(test, sizeof(foreground), GFP_KERNEL);
	KUNIT_ASSERT_NOT_NULL(test, text);
	KUNIT_ASSERT_NOT_NULL(test, saved);
	KUNIT_ASSERT_NOT_NULL(test, old_foreground);
	memcpy(old_foreground, foreground, sizeof(foreground));
	memset(foreground, 'x', sizeof(foreground) - 1);
	foreground[sizeof(foreground) - 1] = 0;
	for (i = 0; i < ARRAY_SIZE(counters); i++) {
		saved[i] = *counters[i]; *counters[i] = U64_MAX;
	}
	last_error = -EIO;
	last_error_stage = "scene_activity_status_tail_fixture";
	for (i = 0; i < ARRAY_SIZE(limits); i++) {
		saved_limits[i] = *limits[i]; *limits[i] = U32_MAX;
	}
	length = format_policy_status(text, FP_STATUS_SIZE, &activity);
	for (i = 0; i < ARRAY_SIZE(limits); i++) *limits[i] = saved_limits[i];
	for (i = 0; i < ARRAY_SIZE(counters); i++) *counters[i] = saved[i];
	memcpy(foreground, old_foreground, sizeof(foreground));
	last_error = old_error; last_error_stage = old_stage;
	KUNIT_EXPECT_GT(test, length, 2048);
	KUNIT_EXPECT_LT(test, length, FP_STATUS_SIZE - 1);
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "scene_activity_error="));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "aux_abi=1\naux_table_bytes=26584\naux_feedback_supported=1\n"));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text, "gpu_aux_rollback_error="));
	KUNIT_EXPECT_NOT_NULL(test, strstr(text,
		"last_error=-5\nlast_error_stage=scene_activity_status_tail_fixture\n"));
}

/* Add these cases to the existing frame_policy suite's case array. */
#define FP_SCENE_KUNIT_CASES \
	KUNIT_CASE(frame_test_scene_selection_and_game_exclusion), \
	KUNIT_CASE(frame_test_scene_validation_boundaries), \
	KUNIT_CASE(frame_test_scene_static_and_dynamic_feedback), \
	KUNIT_CASE(frame_test_scene_table_topology), \
	KUNIT_CASE(frame_test_scene_aux_leases_and_rollback), \
	KUNIT_CASE(frame_test_scene_control_ownership), \
	KUNIT_CASE(frame_test_scene_input_inactive_and_epoch), \
	KUNIT_CASE(frame_test_scene_native_additive_and_physical_gesture), \
	KUNIT_CASE(frame_test_scene_gpu_aux_credentials_and_isolation), \
	KUNIT_CASE(frame_test_scene_status_preserves_error_tail)
