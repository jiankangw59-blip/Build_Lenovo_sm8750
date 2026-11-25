"""Test the kernel snapshot filters, growth and reference lifetime on the host."""
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


HERE = Path(__file__).resolve().parent
MOCKS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#define PID_MAX_LIMIT 32
#define PROCESS_NAME_LEN 128
#define SNAPSHOT_RETRIES 3
#define SNAPSHOT_HEADROOM 4
#define GFP_KERNEL 0
#define __GFP_NOWARN 0
#define READ_ONCE(v) (v)
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))

struct task_struct {
	unsigned int refs, group_id;
	bool ineligible, out_of_scope;
	const char *name;
};
struct task_snapshot {
	struct task_struct **tasks;
	unsigned int capacity, count, growths;
};
struct scene_context {
	char foreground[128], visible[1024];
	bool display_off, game;
};
enum app_stage { APP_STATIC, APP_FOREGROUND, APP_BACKGROUND, APP_DISPLAY_OFF };
static struct { char adaptive_scope[128]; } active_policy = { "*" };
static typeof(active_policy) *policy = &active_policy;
static struct task_struct tasks[PID_MAX_LIMIT + 1];
static struct task_snapshot snapshot, scene_groups;
static unsigned int task_count, group_count, nr_threads, warnings;
static bool under_rcu, fail_allocations;

#define for_each_process(group) \
	for (unsigned int g = 0; g < group_count && \
			((group) = &tasks[(g * task_count + group_count - 1) / group_count]); g++)
#define for_each_thread(group, task) \
	for (unsigned int i = 0; i < task_count; i++) \
		if (((task) = &tasks[i])->group_id == (group)->group_id)

static bool task_eligible(struct task_struct *task) { return !task->ineligible; }
static bool adaptive_in_scope(const void *rules, struct task_struct *task, const char *name)
{
	assert(rules && *name);
	return !task->out_of_scope;
}
static bool process_name(struct task_struct *task, char *name)
{
	assert(!under_rcu && task->refs);
	strcpy(name, task->name);
	return *name != '\0';
}
static void rcu_read_lock(void) { assert(!under_rcu); under_rcu = true; }
static void rcu_read_unlock(void) { assert(under_rcu); under_rcu = false; }
static void get_task_struct(struct task_struct *task) { assert(under_rcu); task->refs++; }
static void put_task_struct(struct task_struct *task) { assert(!under_rcu && task->refs); task->refs--; }
static void cond_resched(void) { assert(!under_rcu); }
static void *kvmalloc_array(size_t count, size_t size, int flags)
{
	(void)flags;
	assert(!under_rcu);
	return fail_allocations ? NULL : calloc(count, size);
}
static void kvfree(void *ptr) { free(ptr); }
static void pr_debug(const char *fmt, ...) { (void)fmt; }
static void pr_warn_ratelimited(const char *fmt, ...) { (void)fmt; warnings++; }
"""

TESTS = r"""
static void reset(unsigned int tasks_to_scan, unsigned int groups_to_scan)
{
	kvfree(snapshot.tasks);
	kvfree(scene_groups.tasks);
	memset(tasks, 0, sizeof(tasks));
	task_count = tasks_to_scan;
	group_count = groups_to_scan;
	nr_threads = 0;
	warnings = 0;
	fail_allocations = false;
	snapshot = (struct task_snapshot){ .capacity = 4 };
	scene_groups = (struct task_snapshot){ .capacity = 2 };
	snapshot.tasks = kvmalloc_array(snapshot.capacity, sizeof(*snapshot.tasks), 0);
	scene_groups.tasks = kvmalloc_array(scene_groups.capacity, sizeof(*scene_groups.tasks), 0);
	assert(snapshot.tasks && scene_groups.tasks);
	for (unsigned int i = 0; i < task_count; i++)
		tasks[i].group_id = i * group_count / task_count;
}

static void check_refs(unsigned int pinned)
{
	for (unsigned int i = 0; i < task_count; i++)
		assert(tasks[i].refs == (i < pinned ? 1 : 0));
}

