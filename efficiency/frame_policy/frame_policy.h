/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_POLICY_H
#define FRAME_POLICY_H

#include <linux/kconfig.h>
#include <linux/sched.h>

#if IS_ENABLED(CONFIG_FRAME_POLICY)
void frame_policy_scene(const char *package, bool display_off, bool game);
void frame_policy_scene_full(const char *package, bool display_off, bool game,
			    u64 source_epoch);
void frame_policy_task_event(struct task_struct *task);
#else
static inline void frame_policy_scene(const char *package, bool display_off, bool game) {}
static inline void frame_policy_scene_full(const char *package, bool display_off, bool game,
					 u64 source_epoch) {}
static inline void frame_policy_task_event(struct task_struct *task) {}
#endif

#endif
