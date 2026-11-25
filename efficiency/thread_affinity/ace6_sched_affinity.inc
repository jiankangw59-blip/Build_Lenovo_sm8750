/* SPDX-License-Identifier: GPL-2.0 */
#include "ace6_sched_affinity.h"

/* The policy owns a task reference. Keep the scheduler's cpuset and vendor checks. */
int ace6_set_task_affinity(struct task_struct *task, const struct cpumask *mask,
			   const struct cpumask *requested)
{
	struct cpumask *user_mask = NULL;
	struct affinity_context ctx;
	bool skip = false;
	int ret;

	if (READ_ONCE(task->flags) & (PF_EXITING | PF_NO_SETAFFINITY))
		return -ESRCH;
	trace_android_vh_sched_setaffinity_early(task, mask, &skip);
	if (skip)
		return -EPERM;
	ret = security_task_setscheduler(task);
	if (ret)
		return ret;
	if (requested) {
		user_mask = alloc_user_cpus_ptr(NUMA_NO_NODE);
		if (!user_mask)
			return -ENOMEM;
		cpumask_copy(user_mask, requested);
	}

	ctx = (struct affinity_context) {
		.new_mask = mask,
		.user_mask = user_mask,
		.flags = SCA_USER,
	};
	ret = __sched_setaffinity(task, &ctx);
	trace_android_rvh_sched_setaffinity(task, mask, &ret);
	kfree(ctx.user_mask);
	return ret;
}
