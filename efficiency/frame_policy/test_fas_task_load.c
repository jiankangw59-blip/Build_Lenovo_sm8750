// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include "fas_task_load.h"

#define SAMPLE(t, c, s, r) { .tid = (t), .cpu = (c), .start_time = (s), .runtime_ns = (r), .allowed_mask = (u64)1 << (c) }

static void basic_identity_and_migration(void)
{
	struct fp_fas_task_load out;
	struct fp_fas_task_sample previous[] = {
		SAMPLE(1, 0, 10, 100), SAMPLE(2, 1, 20, 50), SAMPLE(4, 2, 40, 200),
	};
	struct fp_fas_task_sample current[] = {
		SAMPLE(1, 7, 10, 180), SAMPLE(3, 3, 30, 900), SAMPLE(4, 2, 40, 200),
	};
	assert(fp_fas_task_measure(previous, 3, current, 3, 100, &out));
	assert(out.valid && out.error == FP_FAS_TASK_OK);
	assert(out.util_permille == 800 && out.top8_cpu_mask == ((u64)1 << 7));
	assert(out.measured == 2 && out.new_tasks == 1 && out.exited == 1);
	assert(!out.rollback && out.top_count == 1);
	current[0].start_time++;
	assert(fp_fas_task_measure(previous, 3, current, 3, 100, &out));
	assert(out.measured == 1 && out.new_tasks == 2 && out.exited == 2);
	assert(out.util_permille == 0 && out.top_count == 0 && !out.top8_cpu_mask);
}

static void rollback_and_lifecycle(void)
{
	struct fp_fas_task_load out;
	struct fp_fas_task_sample previous[] = { SAMPLE(1, 0, 1, 100), SAMPLE(2, 1, 2, 200) };
	struct fp_fas_task_sample current[] = { SAMPLE(1, 0, 1, 99), SAMPLE(2, 1, 2, 250) };
	assert(fp_fas_task_measure(previous, 2, current, 2, 100, &out));
	assert(out.rollback == 1 && out.measured == 1 && out.util_permille == 500);
	assert(out.top8_cpu_mask == 2);
	assert(!fp_fas_task_measure(previous, 1, current, 1, 100, &out));
	assert(out.rollback == 1 && !out.measured && !out.valid && !out.top8_cpu_mask);
	current[0].start_time = 2;
	assert(!fp_fas_task_measure(previous, 1, current, 1, 100, &out));
	assert(out.new_tasks == 1 && out.exited == 1 && !out.rollback);
	assert(!fp_fas_task_measure(previous, 2, NULL, 0, 100, &out));
	assert(out.exited == 2 && !out.new_tasks && !out.measured);
	assert(!fp_fas_task_measure(NULL, 0, current, 2, 100, &out));
	assert(out.new_tasks == 2 && !out.exited && !out.measured);
	assert(!fp_fas_task_measure(NULL, 0, NULL, 0, 100, &out));
	assert(out.error == FP_FAS_TASK_OK && !out.valid);
}

static void deterministic_top_eight(void)
{
	struct fp_fas_task_load out;
	struct fp_fas_task_sample previous[10], current[10];
	u32 i;

	for (i = 0; i < 10; i++) {
		previous[i] = (struct fp_fas_task_sample)SAMPLE(i + 1, i, 1, 0);
		current[i] = (struct fp_fas_task_sample)SAMPLE(i + 1, i, 1, 100);
	}
	assert(fp_fas_task_measure(previous, 10, current, 10, 1000, &out));
	assert(out.measured == 10 && out.top_count == 8 && out.top8_cpu_mask == 0xff);
	assert(out.util_permille == 100);
	for (i = 0; i < 10; i++) current[i].runtime_ns = (i + 1) * 100;
	assert(fp_fas_task_measure(previous, 10, current, 10, 1000, &out));
	assert(out.top8_cpu_mask == 0x3fc && out.util_permille == 1000);
	for (i = 0; i < 10; i++) current[i].runtime_ns = i + 1;
	assert(fp_fas_task_measure(previous, 10, current, 10, 1000000, &out));
	assert(out.top8_cpu_mask == 0x3fc && out.util_permille == 0);
	/* Same TID with distinct lifetime identities remains deterministically ordered. */
	for (i = 0; i < 10; i++) {
		previous[i].tid = current[i].tid = 1;
		previous[i].start_time = current[i].start_time = i + 1;
		current[i].runtime_ns = 100;
	}
	assert(fp_fas_task_measure(previous, 10, current, 10, 1000, &out));
	assert(out.top8_cpu_mask == 0xff);
}

static void single_thread_load_not_cpu_sum(void)
{
	struct fp_fas_task_load out;
	struct fp_fas_task_sample previous[8], current[8];
	u32 i;

	for (i = 0; i < 8; i++) {
		previous[i] = (struct fp_fas_task_sample)SAMPLE(i + 1, 3, 1, 0);
		current[i] = (struct fp_fas_task_sample)SAMPLE(i + 1, 3, 1, 100);
	}
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(out.util_permille == 100 && out.top8_cpu_mask == 8);
	current[0].runtime_ns = 0;
	assert(fp_fas_task_measure(previous, 1, current, 1, 1000, &out));
	assert(out.valid && out.measured == 1 && !out.top_count && !out.top8_cpu_mask);
	current[0].cpu = 63;
	current[0].runtime_ns = 1;
	assert(fp_fas_task_measure(previous, 1, current, 1, 1000, &out));
	assert(out.top8_cpu_mask == ((u64)1 << 63));
}

