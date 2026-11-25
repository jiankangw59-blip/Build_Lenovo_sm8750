// SPDX-License-Identifier: GPL-2.0
/* Thread affinity policy. Fork/rename event model from kstraccc/cpuset_hook.c. */
#include <linux/capability.h>
#include <linux/binfmts.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/task.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "affinity_rules.h"

#define MAX_PENDING 4096
#define MAX_SNAPSHOT 16384

struct affinity_state {
	cpumask_t effective;
	cpumask_t requested;
	bool has_request;
};

struct thread_lease {
	struct hlist_node node;
	struct task_struct *task;
	struct affinity_state original;
	struct affinity_state applied;
};

struct task_event {
	struct work_struct work;
	struct task_struct *task;
	struct task_struct *parent;
	bool exit;
};

/* All policy swaps and affinity operations share this lock. Tracepoints only queue work. */
static DEFINE_MUTEX(policy_lock);
static DEFINE_HASHTABLE(leases, 10);
static struct affinity_policy *policy;
static struct workqueue_struct *affinity_wq;
static bool enabled;
static unsigned int revision;
static unsigned int validated;
static unsigned int lease_count;
static atomic_t pending;
static atomic_long_t dropped, changed, clamped, failed;

static struct thread_lease *find_lease(struct task_struct *task)
{
	struct thread_lease *lease;

	hash_for_each_possible(leases, lease, node, (unsigned long)task) {
		if (lease->task == task)
			return lease;
	}
	return NULL;
}

static void free_lease(struct thread_lease *lease)
{
	hash_del(&lease->node);
	put_task_struct(lease->task);
	kfree(lease);
	lease_count--;
}

static bool task_eligible(struct task_struct *task)
{
	return !(READ_ONCE(task->flags) & (PF_KTHREAD | PF_EXITING | PF_NO_SETAFFINITY)) &&
		task_tgid_nr(task) > 1 && READ_ONCE(task->policy) != SCHED_DEADLINE;
}

static void read_affinity(struct task_struct *task, struct affinity_state *state)
{
	unsigned long flags;

	raw_spin_lock_irqsave(&task->pi_lock, flags);
	cpumask_copy(&state->effective, &task->cpus_mask);
	state->has_request = task->user_cpus_ptr != NULL;
	if (state->has_request)
		cpumask_copy(&state->requested, task->user_cpus_ptr);
	else
		cpumask_clear(&state->requested);
	raw_spin_unlock_irqrestore(&task->pi_lock, flags);
}

static bool same_request(const struct affinity_state *a, const struct affinity_state *b)
{
	return a->has_request == b->has_request &&
		cpumask_equal(&a->requested, &b->requested);
}

static bool same_affinity(const struct affinity_state *a, const struct affinity_state *b)
{
	return same_request(a, b) && cpumask_equal(&a->effective, &b->effective);
}

static bool process_name(struct task_struct *task, char name[PROCESS_NAME_LEN])
{
	struct mm_struct *mm = get_task_mm(task);
	unsigned long start;
	char *base;
	int size;

	if (!mm)
		return false;
	spin_lock(&mm->arg_lock);
	start = mm->arg_start;
	spin_unlock(&mm->arg_lock);
	mmput(mm);
	if (!start)
		return false;
	size = access_process_vm(task, start, name, PROCESS_NAME_LEN - 1, FOLL_FORCE);
	if (size <= 0)
		return false;
	name[size] = '\0';
	base = strrchr(name, '/');
	if (base)
		memmove(name, base + 1, strlen(base + 1) + 1);
	return *name && !strstr(name, "zygote");
}

#ifndef MODULE
#include "ace6_sched_affinity.h"
#endif

