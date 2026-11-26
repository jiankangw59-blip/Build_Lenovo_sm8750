/* SPDX-License-Identifier: GPL-2.0 */
#include <assert.h>
#include <stdio.h>
#include "fas_frames.h"

int main(void)
{
	struct fp_fas_history history = {0};
	struct fp_fas_events queue = {0};
	struct fp_fas_event event;
	u64 sum = 0;
	unsigned int i;
	const struct fp_fas_tuning balance = { .abi = FP_FAS_ABI, .mode = 1, .margin_base_mfps = 1000 };
	const struct fp_fas_tuning fast = { .abi = FP_FAS_ABI, .mode = 3 };

	for (i = 0; i < 300; i++) {
		assert(fp_fas_history_push(&history, 16666667, 60));
		sum += 16666667;
	}
	assert(history.sum_ns == sum && history.count == 300);
	assert(!fp_fas_history_janked(&history, 60));
	assert(!fp_fas_history_janked_elapsed(&history, 60, 50000000));
	assert(fp_fas_history_janked_elapsed(&history, 60, 400000000));
	assert(history.sum_ns == sum && history.count == 300);
	assert(fp_fas_history_push(&history, 400000000, 60));
	assert(history.count == 300 && fp_fas_history_janked(&history, 60));
	assert(fp_fas_history_push(&history, 8333333, 30));
	assert(history.count == 150); /* Window shrinks without mixing up its sum. */
	assert(!fp_fas_history_push(&history, 1000000000ULL, 60));
	assert(!history.count && !history.sum_ns);
	for (i = 0; i < FP_FAS_EVENTS; i++)
		assert(fp_fas_event_push(&queue, (struct fp_fas_event){
			.ended_ns = i, .interval_ns = 16666667, .fps = 60,
		}));
	assert(!fp_fas_event_push(&queue, (struct fp_fas_event){0}));
	for (i = 0; i < FP_FAS_EVENTS; i++) {
		assert(fp_fas_event_pop(&queue, &event));
		assert(event.ended_ns == i);
	}
	assert(!fp_fas_event_pop(&queue, &event));
	for (i = 0; i < 10000; i++) {
		assert(fp_fas_event_push(&queue, (struct fp_fas_event){
			.ended_ns = i, .interval_ns = 8333333, .fps = 120,
		}));
		assert(fp_fas_event_pop(&queue, &event) && event.ended_ns == i);
	}
	assert(fp_fas_event_push(&queue, (struct fp_fas_event){
		.ended_ns = 1, .interval_ns = 8333333, .fps = 120,
		.margin_mfps = fp_fas_margin(&balance, 120),
	}));
	assert(fp_fas_event_push(&queue, (struct fp_fas_event){
		.ended_ns = 2, .interval_ns = 8333333, .fps = 120,
		.margin_mfps = fp_fas_margin(&fast, 120),
	}));
	assert(fp_fas_event_pop(&queue, &event) && event.margin_mfps == 2000);
	assert(fp_fas_event_pop(&queue, &event) && !event.margin_mfps);
	puts("FAS history: long-frame recovery, five-second window and ordered loss-detecting batching passed");
	return 0;
}