static void invalid_input(void)
{
	struct fp_fas_task_load out;
	struct fp_fas_task_sample a[] = { SAMPLE(1, 0, 1, 0), SAMPLE(2, 1, 1, 0) };
	struct fp_fas_task_sample b[] = { SAMPLE(1, 0, 1, 1), SAMPLE(2, 1, 1, 1) };
	assert(!fp_fas_task_measure(a, 2, b, 2, 0, &out));
	assert(out.error == FP_FAS_TASK_INVALID_INTERVAL && !out.valid);
	assert(!fp_fas_task_measure(NULL, 1, b, 2, 1, &out));
	assert(out.error == FP_FAS_TASK_INVALID_ARGUMENT);
	assert(!fp_fas_task_measure(a, 2, NULL, 1, 1, &out));
	assert(out.error == FP_FAS_TASK_INVALID_ARGUMENT);
	b[1].tid = 1;
	assert(!fp_fas_task_measure(a, 2, b, 2, 1, &out));
	assert(out.error == FP_FAS_TASK_INVALID_ORDER);
	b[1].tid = 0;
	assert(!fp_fas_task_measure(a, 2, b, 2, 1, &out));
	assert(out.error == FP_FAS_TASK_INVALID_ORDER);
	b[1].tid = 2;
	b[1].cpu = 64;
	assert(!fp_fas_task_measure(a, 2, b, 2, 1, &out));
	assert(out.error == FP_FAS_TASK_INVALID_CPU && !out.top8_cpu_mask);
	b[1].cpu = 1;
	a[0].cpu = 64;
	assert(!fp_fas_task_measure(a, 2, b, 2, 1, &out));
	assert(out.error == FP_FAS_TASK_INVALID_CPU);
	assert(!fp_fas_task_measure(a, 2, b, 2, 1, NULL));
}

static void arithmetic_extremes(void)
{
	u64 largest = ~(u64)0;
	assert(fp_fas_task_util(0, largest) == 0);
	assert(fp_fas_task_util(largest, largest) == 1000);
	assert(fp_fas_task_util(largest, 1) == 1000);
	assert(fp_fas_task_util(largest - 1, largest) == 999);
	assert(fp_fas_task_util(largest / 2, largest) == 499);
	assert(fp_fas_task_util(largest / 2 + 1, largest) == 500);
	assert(fp_fas_task_util(55, 100) == 550);
	assert(fp_fas_task_util(65, 100) == 650);
	assert(fp_fas_task_util(1, 1000000) == 0);
	assert(fp_fas_task_util(1, 0) == 0);
}

static void arithmetic_reference(void)
{
	/* The test may use 128-bit arithmetic; the production helper never does. */
	u64 state = UINT64_C(0xbda7810355ac33f1);
	u32 i;

	for (i = 0; i < 20000; i++) {
		u64 delta, dt;
		u32 expected;

		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		delta = state;
		state ^= state << 13;
		state ^= state >> 7;
		state ^= state << 17;
		dt = state ? state : 1;
		expected = delta >= dt ? 1000 : (u32)(((__uint128_t)delta * 1000) / dt);
		assert(fp_fas_task_util(delta, dt) == expected);
	}
}

static void affinity_groups(void)
{
	struct fp_fas_task_load out;
	struct fp_fas_task_sample previous[8], current[8];
	u32 i;

	for (i = 0; i < 8; i++) {
		previous[i] = (struct fp_fas_task_sample)SAMPLE(i + 1, 0, 1, 0);
		current[i] = (struct fp_fas_task_sample)SAMPLE(i + 1, i, 1, 100);
		current[i].allowed_mask = 0xff;
	}
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(out.critical_mask == 0xff && out.top8_cpu_mask == 0xff);
	/* Six generic threads, two specially bound threads: keep the special set. */
	current[6].allowed_mask = current[7].allowed_mask = 0xc0;
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(out.critical_mask == 0xc0);
	/* Different last CPUs must not change the affinity-derived critical set. */
	for (i = 0; i < 8; i++) current[i].cpu = i % 2;
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(out.critical_mask == 0xc0 && out.top8_cpu_mask == 3);
	/* Equal group sizes: smaller mask is modal, so the other group survives. */
	for (i = 0; i < 8; i++) current[i].allowed_mask = i < 4 ? 0xc0 : 0x3f;
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(out.critical_mask == 0xc0);
	for (i = 0; i < 8; i++) current[i].allowed_mask = i < 4 ? 0x3f : 0xc0;
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(out.critical_mask == 0xc0);
	for (i = 0; i < 8; i++) current[i].allowed_mask = 0;
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(!out.critical_mask);
	current[7].allowed_mask = 0x40;
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(out.critical_mask == 0x40);
	/* Positive-runtime selection excludes the remaining empty/bound sleepers. */
	for (i = 0; i < 8; i++) current[i].runtime_ns = 0;
	assert(fp_fas_task_measure(previous, 8, current, 8, 1000, &out));
	assert(!out.critical_mask && !out.top8_cpu_mask && !out.top_count);
}

int main(void)
{
	basic_identity_and_migration();
	rollback_and_lifecycle();
	deterministic_top_eight();
	single_thread_load_not_cpu_sum();
	invalid_input();
	arithmetic_extremes();
	arithmetic_reference();
	affinity_groups();
	puts("FAS task load: identity, lifecycle, top8, CPU masks and overflow-safe utilization passed");
	return 0;
}
