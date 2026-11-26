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
	configuration.flags = FP_CPU | FP_GPU;
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

	frame_policy_scene("frame.policy.kunit.ignored", false, false);
	flush_work(&control_work);
	KUNIT_EXPECT_EQ(test, last_error, 0);
	KUNIT_EXPECT_TRUE(test, configured);
	KUNIT_EXPECT_FALSE(test, scene_active);
	KUNIT_EXPECT_PTR_EQ(test, scene_profile, NULL);
	KUNIT_EXPECT_PTR_EQ(test, cpu_requests[0].policy, NULL);
	KUNIT_EXPECT_PTR_EQ(test, gpu_device, NULL);
	KUNIT_EXPECT_PTR_EQ(test, pmus, NULL);
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
}

/* Add these five entries to the existing frame_policy suite's case array. */
#define FP_SCENE_KUNIT_CASES \
	KUNIT_CASE(frame_test_scene_selection_and_game_exclusion), \
	KUNIT_CASE(frame_test_scene_validation_boundaries), \
	KUNIT_CASE(frame_test_scene_static_and_dynamic_feedback), \
	KUNIT_CASE(frame_test_scene_table_topology), \
	KUNIT_CASE(frame_test_scene_control_ownership)
