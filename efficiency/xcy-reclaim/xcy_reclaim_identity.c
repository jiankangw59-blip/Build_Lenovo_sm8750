#include <linux/cred.h>
#include <linux/jiffies.h>
#include <linux/pid.h>
#include <linux/pid_namespace.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>

#include "xcy_reclaim_identity.h"

bool xcy_reclaim_identity_current(struct pid *pid, const xcy_memory_process_t *record)
{
	struct task_struct *task = get_pid_task(pid, PIDTYPE_TGID);
	bool valid;

	if (!task)
		return false;
	/* Framework reports Linux IDs and /proc stat ticks in the initial
	 * namespaces. The PID object continues to identify the same generation. */
	valid = pid_nr(pid) == record->pid && record->starttime &&
		!(READ_ONCE(task->flags) & (PF_KTHREAD | PF_EXITING)) &&
		!is_global_init(task) &&
		__kuid_val(task_uid(task)) == xcy_memory_kernel_uid(record) &&
		nsec_to_clock_t(task->start_boottime) == record->starttime;
	put_task_struct(task);
	return valid;
}

struct pid *xcy_reclaim_pin_identity(const xcy_memory_process_t *record)
{
	struct pid *pid;

	if (!(record->flags & XCY_MEMORY_IDENTITY_VALID) || record->pid <= 1 ||
	    record->pid > INT_MAX || !xcy_memory_uid_mapping_valid(record))
		return NULL;
	rcu_read_lock();
	pid = get_pid(find_pid_ns(record->pid, &init_pid_ns));
	rcu_read_unlock();
	if (pid && xcy_reclaim_identity_current(pid, record))
		return pid;
	put_pid(pid);
	return NULL;
}

int xcy_reclaim_validate_identity(const xcy_memory_process_t *record, void *unused)
{
	struct pid *pid = xcy_reclaim_pin_identity(record);
	int valid = pid != NULL;

	put_pid(pid);
	return valid;
}
