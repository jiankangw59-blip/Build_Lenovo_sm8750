/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_POLICY_UAPI_H
#define FRAME_SCENE_POLICY_UAPI_H

#include "frame_policy_uapi.h"

/* Optional table: the existing 592-byte FP_CONFIG ABI remains unchanged. */
#define FP_SCENE_ABI 1U
#define FP_SCENE_MAX_PROFILES 16U
#define FP_SCENE_MAX_RULES 64U
#define FP_SCENE_STATES 3U
#define FP_SCENE_ACTIVE 0U
#define FP_SCENE_INACTIVE 1U
#define FP_SCENE_OFF 2U
#define FP_SCENE_ENABLED 1U

struct fp_scene_cpu {
	struct fp_limits limits;
	__u32 margin_count; /* Zero means static limits, without PMU feedback. */
	__u32 reserved;
	struct {
		__u32 threshold_khz;
		__u32 percent;
	} margins[FP_MAX_MARGINS];
};

struct fp_scene_profile {
	__u32 flags;
	__u32 gpu_percent; /* Zero means release the GPU governor-file lease. */
	struct fp_limits gpu;
	struct fp_scene_cpu cpus[FP_MAX_CPUS];
};

struct fp_scene_rule {
	char package[128];
	__u32 profiles[FP_SCENE_STATES];
};

struct fp_scene_table {
	__u32 abi;
	__u32 count;
	__u32 cpu_count;
	__u32 profile_count;
	__u32 cpu_ids[FP_MAX_CPUS];
	__u32 defaults[FP_SCENE_STATES];
	__u32 reserved;
	struct fp_scene_profile profiles[FP_SCENE_MAX_PROFILES];
	struct fp_scene_rule rules[FP_SCENE_MAX_RULES];
};

/* 14,384 bytes, below the 14-bit ioctl-size limit on ARM64. */
#define FP_SCENE_TABLE _IOW('F', 0x83, struct fp_scene_table)

#endif
