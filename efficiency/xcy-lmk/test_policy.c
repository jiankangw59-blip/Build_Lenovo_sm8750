// SPDX-License-Identifier: GPL-2.0
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "xcy_lmk_policy.h"
#include "proc_guard_pressure_score.h"

static unsigned int cases;

static void expect(uint32_t expected, uint32_t actual)
{
	cases++;
	if (actual == expected)
		return;
	fprintf(stderr, "case %u: expected %u, got %u\n", cases, expected, actual);
	exit(1);
}

static uint32_t next_random(void)
{
	static uint32_t state = 0x7e8053f2;

	state = state * UINT32_C(1664525) + UINT32_C(1013904223);
	return state;
}

int main(void)
{
	struct xcy_lmk_candidate input[64];
	xcy_pressure_score_entry_t original[64];
	uint32_t eligible = XCY_LMK_CANDIDATE_ELIGIBLE;
	uint32_t recent = XCY_LMK_CANDIDATE_RECENT;

	_Static_assert(sizeof(struct xcy_lmk_batch) == 24, "batch ABI");
	_Static_assert(sizeof(struct xcy_lmk_task) == 40, "task ABI");
	_Static_assert(sizeof(struct xcy_lmk_candidate) == 16, "candidate ABI");

	input[0] = (struct xcy_lmk_candidate){0, 1000, eligible | recent};
	input[1] = (struct xcy_lmk_candidate){900000, 599, eligible};
	expect(XCY_LMK_NO_SELECTION, xcy_lmk_pick(input, 2, 0));
	expect(0, xcy_lmk_pick(input, 2, 1));
	input[1].pss_kib = 600;
	expect(1, xcy_lmk_pick(input, 2, 0));

	input[0] = (struct xcy_lmk_candidate){60000, 1000, eligible};
	input[1] = (struct xcy_lmk_candidate){120000, 650, eligible};
	expect(0, xcy_lmk_pick(input, 2, 0));
	input[1].background_age_ms++;
	expect(1, xcy_lmk_pick(input, 2, 0));

	input[0] = (struct xcy_lmk_candidate){0, 1000, eligible};
	input[1] = (struct xcy_lmk_candidate){240000, 756, eligible};
	expect(1, xcy_lmk_pick(input, 2, 1));
	input[1].pss_kib--;
	expect(0, xcy_lmk_pick(input, 2, 1));

	input[0] = (struct xcy_lmk_candidate){UINT64_MAX, UINT32_MAX, eligible};
	input[1] = (struct xcy_lmk_candidate){0, UINT32_MAX, eligible};
	expect(0, xcy_lmk_pick(input, 2, 1));
	input[0].flags = recent;
	input[1].pss_kib = 0;
	expect(XCY_LMK_NO_SELECTION, xcy_lmk_pick(input, 2, 1));

	for (unsigned int trial = 0; trial < 25000; trial++) {
		uint32_t count = next_random() % 65;

		for (uint32_t i = 0; i < count; i++) {
			uint32_t age = next_random();
			uint32_t pss = next_random();
			uint32_t flags = (next_random() >> 29) &
					(eligible | recent);

			input[i] = (struct xcy_lmk_candidate){age, pss, flags};
			original[i] = (xcy_pressure_score_entry_t){
				.pss_kb = pss, .age_ms = age,
				.valid = !!(flags & eligible), .recent = !!(flags & recent),
			};
		}
		for (int urgent = 0; urgent <= 1; urgent++) {
			size_t reference = xcy_pressure_score_pick_large(original, count,
									 urgent);
			uint32_t expected = reference == SIZE_MAX ?
						XCY_LMK_NO_SELECTION : (uint32_t)reference;

			expect(expected, xcy_lmk_pick(input, count, urgent));
		}
	}
	printf("%u policy replay and boundary cases passed\n", cases);
	return 0;
}
