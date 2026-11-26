/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SCENE_ACTIVITY_BACKEND_H
#define SCENE_ACTIVITY_BACKEND_H

#include <linux/kconfig.h>
#include "scene_activity_policy.h"

struct sab_status {
	sa_u64 epoch, inputs, transitions, load_samples, load_errors, deadline_ms;
	sa_u32 devices, contacts, load_permille, phase;
	bool registered, reliable, deadline_valid;
	int error;
};

/*
 * configure/exit are sleepable and MUST NOT be called from the backend notifier.
 * The notifier runs under activity_lock, sometimes under input_dev.event_lock.
 * It may take frame_lock and queue irq_work only: no config_lock or WQ pool lock.
 * The owner serializes configuration and supplies a semantic (not work) epoch.
 */
int scene_activity_backend_init(void (*notify)(sa_u64 epoch, bool inactive));
int scene_activity_backend_configure(const struct sa_context *context,
		unsigned int first_policy_cpu);
void scene_activity_backend_status(struct sab_status *status);
void scene_activity_backend_exit(void);

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
struct input_dev;
void scene_activity_backend_test_connect_hook(void (*hook)(struct input_dev *dev));
int scene_activity_backend_test_pause_worker(void);
void scene_activity_backend_test_resume_worker(void);
void scene_activity_backend_test_flush(void);
int scene_activity_backend_test_deadline(sa_u32 known_load_permille);
#endif

#endif
