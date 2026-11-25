/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ACE6_SCHED_AFFINITY_H
#define ACE6_SCHED_AFFINITY_H

struct task_struct;
struct cpumask;

int ace6_set_task_affinity(struct task_struct *task, const struct cpumask *mask,
			   const struct cpumask *requested);

#endif
