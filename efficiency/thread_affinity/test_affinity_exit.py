"""Exercise the actual affinity driver and private setter with host-side kernel stubs."""
from argparse import ArgumentParser
from pathlib import Path
import re
import subprocess
from tempfile import TemporaryDirectory


HERE = Path(__file__).resolve().parent
MOCK_TYPES = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TASK_COMM_LEN 16
#define PROCESS_NAME_LEN 128
#define PF_EXITING 0x4
#define PF_KTHREAD 0x200000
#define PF_NO_SETAFFINITY 0x04000000
#define SCHED_DEADLINE 6
#define GFP_KERNEL 0
#define NUMA_NO_NODE (-1)
#define SCA_USER 1
#define GAME_GATE_TIMEOUT 300
#define READ_ONCE(value) (value)
#define ARRAY_SIZE(array) (sizeof(array) / sizeof((array)[0]))

typedef unsigned long long u64;
typedef int pid_t;
typedef int atomic_long_t;
typedef struct cpumask { unsigned int bits; } cpumask_t;
struct hlist_node { struct hlist_node *next; };
struct work_struct { int unused; };
"""

MOCK_KERNEL = r"""
struct task_struct {
	unsigned int flags, policy, refs;
	pid_t tgid;
	char comm[TASK_COMM_LEN];
	struct affinity_state affinity;
	cpumask_t cpus_mask;
};
struct affinity_context {
	const struct cpumask *new_mask;
	struct cpumask *user_mask;
	unsigned int flags;
};
struct cred { int unused; };
struct scene_context { bool display_off; };
struct affinity_rule { bool manual; cpumask_t mask; };
struct affinity_match { const struct affinity_rule *rule; };
struct affinity_policy { unsigned int count; char adaptive_scope[PROCESS_NAME_LEN]; };

static unsigned int lease_count, reads, exit_on_read, setter_calls, read_flags, late_flags;
static int security_result, late_result, pending;
static bool policy_lock, fail_lease_alloc, fail_user_alloc;
static atomic_long_t clamped, changed, adapted, failed, exit_skipped;
static struct hlist_node *leases;
static const struct cred identity;
static const struct cred *policy_cred = &identity;
static struct affinity_policy active_policy = { 1, "*" };
static struct affinity_policy *policy;
static struct affinity_rule test_rule = { true, {3} };
static struct scene_context *scene;
static const cpumask_t tiny_cpus = {1}, little_cpus = {3};
static struct task_struct *game_leader, *game_rank[3];
static bool game_finalized, enabled;
static unsigned int game_passes, game_deadline, jiffies, affinity_wq, game_work;

#define node_to_lease(link) container_of(link, struct thread_lease, node)
#define container_of(ptr, type, field) ((type *)((char *)(ptr) - offsetof(type, field)))
#define hash_for_each_possible(head, lease, member, key) \
	for (struct hlist_node *link = (head); link && ((lease) = node_to_lease(link)); \
	     link = link->next)
#define hash_add(head, link, key) ((link)->next = (head), (head) = (link))

static void hash_del(struct hlist_node *node)
{
	struct hlist_node **link = &leases;

	while (*link && *link != node)
		link = &(*link)->next;
	assert(*link == node);
	*link = node->next;
}

#define mutex_lock(lock) (assert(!*(lock)), *(lock) = true)
#define mutex_unlock(lock) (assert(*(lock)), *(lock) = false)
#define atomic_long_inc(counter) (++*(counter))
#define atomic_dec(counter) (--*(counter))
#define cpumask_copy(dst, src) (*(dst) = *(src))
#define cpumask_equal(a, b) ((a)->bits == (b)->bits)
#define cpumask_and(dst, a, b) ((dst)->bits = (a)->bits & (b)->bits)
#define task_tgid_nr(task) ((task)->tgid)
#define get_task_comm(dest, task) memcpy((dest), (task)->comm, TASK_COMM_LEN)
#define get_task_struct(task) (++(task)->refs)
#define put_task_struct(task) (assert((task)->refs), --(task)->refs)
#define kzalloc(size, flags) (fail_lease_alloc ? NULL : calloc(1, (size)))
#define kfree(ptr) free(ptr)
#define cpuset_cpus_allowed(task, allowed) ((void)(task), (allowed)->bits = 15)
#define override_creds(cred) (++setter_calls, (cred))
#define revert_creds(cred) assert((cred) == policy_cred)
#define process_name(task, name) ((void)(task), (void)(name), false)
#define strim(name) (name)
#define select_rule(policy, process, thread) \
	((void)(policy), (void)(process), (void)(thread), (struct affinity_match){ &test_rule })
