// SPDX-License-Identifier: GPL-2.0
/* Thread affinity policy. Fork/rename event model from kstraccc/cpuset_hook.c. */
#include <linux/capability.h>
#include <linux/binfmts.h>
#include <linux/cgroup.h>
#include <linux/cpuset.h>
#include <linux/fs.h>
#include <linux/hashtable.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/miscdevice.h>
#include <linux/mm.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/pid.h>
#include <linux/sched/cputime.h>
#include <linux/sched/mm.h>
#include <linux/sched/signal.h>
#include <linux/sched/stat.h>
#include <linux/sched/task.h>
#include <linux/sort.h>
#include <linux/threads.h>
#include <linux/tracepoint.h>
#include <linux/uaccess.h>
#include <linux/workqueue.h>

#include "ace6_cpuset_affinity.h"
#include "adaptive_policy.h"

DEFINE_TRACE(ace6_cpuset_attach_task, TP_PROTO(struct task_struct *task),
	     TP_ARGS(task));

#define MAX_PENDING 4096
#define INITIAL_SNAPSHOT 16384
#define INITIAL_SCENE_GROUPS 512
#define SNAPSHOT_RETRIES 3
#define SNAPSHOT_HEADROOM 256
#define MAX_GAME_THREADS 1024
#define GAME_GATE_HITS 3
#define GAME_GATE_TIMEOUT (300 * HZ)
#define GAME_ENTRY_SAMPLES 2
#define GAME_SAMPLES 16
#define GAME_THIRD_HITS 8

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

struct game_sample {
	struct task_struct *task;
	pid_t tid;
	u64 runtime;
	unsigned int top_three;
};

/* Two bounded snapshots retain task identity even when a TID is reused. */
static struct game_sample *game_samples, *game_next;
static struct task_struct *game_leader;
static struct task_struct *game_rank[3];
static unsigned int game_count, game_gate_hits, game_entry_samples, game_passes;
static unsigned long game_deadline;
static u64 game_time;
static bool game_sampling, game_finalized;
static void sample_game(struct work_struct *work);
static DECLARE_DELAYED_WORK(game_work, sample_game);

/* All policy swaps and affinity operations share this lock. Tracepoints only queue work. */
static DEFINE_MUTEX(policy_lock);
static DEFINE_HASHTABLE(leases, 10);
static struct affinity_policy *policy;
#ifndef MODULE
static const struct cred *policy_cred;
#endif
static struct scene_context *scene;
static struct file *scene_owner;
static struct workqueue_struct *affinity_wq;
struct task_snapshot {
	struct task_struct **tasks;
	unsigned int capacity;
	unsigned int count;
	unsigned int growths;
};
static struct task_snapshot snapshot = { .capacity = INITIAL_SNAPSHOT };
static struct task_snapshot scene_groups = { .capacity = INITIAL_SCENE_GROUPS };
static bool enabled;
static unsigned int revision;
static unsigned int validated;
static unsigned int lease_count;
static unsigned int context_switches;
static unsigned int scene_captured_groups, scene_captured_threads;
static atomic_t pending;
static atomic_long_t dropped, changed, clamped, failed, adapted;

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
	const struct cred *saved;
	int ret;

	/* Use the loading client's security identity in asynchronous workers. */
	saved = override_creds(policy_cred);
	ret = ace6_set_task_affinity(task, target, user_request);
	revert_creds(saved);
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

static void release_game_samples(struct game_sample *samples, unsigned int count)
{
	while (count)
		put_task_struct(samples[--count].task);
}

static void stop_game(void)
{
	cancel_delayed_work(&game_work);
	release_game_samples(game_samples, game_count);
	if (game_leader)
		put_task_struct(game_leader);
	game_leader = NULL;
	memset(game_rank, 0, sizeof(game_rank));
	game_count = 0;
	game_time = 0;
	game_gate_hits = 0;
	game_entry_samples = 0;
	game_passes = 0;
	game_sampling = false;
	game_finalized = false;
}

