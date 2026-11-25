"""Exercise the kernel snapshot helpers with mock task references and allocation."""
from pathlib import Path
import subprocess
from tempfile import TemporaryDirectory


SOURCE = Path(__file__).with_name("ace6_thread_affinity.c")
MOCKS = r"""
#include <assert.h>
#include <errno.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define PID_MAX_LIMIT 32
#define GFP_KERNEL 0
#define __GFP_NOWARN 0
#define min_t(type, a, b) ((type)(a) < (type)(b) ? (type)(a) : (type)(b))

struct task_struct { unsigned int refs; };
static struct task_struct tasks[PID_MAX_LIMIT + 1];
static struct task_struct **snapshot;
static unsigned int snapshot_capacity, snapshot_growths, task_count;
static bool under_rcu, fail_allocations;

#define for_each_process_thread(group, task) \
	for (unsigned int i = 0; (void)(group), i < task_count && ((task) = &tasks[i]); i++)

static bool task_eligible(struct task_struct *task) { return task != NULL; }
static void rcu_read_lock(void) { assert(!under_rcu); under_rcu = true; }
static void rcu_read_unlock(void) { assert(under_rcu); under_rcu = false; }
static void get_task_struct(struct task_struct *task) { assert(under_rcu); task->refs++; }
static void put_task_struct(struct task_struct *task) { assert(!under_rcu && task->refs); task->refs--; }
static void *kvmalloc_array(size_t count, size_t size, int flags)
{
	(void)flags;
	assert(!under_rcu);
	for (unsigned int i = 0; i < task_count; i++)
		assert(tasks[i].refs == 0);
	return fail_allocations ? NULL : calloc(count, size);
}
static void kvfree(void *ptr) { free(ptr); }
"""

TESTS = r"""
static void reset(unsigned int tasks_to_scan)
{
	kvfree(snapshot);
	snapshot_capacity = 4;
	snapshot_growths = 0;
	task_count = tasks_to_scan;
	fail_allocations = false;
	snapshot = kvmalloc_array(snapshot_capacity, sizeof(*snapshot), 0);
	assert(snapshot);
}

static void check_refs(unsigned int pinned)
{
	for (unsigned int i = 0; i < task_count; i++)
		assert(tasks[i].refs == (i < pinned ? 1 : 0));
}

int main(void)
{
	unsigned int count;

	reset(4);
	assert(capture_tasks(&count, true) == 0 && count == 4 && snapshot_growths == 0);
	check_refs(4);
	release_snapshot(count);
	check_refs(0);

	reset(13);
	assert(capture_tasks(&count, true) == 0 && count == 13);
	assert(snapshot_capacity == 16 && snapshot_growths == 2);
	check_refs(13);
	release_snapshot(count);
	check_refs(0);

	reset(5);
	fail_allocations = true;
	assert(capture_tasks(&count, true) == -ENOMEM && count == 0);
	assert(snapshot_capacity == 4 && snapshot_growths == 0);
	check_refs(0);
	fail_allocations = false;
	assert(capture_tasks(&count, true) == 0 && count == 5);
	release_snapshot(count);
	check_refs(0);

	reset(5);
	assert(capture_tasks(&count, false) == -E2BIG && count == 4);
	assert(snapshot_capacity == 4 && snapshot_growths == 0);
	check_refs(4);
	release_snapshot(count);
	check_refs(0);

	reset(PID_MAX_LIMIT + 1);
	assert(capture_tasks(&count, true) == -E2BIG && count == PID_MAX_LIMIT);
	assert(snapshot_capacity == PID_MAX_LIMIT);
	release_snapshot(count);
	check_refs(0);
	kvfree(snapshot);
	return 0;
}
"""


def main():
    source = SOURCE.read_text(encoding="utf-8")
    first = source.index("static void release_snapshot(")
    last = source.index("static enum app_stage current_stage(", first)
    with TemporaryDirectory() as directory:
        path = Path(directory)
        test = path / "test.c"
        binary = path / "test"
        test.write_text(MOCKS + source[first:last] + TESTS, encoding="utf-8")
        subprocess.run(["cc", "-std=gnu11", "-O2", "-Wall", "-Wextra", "-Werror",
                        str(test), "-o", str(binary)], check=True)
        subprocess.run([str(binary)], check=True)
    print("Task snapshot boundaries, growth, failed allocation and reference cleanup passed")


if __name__ == "__main__":
    main()