/* Leave the task in its current Android cpuset. */
static int apply_mask(struct task_struct *task, const struct cpumask *target,
		      const struct cpumask *user_request, struct affinity_state *applied)
{
#ifdef MODULE
	cpumask_t allowed;
	cpumask_t effective;
	int ret;

	cpuset_cpus_allowed(task, &allowed);
	if (!cpumask_and(&effective, target, &allowed)) {
		atomic_long_inc(&clamped);
		return -EINVAL;
	}
	ret = set_cpus_allowed_ptr(task, &effective);
	if (ret)
		return ret;

	/* A cpuset attach can overlap the write. Its event will also recheck policy. */
	read_affinity(task, applied);
	cpuset_cpus_allowed(task, &allowed);
	if (!cpumask_subset(&applied->effective, &allowed)) {
		if (!cpumask_and(&effective, target, &allowed))
			cpumask_copy(&effective, &allowed);
		ret = set_cpus_allowed_ptr(task, &effective);
		read_affinity(task, applied);
	}
	if (!cpumask_equal(target, &applied->effective))
		atomic_long_inc(&clamped);
	return ret;
#else
	int ret = ace6_set_task_affinity(task, target, user_request);

	if (!ret) {
		read_affinity(task, applied);
		if (!cpumask_equal(target, &applied->effective))
			atomic_long_inc(&clamped);
	}
	return ret;
#endif
}

static int restore_mask(struct task_struct *task, const struct affinity_state *original)
{
	struct affinity_state applied;
	const struct cpumask *request = original->has_request ? &original->requested : NULL;
	const struct cpumask *mask = request ? request : &original->effective;

	return apply_mask(task, mask, request, &applied);
}

static bool owns_affinity(const struct affinity_state *applied,
			  const struct affinity_state *observed)
{
#ifdef MODULE
	return same_affinity(applied, observed);
#else
	/* The effective mask can change when a cpuset moves a leased thread. */
	return same_request(applied, observed);
#endif
}

static void restore_lease(struct thread_lease *lease)
{
	if (task_eligible(lease->task)) {
		struct affinity_state observed;

		read_affinity(lease->task, &observed);
		if (owns_affinity(&lease->applied, &observed) &&
		    restore_mask(lease->task, &lease->original))
			atomic_long_inc(&failed);
	}
	free_lease(lease);
}

static void apply_task(struct task_struct *task, struct task_struct *parent)
{
	struct thread_lease *lease = find_lease(task);
	const struct affinity_rule *rule = NULL;
	char process[PROCESS_NAME_LEN], thread[TASK_COMM_LEN];
	struct affinity_state observed, original;
	cpumask_t effective, allowed;

	if (!task_eligible(task)) {
		if (lease && (READ_ONCE(task->flags) & PF_EXITING))
			free_lease(lease);
		return;
	}
	if (policy && policy->count && process_name(task, process)) {
		get_task_comm(thread, task);
		rule = select_rule(policy, process, strim(thread));
	}
	read_affinity(task, &observed);
	original = observed;
	if (!lease && parent) {
		struct thread_lease *origin = find_lease(parent);

		if (origin && owns_affinity(&origin->applied, &observed))
			original = origin->original;
	}
	if (!rule) {
		if (lease)
			restore_lease(lease);
		else if (!same_affinity(&original, &observed) && restore_mask(task, &original))
			atomic_long_inc(&failed);
		return;
	}
	if (lease && !owns_affinity(&lease->applied, &observed)) {
		/* Android or the application changed the mask since our last write. */
		free_lease(lease);
		lease = NULL;
	}
	cpuset_cpus_allowed(task, &allowed);
	if (!cpumask_and(&effective, &allowed, &rule->mask)) {
		atomic_long_inc(&clamped);
		if (lease)
			restore_lease(lease);
		return;
	}
	if (!lease) {
		if (cpumask_equal(&observed.effective, &effective) &&
		    same_affinity(&observed, &original))
			return;
		lease = kzalloc(sizeof(*lease), GFP_KERNEL);
		if (!lease) {
			atomic_long_inc(&failed);
			return;
		}
		get_task_struct(task);
		lease->task = task;
		lease->original = original;
		lease->applied = observed;
		hash_add(leases, &lease->node, (unsigned long)task);
		lease_count++;
	}
	if (!cpumask_equal(&observed.effective, &effective)) {
		if (apply_mask(task, &rule->mask, &rule->mask, &lease->applied)) {
			atomic_long_inc(&failed);
			return;
		}
		atomic_long_inc(&changed);
	}
}

