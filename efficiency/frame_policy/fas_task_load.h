/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_FAS_TASK_LOAD_H
#define FRAME_FAS_TASK_LOAD_H

#ifdef __KERNEL__
#include <linux/types.h>
#else
#include <stdbool.h>
#include <stdint.h>
typedef uint32_t u32;
typedef uint64_t u64;
#endif

/* Both snapshots must be strictly ordered by (tid, start_time). */
struct fp_fas_task_sample {
	u32 tid, cpu;
	u64 start_time, runtime_ns;
	u64 allowed_mask;
};

enum fp_fas_task_error {
	FP_FAS_TASK_OK,
	FP_FAS_TASK_INVALID_ARGUMENT,
	FP_FAS_TASK_INVALID_INTERVAL,
	FP_FAS_TASK_INVALID_ORDER,
	FP_FAS_TASK_INVALID_CPU,
};

struct fp_fas_task_load {
	u64 top8_cpu_mask;
	u64 critical_mask;
	u32 util_permille;
	u32 measured, new_tasks, exited, rollback, top_count;
	enum fp_fas_task_error error;
	bool valid;
};

static inline int fp_fas_task_identity(const struct fp_fas_task_sample *a,
				     const struct fp_fas_task_sample *b)
{
	if (a->tid != b->tid)
		return a->tid < b->tid ? -1 : 1;
	if (a->start_time == b->start_time)
		return 0;
	return a->start_time < b->start_time ? -1 : 1;
}

/* A task cannot execute on two CPUs simultaneously. Saturate snapshot skew at
 * one CPU, and avoid overflowing delta*1000 even for the complete u64 range. */
static inline u32 fp_fas_task_util(u64 delta, u64 dt)
{
	u64 remainder = 0;
	u32 quotient = 0, bit;

	if (!dt)
		return 0;
	if (delta >= dt)
		return 1000;
	if (delta <= (~(u64)0) / 1000)
		return delta * 1000 / dt;
	/* Multiply the fraction by the ten bits of 1000. Each remainder stays
	 * below dt; comparisons avoid overflowing a sum of two u64 values. */
	for (bit = 512; bit; bit >>= 1) {
		quotient <<= 1;
		if (remainder >= dt - remainder) {
			remainder -= dt - remainder;
			quotient++;
		} else {
			remainder += remainder;
		}
		if (!(1000U & bit))
			continue;
		if (remainder >= dt - delta) {
			remainder -= dt - delta;
			quotient++;
		} else {
			remainder += delta;
		}
	}
	return quotient;
}

static inline enum fp_fas_task_error
fp_fas_task_check(const struct fp_fas_task_sample *samples, u32 count)
{
	u32 i;

	if (!samples && count)
		return FP_FAS_TASK_INVALID_ARGUMENT;
	for (i = 0; i < count; i++) {
		if (samples[i].cpu >= 64)
			return FP_FAS_TASK_INVALID_CPU;
		if (i && fp_fas_task_identity(&samples[i - 1], &samples[i]) >= 0)
			return FP_FAS_TASK_INVALID_ORDER;
	}
	return FP_FAS_TASK_OK;
}

/* Preserve the affinity grouping rule: a common background affinity is removed
 * when the hottest threads also have special affinities. Equal group counts
 * prefer the smaller numeric mask, so hash/walk order cannot change the result.
 * An empty mask is an empty set, never an implicit all-CPU fallback. */
static inline u64 fp_fas_task_critical(const struct fp_fas_task_sample *top,
				      u32 count)
{
	u64 modal = 0, critical = 0;
	u32 maximum = 0, i, j;

	for (i = 0; i < count; i++) {
		u32 matches = 0;

		for (j = 0; j < count; j++)
			matches += top[i].allowed_mask == top[j].allowed_mask;
		if (matches > maximum || (matches == maximum && top[i].allowed_mask < modal)) {
			maximum = matches;
			modal = top[i].allowed_mask;
		}
	}
	if (maximum == count)
		return modal;
	for (i = 0; i < count; i++)
		if (top[i].allowed_mask != modal)
			critical |= top[i].allowed_mask;
	return critical;
}

/* No allocation, lock, clock read or polling. Runtime rollback is excluded and
 * diagnosed; ordinary exits and PID reuse do not invalidate other identities.
 * measured includes zero-delta common identities. The diagnostic CPU mask uses
 * latest CPU and only the eight largest positive per-task runtime deltas.
 * critical_mask instead groups those tasks by latest allowed affinity.
 * Equal deltas prefer the smaller (tid,start_time), independent of input walk.
 * A valid zero load requires at least one monotonic common identity. */
static inline bool fp_fas_task_measure(const struct fp_fas_task_sample *previous,
				     u32 previous_count,
				     const struct fp_fas_task_sample *latest,
				     u32 current_count, u64 dt_ns,
				     struct fp_fas_task_load *out)
{
	struct fp_fas_task_sample top[8];
	u64 deltas[8], maximum = 0;
	u32 i = 0, j = 0, count = 0, k;

	if (!out)
		return false;
	*out = (struct fp_fas_task_load){};
	if (!dt_ns) {
		out->error = FP_FAS_TASK_INVALID_INTERVAL;
		return false;
	}
	out->error = fp_fas_task_check(previous, previous_count);
	if (out->error != FP_FAS_TASK_OK)
		return false;
	out->error = fp_fas_task_check(latest, current_count);
	if (out->error != FP_FAS_TASK_OK)
		return false;
	while (i < previous_count && j < current_count) {
		int identity = fp_fas_task_identity(&previous[i], &latest[j]);
		u64 delta;
		u32 at, move;

		if (identity < 0) {
			out->exited++;
			i++;
			continue;
		}
		if (identity > 0) {
			out->new_tasks++;
			j++;
			continue;
		}
		if (latest[j].runtime_ns < previous[i].runtime_ns) {
			out->rollback++;
			i++;
			j++;
			continue;
		}
		delta = latest[j].runtime_ns - previous[i].runtime_ns;
		out->measured++;
		if (delta > maximum)
			maximum = delta;
		if (delta) {
			for (at = 0; at < count; at++)
				if (delta > deltas[at] || (delta == deltas[at] &&
				    fp_fas_task_identity(&latest[j], &top[at]) < 0))
					break;
			if (at < 8) {
				move = count < 8 ? count : 7;
				for (; move > at; move--) {
					top[move] = top[move - 1];
					deltas[move] = deltas[move - 1];
				}
				top[at] = latest[j];
				deltas[at] = delta;
				if (count < 8)
					count++;
			}
		}
		i++;
		j++;
	}
	out->exited += previous_count - i;
	out->new_tasks += current_count - j;
	for (k = 0; k < count; k++)
		out->top8_cpu_mask |= (u64)1 << top[k].cpu;
	out->critical_mask = fp_fas_task_critical(top, count);
	out->top_count = count;
	out->util_permille = fp_fas_task_util(maximum, dt_ns);
	out->valid = out->measured != 0;
	return out->valid;
}

#endif