static void test_growth(void)
{
	reset(4, 2);
	assert(capture_snapshot(&snapshot, NULL, NULL, true) == 0 && snapshot.count == 4);
	assert(snapshot.growths == 0);
	check_refs(4);
	release_snapshot(&snapshot);
	check_refs(0);

	reset(13, 4);
	assert(capture_snapshot(&snapshot, NULL, NULL, true) == 0 && snapshot.count == 13);
	assert(snapshot.capacity == 16 && snapshot.growths == 2);
	check_refs(13);
	release_snapshot(&snapshot);
	check_refs(0);
	nr_threads = 20;
	assert(capture_snapshot(&snapshot, NULL, NULL, true) == 0 && snapshot.count == 13);
	assert(snapshot.capacity == 24 && snapshot.growths == 3);
	release_snapshot(&snapshot);
	check_refs(0);

	reset(5, 2);
	fail_allocations = true;
	assert(capture_snapshot(&snapshot, NULL, NULL, true) == -ENOMEM && snapshot.count == 0);
	assert(snapshot.capacity == 4 && snapshot.growths == 0 && warnings == 1);
	check_refs(0);
	fail_allocations = false;
	assert(capture_snapshot(&snapshot, NULL, NULL, true) == 0 && snapshot.count == 5);
	release_snapshot(&snapshot);
	check_refs(0);

	reset(5, 2);
	assert(capture_snapshot(&snapshot, NULL, NULL, false) == -E2BIG && snapshot.count == 4);
	assert(snapshot.capacity == 4 && snapshot.growths == 0 && warnings == 1);
	check_refs(4);
	release_snapshot(&snapshot);
	check_refs(0);

	reset(PID_MAX_LIMIT + 1, 5);
	assert(capture_snapshot(&snapshot, NULL, NULL, true) == -E2BIG);
	assert(snapshot.capacity == PID_MAX_LIMIT && snapshot.count == PID_MAX_LIMIT);
	release_snapshot(&snapshot);
	check_refs(0);
}

static void expect_scene(const struct scene_context *old, const struct scene_context *next,
			 const unsigned int *expected, unsigned int count)
{
	assert(capture_snapshot(&scene_groups, NULL, NULL, true) == 0);
	assert(scene_groups.count == group_count);
	assert(capture_snapshot(&snapshot, old, next, true) == 0);
	assert(snapshot.count == count);
	for (unsigned int i = 0; i < count; i++)
		assert(snapshot.tasks[i] == &tasks[expected[i]]);
	release_snapshot(&snapshot);
	release_snapshot(&scene_groups);
	check_refs(0);
}

static void test_scene(void)
{
	const struct scene_context old = { .foreground = "app.old", .visible = "app.window,app.stays" };
	const struct scene_context next = { .foreground = "app.new", .visible = "app.stays" };
	const struct scene_context game = { .foreground = "app.new", .visible = "app.stays", .game = true };
	const struct scene_context off = { .foreground = "app.new", .visible = "app.stays", .display_off = true };
	const char *const names[] = { "app.old", "app.new:worker", "app.window", "app.stays",
				      "app.unrelated", "scene-daemon", "app.new" };

	reset(14, 7);
	for (unsigned int i = 0; i < task_count; i++)
		tasks[i].name = names[tasks[i].group_id];
	tasks[0].ineligible = true;
	tasks[3].out_of_scope = true;
	tasks[10].out_of_scope = true;
	tasks[11].out_of_scope = true;
	expect_scene(&old, &next, (const unsigned int[]){1, 2, 4, 5, 12, 13}, 6);
	expect_scene(&next, &game, (const unsigned int[]){12, 13}, 2);
	expect_scene(&next, &off, (const unsigned int[]){1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}, 12);
	expect_scene(&off, NULL, (const unsigned int[]){1, 2, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13}, 12);
}

int main(void)
{
	test_growth();
	test_scene();
	kvfree(scene_groups.tasks);
	kvfree(snapshot.tasks);
	return 0;
}
"""


def source_region(source, first, stop):
    start = source.index(first)
    return source[start:source.index(stop, start)]


def main():
    source = (HERE / "ace6_thread_affinity.c").read_text(encoding="utf-8")
    adaptive = (HERE / "adaptive_policy.h").read_text(encoding="utf-8")
    package = source_region(adaptive, "static bool same_package(", "static bool adaptive_in_scope(")
    stages = source_region(adaptive, "static enum app_stage scene_stage(", "\n}\n") + "\n}\n"
    snapshots = source_region(source, "static void release_snapshot(", "static void apply_snapshot(")
    with TemporaryDirectory() as directory:
        path = Path(directory)
        test = path / "test.c"
        binary = path / "test"
        test.write_text(MOCKS + package + stages + snapshots + TESTS, encoding="utf-8")
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("Snapshot growth, capacity fallback, reference cleanup and Scene filtering passed")


if __name__ == "__main__":
    main()