static void run_event(struct work_struct *work)
{
	struct task_event *event = container_of(work, struct task_event, work);
	struct thread_lease *lease;

	mutex_lock(&policy_lock);
	if (event->exit) {
		lease = find_lease(event->task);
		if (lease)
			free_lease(lease);
	} else if (enabled) {
		apply_task(event->task, event->parent);
	}
	mutex_unlock(&policy_lock);
	if (event->parent)
		put_task_struct(event->parent);
	put_task_struct(event->task);
	kfree(event);
	atomic_dec(&pending);
}

static void queue_event(struct task_struct *task, struct task_struct *parent,
			bool exit)
{
	struct task_event *event;

	if (!READ_ONCE(enabled) || (READ_ONCE(task->flags) & PF_KTHREAD))
		return;
	if (atomic_inc_return(&pending) > MAX_PENDING)
		goto drop;
	event = kmalloc(sizeof(*event), GFP_ATOMIC);
	if (!event)
		goto drop;
	get_task_struct(task);
	if (parent)
		get_task_struct(parent);
	event->task = task;
	event->parent = parent;
	event->exit = exit;
	INIT_WORK(&event->work, run_event);
	queue_work(affinity_wq, &event->work);
	return;
drop:
	atomic_dec(&pending);
	atomic_long_inc(&dropped);
}

static void on_fork(void *data, struct task_struct *parent, struct task_struct *task)
{
	queue_event(task, parent, false);
}

static void on_rename(void *data, struct task_struct *task, const char *name)
{
	queue_event(task, NULL, false);
}

static void on_exec(void *data, struct task_struct *task, pid_t old_pid,
		    struct linux_binprm *bprm)
{
	queue_event(task, NULL, false);
}

static void on_exit(void *data, struct task_struct *task)
{
	queue_event(task, NULL, true);
}

static void on_cpuset(void *data, struct cgroup_subsys_state *css,
		      struct task_struct *task)
{
	queue_event(task, NULL, false);
}

static struct {
	const char *name;
	void *probe;
	struct tracepoint *tp;
} hooks[] = {
	{ "sched_process_fork", on_fork },
	{ "task_rename", on_rename },
	{ "sched_process_exec", on_exec },
	{ "sched_process_exit", on_exit },
	{ "android_vh_cpuset_attach_task", on_cpuset },
};

static void find_hook(struct tracepoint *tp, void *data)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		if (!strcmp(tp->name, hooks[i].name))
			hooks[i].tp = tp;
	}
}

static void release_policy(void)
{
	struct thread_lease *lease;
	struct hlist_node *tmp;
	unsigned int bucket;

	WRITE_ONCE(enabled, false);
	hash_for_each_safe(leases, bucket, tmp, lease, node)
		restore_lease(lease);
	kvfree(policy);
	policy = NULL;
}

static int replace_policy(struct affinity_policy *next)
{
	struct task_struct *group, *task;
	struct task_struct **tasks = NULL;
	struct thread_lease *lease;
	struct hlist_node *tmp;
	unsigned int count = 0, bucket, i;
	int ret = 0;

	if (next->count) {
		tasks = kvmalloc_array(MAX_SNAPSHOT, sizeof(*tasks), GFP_KERNEL);
		if (!tasks)
			return -ENOMEM;
	}
	mutex_lock(&policy_lock);
	if (!next->count) {
		release_policy();
		kvfree(next);
		goto out;
	}
	if (!enabled)
		WRITE_ONCE(enabled, true);
	rcu_read_lock();
	for_each_process_thread(group, task) {
		if (!task_eligible(task))
			continue;
		if (count == MAX_SNAPSHOT) {
			ret = -E2BIG;
			goto finish_snapshot;
		}
		get_task_struct(task);
		tasks[count++] = task;
	}
finish_snapshot:
	rcu_read_unlock();
	if (ret) {
		WRITE_ONCE(enabled, policy != NULL);
		goto out;
	}
	kvfree(policy);
	policy = next;
	hash_for_each_safe(leases, bucket, tmp, lease, node) {
		if (READ_ONCE(lease->task->flags) & PF_EXITING)
			free_lease(lease);
	}
	for (i = 0; i < count; i++) {
		apply_task(tasks[i], NULL);
		cond_resched();
	}
out:
	if (!ret)
		revision++;
	mutex_unlock(&policy_lock);
	for (i = 0; i < count; i++)
		put_task_struct(tasks[i]);
	kvfree(tasks);
	return ret;
}