#define start_game(task, process) ((void)(task), (void)(process))
#define stop_game() assert(false)
#define queue_delayed_work(queue, work, delay) ((void)(queue), (void)(work), (void)(delay))
#define msecs_to_jiffies(value) (value)
#define game_mask(task, rule) ((void)(task), (void)(rule), NULL)
#define adaptive_in_scope(policy, task, process) ((void)(policy), (void)(task), (void)(process), false)
#define adaptive_mask(task, stage, name, match) ((void)(task), (void)(name), (void)(match), NULL)

static void read_affinity(struct task_struct *task, struct affinity_state *state)
{
	assert(task->refs);
	*state = task->affinity;
	if (++reads == exit_on_read)
		task->flags |= read_flags;
}

#define trace_android_vh_sched_setaffinity_early(task, mask, skip) \
	((void)(task), (void)(mask), assert(!*(skip)))
#define security_task_setscheduler(task) ((void)(task), security_result)
#define alloc_user_cpus_ptr(node) (fail_user_alloc ? NULL : calloc(1, sizeof(cpumask_t)))
#define trace_android_rvh_sched_setaffinity(task, mask, ret) \
	((void)(mask), (task)->flags |= late_flags, *(ret) = late_result)

static int __sched_setaffinity(struct task_struct *task, struct affinity_context *ctx)
{
	assert(ctx->flags == SCA_USER);
	task->cpus_mask = task->affinity.effective = *ctx->new_mask;
	task->affinity.has_request = ctx->user_mask != NULL;
	task->affinity.requested = ctx->user_mask ? *ctx->user_mask : (cpumask_t){0};
	return 0;
}

"""

TESTS = r"""
static struct task_struct new_task(void)
{
	return (struct task_struct){
		.tgid = 20, .refs = 1, .comm = "POSIX timer 175",
		.cpus_mask = {15}, .affinity = { .effective = {15} },
	};
}

static void reset(void)
{
	assert(!leases && !lease_count && !pending);
	failed = exit_skipped = 0;
	reads = exit_on_read = read_flags = late_flags = setter_calls = 0;
	security_result = late_result = changed = clamped = adapted = 0;
	fail_lease_alloc = fail_user_alloc = false;
	policy = &active_policy;
	test_rule.mask.bits = 3;
}

static void apply(struct task_struct *task, struct task_struct *parent)
{
	mutex_lock(&policy_lock);
	apply_task(task, parent, "app.test");
	mutex_unlock(&policy_lock);
}

static void exit_event(struct task_struct *task)
{
	struct task_event *event = calloc(1, sizeof(*event));

	assert(event);
	event->exit = true;
	event->task = task;
	task->flags |= PF_EXITING;
	get_task_struct(task);
	pending++;
	run_event(&event->work);
	assert(!pending && task->refs == 1 && !find_lease(task));
}

static void test_apply(void)
{
	struct task_struct task = new_task();

	reset();
	exit_on_read = 1;
	read_flags = PF_EXITING;
	apply(&task, NULL);
	assert(exit_skipped == 1 && !failed && !changed);

	assert(!lease_count && task.refs == 1);
	exit_event(&task);
	apply(&task, NULL);
	assert(setter_calls == 1 && exit_skipped == 1 && !lease_count);

	reset();
	task = new_task();
	apply(&task, NULL);
	assert(lease_count == 1 && task.refs == 2 && changed == 1 && task.cpus_mask.bits == 3);
	test_rule.mask.bits = 1;
	exit_on_read = reads + 1;
	read_flags = PF_EXITING;
	apply(&task, NULL);
	assert(exit_skipped == 1 && !failed && changed == 1);

	assert(!lease_count && task.refs == 1);
	exit_event(&task);

	reset();
	task = new_task();
	apply(&task, NULL);
	task.flags = PF_EXITING;
	apply(&task, NULL);
	assert(!exit_skipped && !failed && setter_calls == 1);
	assert(!lease_count && task.refs == 1);
	exit_event(&task);

	reset();
	task = new_task();
	late_flags = PF_EXITING;
	apply(&task, NULL);
	assert(changed == 1 && !exit_skipped && !failed);
	exit_event(&task);
}

static void test_true_errors(void)
{
	const unsigned int cases[] = { PF_NO_SETAFFINITY, PF_NO_SETAFFINITY | PF_EXITING, 0, 0 };

	for (unsigned int i = 0; i < ARRAY_SIZE(cases); i++) {
		struct task_struct task = new_task();

		reset();
		exit_on_read = 1;
		read_flags = cases[i];
		if (i >= 2) {
			late_result = -ESRCH;
			late_flags = i == 3 ? PF_EXITING : 0;
		}
		apply(&task, NULL);
		assert(failed == 1 && !exit_skipped && !changed);

		assert(lease_count == 1);
		exit_event(&task);
	}

	reset();
	struct task_struct task = new_task();
	security_result = -EPERM;
	apply(&task, NULL);
	assert(failed == 1 && !exit_skipped && !changed);

	exit_event(&task);

	reset();
	task = new_task();
	fail_user_alloc = true;
	apply(&task, NULL);
	assert(failed == 1 && !exit_skipped);

	exit_event(&task);
}