static void start_game(struct task_struct *task, const char *process)
{
	struct affinity_match match;

	if (game_leader || !scene || !scene->game || cpumask_empty(&game_top_cpus) ||
	    task != task->group_leader || strcmp(scene->foreground, process) ||
	    !adaptive_in_scope(policy, task, process))
		return;
	match = select_rule(policy, process, "");
	if (match.rule && match.rule->manual && !strcmp(match.selector->name, process))
		return;
	get_task_struct(task);
	game_leader = task;
	game_deadline = jiffies + GAME_GATE_TIMEOUT;
	queue_delayed_work(affinity_wq, &game_work, 0);
}

static const struct cpumask *game_mask(struct task_struct *task,
				      const struct affinity_rule *rule)
{
	if (game_leader != task->group_leader || (rule && rule->source >= 3))
		return NULL;
	if (task == game_rank[0] || task == game_rank[1])
		return &game_top_cpus;
	if (task == game_rank[2])
		return &game_third_cpus;
	return &interactive_cpus;
}

static void apply_task(struct task_struct *task, struct task_struct *parent, const char *process)
{
	struct thread_lease *lease = find_lease(task);
	const struct affinity_rule *rule = NULL;
	const struct cpumask *requested = NULL;
	const struct cpumask *automatic = NULL;
	char name[PROCESS_NAME_LEN], thread[TASK_COMM_LEN];
	const char *thread_name;
	struct affinity_state observed, original;
	cpumask_t effective, allowed;

	if (!task_eligible(task)) {
		if (lease && (READ_ONCE(task->flags) & PF_EXITING))
			free_lease(lease);
		return;
	}
	if (!process && process_name(task, name))
		process = name;
	if (policy && policy->count && process && *process) {
		struct affinity_match match;

		get_task_comm(thread, task);
		thread_name = strim(thread);
		match = select_rule(policy, process, thread_name);
		rule = match.rule;
		start_game(task, process);
		automatic = game_mask(task, rule);
		if (!strcmp(process, "scene-daemon") && (!rule || !rule->manual)) {
			/* This daemon is managed separately from general app templates. */
			rule = NULL;
			if (scene && !strcmp(policy->adaptive_scope, "*"))
				automatic = scene->display_off ? &tiny_cpus : &little_cpus;
		} else if (!automatic && scene && adaptive_in_scope(policy, task, process)) {
			automatic = adaptive_mask(task, scene_stage(scene, process), thread_name, &match);
		}
		requested = automatic ?: (rule ? &rule->mask : NULL);
	}
	read_affinity(task, &observed);
	original = observed;
	if (!lease && parent) {
		struct thread_lease *origin = find_lease(parent);

		if (origin && owns_affinity(&origin->applied, &observed))
			original = origin->original;
	}
	if (!requested) {
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
	if (!cpumask_and(&effective, &allowed, requested)) {
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
		if (apply_mask(task, requested, requested, &lease->applied)) {
			atomic_long_inc(&failed);
			return;
		}
		atomic_long_inc(&changed);
		if (automatic)
			atomic_long_inc(&adapted);
	}
}

static int compare_game_sample(const void *a, const void *b)
{
	return ((const struct game_sample *)a)->tid - ((const struct game_sample *)b)->tid;
}

static unsigned int capture_game_threads(void)
{
	struct task_struct *task;
	unsigned int count = 0;

	rcu_read_lock();
	for_each_thread(game_leader, task) {
		if (!task_eligible(task))
			continue;
		if (count == MAX_GAME_THREADS)
			break;
		get_task_struct(task);
		game_next[count++] = (struct game_sample){ .task = task, .tid = task_pid_nr(task) };
	}
	rcu_read_unlock();
	sort(game_next, count, sizeof(*game_next), compare_game_sample, NULL);
	return count;
}

static bool game_thread_can_rank(struct task_struct *task)
{
	const struct affinity_rule *rule;
	char name[TASK_COMM_LEN];
	cpumask_t allowed;

	get_task_comm(name, task);
	rule = select_rule(policy, scene->foreground, strim(name)).rule;
	if (rule && rule->source >= 3)
		return false;
	cpuset_cpus_allowed(task, &allowed);
	return cpumask_intersects(&allowed, &game_top_cpus);
}

static void sample_game(struct work_struct *work)
{
	struct game_sample *top[3] = {};
	struct task_struct *next_rank[3] = {};
	u64 deltas[3] = {}, total = 0, now;
	unsigned int i, j, old = 0, count, delay;
	bool gate_opened = false;

	mutex_lock(&policy_lock);
	if (!game_leader || game_finalized)
		goto out;
	if (!task_eligible(game_leader)) {
		stop_game();
		goto out;
	}
	count = capture_game_threads();
	now = ktime_get_ns();
	for (i = 0; i < count; i++) {
		struct game_sample *sample = &game_next[i];
		u64 delta;

		sample->runtime = task_sched_runtime(sample->task);
		while (old < game_count && game_samples[old].tid < sample->tid)
			old++;
		if (old == game_count || game_samples[old].task != sample->task ||
		    sample->runtime < game_samples[old].runtime)
			continue;
		delta = sample->runtime - game_samples[old].runtime;
		total += delta;
		sample->top_three = game_samples[old].top_three;
		if (!delta || delta <= deltas[2] || !game_thread_can_rank(sample->task))
			continue;
		for (j = ARRAY_SIZE(top) - 1; j && delta > deltas[j - 1]; j--) {
			top[j] = top[j - 1];
			deltas[j] = deltas[j - 1];
		}
		top[j] = sample;
		deltas[j] = delta;
	}
	if (game_time && !game_sampling) {
		if (total >= (now - game_time) * 9 / 10)
			game_gate_hits++;
		else
			game_gate_hits = 0;
		gate_opened = game_gate_hits >= GAME_GATE_HITS;
		game_sampling = gate_opened;
	}
	if (game_time && (game_sampling || game_entry_samples < GAME_ENTRY_SAMPLES)) {
		for (j = 0; j < 2; j++)
			next_rank[j] = top[j] ? top[j]->task : NULL;
		if (game_sampling && !gate_opened) {
			game_passes++;
			for (j = 0; j < ARRAY_SIZE(top) && top[j]; j++)
				top[j]->top_three++;
			if (top[2] && top[2]->top_three >= GAME_THIRD_HITS &&
			    deltas[2] * 10 >= deltas[1] * 7)
				next_rank[2] = top[2]->task;
		} else if (!game_sampling) {
			game_entry_samples++;
		}
	} else if (game_time) {
		/* Preserve the entry assignment until the load gate opens. */
		for (i = 0; i < count; i++)
			for (j = 0; j < ARRAY_SIZE(game_rank); j++)
				if (game_next[i].task == game_rank[j])
					next_rank[j] = game_rank[j];
	}
	if (memcmp(game_rank, next_rank, sizeof(game_rank))) {
		struct task_struct *previous[ARRAY_SIZE(game_rank)];

		memcpy(previous, game_rank, sizeof(previous));
		memcpy(game_rank, next_rank, sizeof(game_rank));
		for (j = 0; j < ARRAY_SIZE(game_rank); j++) {
			if (previous[j] && previous[j] != game_rank[j])
				apply_task(previous[j], NULL, scene->foreground);
			if (game_rank[j] && previous[j] != game_rank[j])
				apply_task(game_rank[j], NULL, scene->foreground);
		}
	}
	release_game_samples(game_samples, game_count);
	swap(game_samples, game_next);
	game_count = count;
	game_time = now;
	game_finalized = game_passes >= GAME_SAMPLES ||
		(!game_sampling && time_after_eq(jiffies, game_deadline));
	if (!game_finalized) {
		delay = game_sampling ? 1500 : game_entry_samples < GAME_ENTRY_SAMPLES ? 1200 : 2000;
		queue_delayed_work(affinity_wq, &game_work, msecs_to_jiffies(delay));
	}
out:
	mutex_unlock(&policy_lock);
}

static void run_event(struct work_struct *work)
{
	struct task_event *event = container_of(work, struct task_event, work);
	struct thread_lease *lease;

	mutex_lock(&policy_lock);
	if (event->exit) {
		unsigned int rank;

		if (event->task == game_leader) {
			stop_game();
		} else {
			for (rank = 0; rank < ARRAY_SIZE(game_rank); rank++) {
				if (event->task == game_rank[rank]) {
					game_rank[rank] = NULL;
					game_finalized = false;
					game_passes = 0;
					game_deadline = jiffies + GAME_GATE_TIMEOUT;
					queue_delayed_work(affinity_wq, &game_work, msecs_to_jiffies(1200));
				}
			}
		}
		lease = find_lease(event->task);
		if (lease)
			free_lease(lease);
	} else if (enabled) {
		apply_task(event->task, event->parent, NULL);
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

static void on_cpuset(void *data, struct task_struct *task)
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
	stop_game();
	hash_for_each_safe(leases, bucket, tmp, lease, node)
		restore_lease(lease);
	free_policy(policy);
	policy = NULL;
#ifndef MODULE
	if (policy_cred) {
		put_cred(policy_cred);
		policy_cred = NULL;
	}
#endif
}

static void release_snapshot(struct task_snapshot *buffer)
{
	while (buffer->count)
		put_task_struct(buffer->tasks[--buffer->count]);
}

static bool snapshot_task(struct task_snapshot *buffer, struct task_struct *task)
{
	if (buffer->count >= buffer->capacity)
		return false;
	get_task_struct(task);
	buffer->tasks[buffer->count++] = task;
	return true;
}

static int capture_task_refs(struct task_snapshot *buffer, bool groups_only)
{
	struct task_struct *group, *task;
	int ret = 0;

	rcu_read_lock();
	for_each_process(group) {
		if (groups_only && task_eligible(group)) {
			if (!snapshot_task(buffer, group)) {
				ret = -E2BIG;
				goto finish_snapshot;
			}
			continue;
		}
		for_each_thread(group, task) {
			if (!task_eligible(task))
				continue;
			if (!snapshot_task(buffer, task)) {
				ret = -E2BIG;
				goto finish_snapshot;
			}
			if (groups_only)
				break;
		}
	}
finish_snapshot:
	rcu_read_unlock();
	return ret;
}

static enum app_stage current_stage(const struct scene_context *context, const char *process)
{
	return context ? scene_stage(context, process) : APP_STATIC;
}

static bool scene_affects_process(const struct scene_context *previous,
				  const struct scene_context *next,
				  const char *process)
{
	bool was_game, is_game;

	if (!*process)
		return false;
	if (!strcmp(process, "scene-daemon"))
		return !strcmp(policy->adaptive_scope, "*") &&
			(!previous || !next || previous->display_off != next->display_off);
	was_game = previous && previous->game && !strcmp(previous->foreground, process);
	is_game = next && next->game && !strcmp(next->foreground, process);
	return current_stage(previous, process) != current_stage(next, process) ||
	       was_game != is_game;
}

static bool scene_task_in_scope(struct task_struct *task, const char *process)
{
	return !strcmp(process, "scene-daemon") || adaptive_in_scope(policy, task, process);
}

static int capture_scene_refs(struct task_snapshot *buffer,
			      const struct scene_context *previous,
			      const struct scene_context *next)
{
	char process[PROCESS_NAME_LEN];
	unsigned int i;

	for (i = 0; i < scene_groups.count; i++) {
		struct task_struct *representative = scene_groups.tasks[i];
		struct task_struct *task;

		/* Reading argv can sleep. The pinned representative keeps its
		 * signal struct and thread-group list alive across this step.
		 */
		if (process_name(representative, process) &&
		    scene_affects_process(previous, next, process)) {
			rcu_read_lock();
			for_each_thread(representative, task) {
				if (!task_eligible(task) || !scene_task_in_scope(task, process))
					continue;
				if (!snapshot_task(buffer, task)) {
					rcu_read_unlock();
					return -E2BIG;
				}
			}
			rcu_read_unlock();
		}
		cond_resched();
	}
	return 0;
}

static int reserve_snapshot(struct task_snapshot *buffer, unsigned int capacity)
{
	struct task_struct **tasks;

	tasks = kvmalloc_array(capacity, sizeof(*tasks), GFP_KERNEL | __GFP_NOWARN);
	if (!tasks)
		return -ENOMEM;
	kvfree(buffer->tasks);
	buffer->tasks = tasks;
	buffer->capacity = capacity;
	buffer->growths++;
	return 0;
}

static unsigned int current_thread_count(void)
{
#ifndef MODULE
	return READ_ONCE(nr_threads);
#else
	return 0;
#endif
}

static int capture_snapshot(struct task_snapshot *buffer,
			    const struct scene_context *previous,
			    const struct scene_context *next, bool grow)
{
	bool context = previous || next;
	bool groups_only = buffer == &scene_groups;
	const char *kind = groups_only ? "scene-groups" : context ? "scene-threads" : "full";
	unsigned int attempt, collected, estimate = 0;
	int ret;

	if (!groups_only && grow &&
	    (!context || !previous || !next || previous->display_off != next->display_off)) {
		unsigned int threads = current_thread_count();

		estimate = min_t(unsigned int, threads + SNAPSHOT_HEADROOM, PID_MAX_LIMIT);
		if (estimate > buffer->capacity && reserve_snapshot(buffer, estimate))
			pr_debug("ace6 affinity: deferred capacity estimate %u\n", estimate);
	}
	for (attempt = 0; ; attempt++) {
		ret = context ? capture_scene_refs(buffer, previous, next) :
			capture_task_refs(buffer, groups_only);
		if (ret != -E2BIG || !grow || attempt >= SNAPSHOT_RETRIES ||
		    buffer->capacity >= PID_MAX_LIMIT)
			break;
		/* Never allocate or free task references under RCU. */
		collected = buffer->count;
		release_snapshot(buffer);
		ret = reserve_snapshot(buffer, min_t(unsigned int, buffer->capacity * 2,
						     PID_MAX_LIMIT));
		if (ret)
			goto report;
	}
	collected = buffer->count;
report:
	if (ret)
		pr_warn_ratelimited("ace6 affinity: %s snapshot failed: err=%d threads=%u collected=%u capacity=%u\n",
				    kind, ret, current_thread_count(), collected, buffer->capacity);
	return ret;
}

static void apply_snapshot(const struct scene_context *previous, bool scene_change)
{
	char name[PROCESS_NAME_LEN] = "";
	pid_t last_tgid = 0;
	unsigned int i;

	for (i = 0; i < snapshot.count; i++) {
		struct task_struct *task = snapshot.tasks[i];
		pid_t tgid = task_tgid_nr(task);

		if (tgid != last_tgid) {
			last_tgid = tgid;
			if (!process_name(task, name))
				name[0] = '\0';
		}
		if (!scene_change || (scene_affects_process(previous, scene, name) &&
				      scene_task_in_scope(task, name)))
			apply_task(task, NULL, name);
		cond_resched();
	}
}

static int replace_policy(struct affinity_policy *next)
{
	struct thread_lease *lease;
	struct hlist_node *tmp;
	unsigned int bucket;
	int ret = 0;

	mutex_lock(&policy_lock);
	if (!next->count) {
		release_policy();
		free_policy(next);
		goto out;
	}
	WRITE_ONCE(enabled, true);
	ret = capture_snapshot(&snapshot, NULL, NULL, true);
	if (ret) {
		WRITE_ONCE(enabled, policy != NULL);
		goto out;
	}
	stop_game();
	free_policy(policy);
	policy = next;
#ifndef MODULE
	if (policy_cred)
		put_cred(policy_cred);
	policy_cred = get_current_cred();
#endif
	hash_for_each_safe(leases, bucket, tmp, lease, node) {
		if (READ_ONCE(lease->task->flags) & PF_EXITING)
			free_lease(lease);
	}
	apply_snapshot(NULL, false);
out:
	if (!ret)
		revision++;
	release_snapshot(&snapshot);
	mutex_unlock(&policy_lock);
	return ret;
}

/* A single writer owns scene state until its control fd closes. */
static int replace_scene(struct file *file, struct scene_context *next)
{
	struct scene_context *previous;
	struct thread_lease *lease;
	struct hlist_node *tmp;
	unsigned int bucket;
	bool rescan;
	int ret = 0;

	mutex_lock(&policy_lock);
	if (scene_owner && scene_owner != file) {
		ret = -EBUSY;
		goto out;
	}
	previous = scene;
	if (next && previous && !memcmp(next, previous, sizeof(*next)))
		goto out;
	rescan = policy && policy->adaptive_scope[0];
	if (rescan) {
		ret = capture_snapshot(&scene_groups, NULL, NULL, next != NULL);
		if (!ret)
			ret = capture_snapshot(&snapshot, previous, next, next != NULL);
		if (ret && next)
			goto out;
		scene_captured_groups = scene_groups.count;
		scene_captured_threads = snapshot.count;
	}
	if (game_leader && (!previous || !next || !next->game ||
	    strcmp(previous->foreground, next->foreground)))
		stop_game();
	scene = next;
	WRITE_ONCE(scene_owner, next ? file : NULL);
	context_switches++;
	if (rescan && ret) {
		/* On watcher exit, revisit existing leases even if the full
		 * task snapshot no longer fits.
		 */
		hash_for_each_safe(leases, bucket, tmp, lease, node) {
			struct task_struct *task = lease->task;

			get_task_struct(task);
			apply_task(task, NULL, NULL);
			put_task_struct(task);
			cond_resched();
		}
	} else if (rescan) {
		apply_snapshot(previous, true);
	}
	kfree(previous);
	next = NULL;
out:
	release_snapshot(&snapshot);
	release_snapshot(&scene_groups);
	mutex_unlock(&policy_lock);
	kfree(next);
	return ret;
}

static int control_release(struct inode *inode, struct file *file)
{
	if (READ_ONCE(scene_owner) == file)
		replace_scene(file, NULL);
	return 0;
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
	if (size >= 9 && !memcmp(text, "?context\t", 9)) {
		struct scene_context *next_scene = kzalloc(sizeof(*next_scene), GFP_KERNEL);

		if (!next_scene) {
			kvfree(text);
			return -ENOMEM;
		}
		ret = parse_scene_context(text + 9, next_scene);
		kvfree(text);
		if (ret) {
			kfree(next_scene);
			return ret;
		}
		ret = replace_scene(file, next_scene);
		return ret ? ret : size;
	}
	validate = size >= 7 && !memcmp(text, "?check\n", 7);
	next = parse_policy(text + (validate ? 7 : 0), size - (validate ? 7 : 0));
	kvfree(text);
	if (IS_ERR(next))
		return PTR_ERR(next);
	if (validate) {
		mutex_lock(&policy_lock);
		validated = next->count;
		mutex_unlock(&policy_lock);
		free_policy(next);
		return size;
	}
	ret = replace_policy(next);
	if (ret) {
		free_policy(next);
		return ret;
	}
	return size;
}

static ssize_t control_read(struct file *file, char __user *user,
			    size_t size, loff_t *offset)
{
	char stats[768];
	int len;

	mutex_lock(&policy_lock);
	len = scnprintf(stats, sizeof(stats),
		"revision=%u\nrules=%u\nvalidated=%u\nleases=%u\npending=%d\nchanged=%ld\nclamped=%ld\nfailed=%ld\ndropped=%ld\n"
		"profile_wire=1\nwire_lines=%u\nstored_rules=%u\nprofiles=%u\nprofile_bindings=%u\n"
		"adaptive=%s\nforeground=%s\ndisplay_off=%u\ncontext_switches=%u\nadapted=%ld\n"
		"game_tgid=%d\ngame_top=%d,%d,%d\ngame_samples=%u\ngame_finalized=%u\n"
		"snapshot_capacity=%u\nsnapshot_growths=%u\nscene_group_capacity=%u\n"
		"scene_captured_groups=%u\nscene_captured_threads=%u\n",
		revision, policy ? policy->count : 0, validated, lease_count,
		atomic_read(&pending), atomic_long_read(&changed),
		atomic_long_read(&clamped), atomic_long_read(&failed),
		atomic_long_read(&dropped),
		policy ? policy->wire_lines : 0, policy ? policy->stored_rules : 0,
		policy ? policy->profile_count : 0, policy ? policy->binding_count : 0,
		policy && policy->adaptive_scope[0] ? policy->adaptive_scope : "-",
		scene && scene->foreground[0] ? scene->foreground : "-",
		scene && scene->display_off, context_switches, atomic_long_read(&adapted),
		game_leader ? task_pid_nr(game_leader) : 0,
		game_rank[0] ? task_pid_nr(game_rank[0]) : 0,
		game_rank[1] ? task_pid_nr(game_rank[1]) : 0,
		game_rank[2] ? task_pid_nr(game_rank[2]) : 0, game_passes, game_finalized,
		snapshot.capacity, snapshot.growths, scene_groups.capacity,
		scene_captured_groups, scene_captured_threads);
	mutex_unlock(&policy_lock);
	return simple_read_from_buffer(user, size, offset, stats, len);
}

static const struct file_operations control_ops = {
	.owner = THIS_MODULE,
	.read = control_read,
	.write = control_write,
	.release = control_release,
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
	snapshot.tasks = kvmalloc_array(snapshot.capacity, sizeof(*snapshot.tasks), GFP_KERNEL);
	scene_groups.tasks = kvmalloc_array(scene_groups.capacity, sizeof(*scene_groups.tasks),
					    GFP_KERNEL);
	game_samples = kvmalloc_array(MAX_GAME_THREADS, sizeof(*game_samples), GFP_KERNEL);
	game_next = kvmalloc_array(MAX_GAME_THREADS, sizeof(*game_next), GFP_KERNEL);
	if (!snapshot.tasks || !scene_groups.tasks || !game_samples || !game_next) {
		destroy_workqueue(affinity_wq);
		kvfree(game_next);
		kvfree(game_samples);
		kvfree(scene_groups.tasks);
		kvfree(snapshot.tasks);
		return -ENOMEM;
	}
	init_adaptive_masks();
	for_each_kernel_tracepoint(find_hook, NULL);
	for (i = 0; i < ARRAY_SIZE(hooks); i++) {
		if (!hooks[i].tp) {
			ret = -ENOENT;
			pr_err("ace6_affinity: missing tracepoint %s\n", hooks[i].name);
			goto unregister;
		}
		ret = tracepoint_probe_register(hooks[i].tp, hooks[i].probe, NULL);
		if (ret) {
			pr_err("ace6_affinity: cannot register %s: %d\n", hooks[i].name, ret);
			goto unregister;
		}
	}
	ret = register_trace_ace6_cpuset_attach_task(on_cpuset, NULL);
	if (ret) {
		pr_err("ace6_affinity: cannot register cpuset migration: %d\n", ret);
		goto unregister;
	}
	ret = misc_register(&control);
	if (!ret)
		return 0;
	pr_err("ace6_affinity: cannot register control device: %d\n", ret);
	unregister_trace_ace6_cpuset_attach_task(on_cpuset, NULL);
unregister:
	while (i--)
		tracepoint_probe_unregister(hooks[i].tp, hooks[i].probe, NULL);
	tracepoint_synchronize_unregister();
	destroy_workqueue(affinity_wq);
	kvfree(game_next);
	kvfree(game_samples);
	kvfree(scene_groups.tasks);
	kvfree(snapshot.tasks);
	return ret;
}

static void __exit affinity_exit(void)
{
	unsigned int i;

	misc_deregister(&control);
	unregister_trace_ace6_cpuset_attach_task(on_cpuset, NULL);
	for (i = 0; i < ARRAY_SIZE(hooks); i++)
		tracepoint_probe_unregister(hooks[i].tp, hooks[i].probe, NULL);
	tracepoint_synchronize_unregister();
	mutex_lock(&policy_lock);
	release_policy();
	mutex_unlock(&policy_lock);
	cancel_delayed_work_sync(&game_work);
	destroy_workqueue(affinity_wq);
	kfree(scene);
	kvfree(game_next);
	kvfree(game_samples);
	kvfree(scene_groups.tasks);
	kvfree(snapshot.tasks);
}

module_init(affinity_init);
module_exit(affinity_exit);
MODULE_DESCRIPTION("Rule-driven thread CPU affinity for OnePlus Ace 6");
MODULE_LICENSE("GPL");