static ssize_t control_write(struct file *file, const char __user *user,
			     size_t size, loff_t *offset)
{
	struct affinity_policy *next;
	bool validate;
	char *text;
	int ret;

	if (!capable(CAP_SYS_NICE))
		return -EPERM;
	if (!size || size > MAX_RULE_BYTES)
		return -EINVAL;
	text = kvmalloc(size + 1, GFP_KERNEL);
	if (!text)
		return -ENOMEM;
	if (copy_from_user(text, user, size)) {
		kvfree(text);
		return -EFAULT;
	}
	if (memchr(text, '\0', size)) {
		kvfree(text);
		return -EINVAL;
	}
	text[size] = '\0';
	validate = size >= 7 && !memcmp(text, "?check\n", 7);
	next = parse_policy(text + (validate ? 7 : 0), size - (validate ? 7 : 0));
	kvfree(text);
	if (IS_ERR(next))
		return PTR_ERR(next);
	if (validate) {
		mutex_lock(&policy_lock);
		validated = next->count;
		mutex_unlock(&policy_lock);
		kvfree(next);
		return size;
	}
	ret = replace_policy(next);
	if (ret) {
		kvfree(next);
		return ret;
	}
	return size;
}

static ssize_t control_read(struct file *file, char __user *user,
			    size_t size, loff_t *offset)
{
	char stats[240];
	int len;

	mutex_lock(&policy_lock);
	len = scnprintf(stats, sizeof(stats),
		"revision=%u\nrules=%u\nvalidated=%u\nleases=%u\npending=%d\nchanged=%ld\nclamped=%ld\nfailed=%ld\ndropped=%ld\n",
		revision, policy ? policy->count : 0, validated, lease_count,
		atomic_read(&pending), atomic_long_read(&changed),
		atomic_long_read(&clamped), atomic_long_read(&failed),
		atomic_long_read(&dropped));
	mutex_unlock(&policy_lock);
	return simple_read_from_buffer(user, size, offset, stats, len);
}

static const struct file_operations control_ops = {
	.owner = THIS_MODULE,
	.read = control_read,
	.write = control_write,
	.llseek = no_llseek,
};

static struct miscdevice control = {
	.minor = MISC_DYNAMIC_MINOR,
	.name = "ace6_thread_affinity",
	.fops = &control_ops,
	.mode = 0600,
};

static int __init affinity_init(void)
{
	unsigned int i;
	int ret;

	affinity_wq = alloc_ordered_workqueue("ace6_affinity", WQ_FREEZABLE | WQ_POWER_EFFICIENT);
	if (!affinity_wq)
		return -ENOMEM;
	for_each_kernel_tracepoint(find_hook, NULL);
	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		if (!hooks[i].tp) {
			ret = -ENOENT;
			goto unregister;
		}
		ret = tracepoint_probe_register(hooks[i].tp, hooks[i].probe, NULL);
		if (ret)
			goto unregister;
	}
	ret = misc_register(&control);
	if (!ret)
		return 0;
unregister:
	while (i--)
		tracepoint_probe_unregister(hooks[i].tp, hooks[i].probe, NULL);
	tracepoint_synchronize_unregister();
	destroy_workqueue(affinity_wq);
	return ret;
}

static void __exit affinity_exit(void)
{
	unsigned int i;

	misc_deregister(&control);
	for (i = 0; i < ARRAY_SIZE(hooks); i++)
		tracepoint_probe_unregister(hooks[i].tp, hooks[i].probe, NULL);
	tracepoint_synchronize_unregister();
	destroy_workqueue(affinity_wq);
	release_policy();
}

module_init(affinity_init);
module_exit(affinity_exit);
MODULE_DESCRIPTION("Rule-driven thread CPU affinity for OnePlus Ace 6");
MODULE_LICENSE("GPL");