static void test_restore(bool inherit)
{
	for (unsigned int i = 0; i < 3; i++) {
		struct task_struct parent = new_task(), child = new_task();
		struct task_struct *task = inherit ? &child : &parent;

		reset();
		apply(&parent, NULL);
		child.affinity = parent.affinity;
		child.cpus_mask = parent.cpus_mask;
		policy = NULL;
		if (i == 1) {
			exit_on_read = reads + (inherit ? 1 : 2);
			read_flags = PF_EXITING;
		}
		if (i == 2)
			security_result = -EPERM;
		apply(task, inherit ? &parent : NULL);
		assert(!find_lease(task) && task->refs == 1 && changed == 1);
		assert(exit_skipped == (i == 1));
		assert(failed == (i == 2));
		if (i == 0)
			assert(task->cpus_mask.bits == 15 && !task->affinity.has_request);
		if (inherit) {
			assert(lease_count == 1 && parent.refs == 2);
			exit_event(&parent);
		}
		exit_event(task);
	}
}

static void test_private_setter(void)
{
	const struct cpumask mask = {3};
	const unsigned int flags[] = { PF_EXITING, PF_NO_SETAFFINITY, PF_EXITING | PF_NO_SETAFFINITY, 0 };

	for (unsigned int i = 0; i < ARRAY_SIZE(flags); i++) {
		struct task_struct task = new_task();
		bool expected_exit = flags[i] == PF_EXITING;
		bool exiting = !expected_exit;

		reset();
		task.flags = flags[i];
		if (!task.flags) {
			late_result = -ESRCH;
			late_flags = PF_EXITING;
		}
		assert(ace6_set_task_affinity(&task, &mask, &mask, &exiting) == -ESRCH);
		assert(exiting == expected_exit);
	}
}

static void test_lease_allocation_failure(void)
{
	for (unsigned int i = 0; i < 2; i++) {
		struct task_struct task = new_task();

		reset();
		fail_lease_alloc = true;
		exit_on_read = 1;
		read_flags = i ? PF_EXITING : 0;
		apply(&task, NULL);
		assert(failed == 1 && !exit_skipped && !changed);
		assert(!setter_calls && !lease_count && task.refs == 1 && !find_lease(&task));
		exit_event(&task);
	}
}

int main(void)
{
	test_private_setter();
	test_apply();
	test_true_errors();
	test_restore(false);
	test_restore(true);
	test_lease_allocation_failure();
	puts("Affinity exit classification, lease cleanup and status ABI passed");
	return 0;
}
"""


def region(source, first, stop):
    start = source.index(first)
    return source[start:source.index(stop, start)]


def check_status_size(source):
    status = region(source, "static ssize_t control_read(", "static const struct file_operations control_ops")
    capacity = int(re.search(r"char stats\[(\d+)\];", status)[1])
    format_part = region(status, "len = scnprintf", "\n\t\trevision,")
    fmt = "".join(re.findall(r'"([^"]+)"', format_part)).replace(r"\n", "\n")
    assert fmt.startswith("revision=%u\nrules=%u\nvalidated=%u\nleases=%u\npending=%d\n"
                          "changed=%ld\nclamped=%ld\nfailed=%ld\ndropped=%ld\n")
    assert fmt.endswith("\nexit_skipped=%ld\n")
    field_widths = {"%u": 10, "%d": 11, "%ld": 20, "%s": 127}
    max_size = len(re.sub(r"%\w+", lambda match: "x" * field_widths[match[0]], fmt))
    assert max_size < capacity


def main():
    parser = ArgumentParser(description=__doc__)
    parser.add_argument("--cc", default="cc")
    args = parser.parse_args()
    source = (HERE / "ace6_thread_affinity.c").read_text(encoding="utf-8")
    setter = (HERE / "ace6_sched_affinity.inc").read_text(encoding="utf-8")
    check_status_size(source)
    with TemporaryDirectory() as directory:
        path = Path(directory)
        test = path / "test.c"
        binary = path / "test.exe"
        test.write_text(
            MOCK_TYPES
            + region(source, "struct affinity_state {", "struct game_sample")
            + MOCK_KERNEL
            + region(setter, "int ace6_set_task_affinity(", "\n}\n") + "\n}\n"
            + region(source, "static struct thread_lease *find_lease(", "static void read_affinity(")
            + region(source, "static bool same_request(", "static bool process_name(")
            + region(source, "enum affinity_result", "static void release_game_samples(")
            + region(source, "static void apply_task(", "static int compare_game_sample(")
            + region(source, "static void run_event(", "static void queue_event(")
            + TESTS,
            encoding="utf-8",
        )
        subprocess.run([args.cc, "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-Wno-unused-parameter", str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)


if __name__ == "__main__":
    main()
