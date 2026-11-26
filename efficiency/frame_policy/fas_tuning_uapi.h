/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_FAS_TUNING_UAPI_H
#define FRAME_FAS_TUNING_UAPI_H

#include "frame_policy_uapi.h"

#define FP_FAS_ABI 1U
#define FP_FAS_MODES 4U

/* FPS values use thousandths; frequency feedback uses kHz, never floating point. */
struct fp_fas_tuning {
	__u32 abi;
	__u32 mode;
	__u32 margin_base_mfps;
	__u32 override_count;
	struct {
		__u32 fps;
		__u32 margin_mfps;
	} overrides[FP_MAX_RATES];
};

#define FP_FAS_TUNING _IOW('F', 0x84, struct fp_fas_tuning)

#endif
