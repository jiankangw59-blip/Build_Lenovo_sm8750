/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_POLICY_UAPI_H
#define FRAME_POLICY_UAPI_H

#include <linux/ioctl.h>
#include <linux/types.h>

#define FP_ABI 2
#define FP_MAX_CPUS 4
#define FP_MAX_MARGINS 8
#define FP_CPU 1U
#define FP_GPU 2U
#define FP_OBSERVE 4U
#define FP_FLAGS (FP_CPU | FP_GPU | FP_OBSERVE)

struct fp_limits {
	__u32 min_khz;
	__u32 max_khz;
};

struct fp_cpu_config {
	__u32 cpu;
	__u32 reserved;
	struct fp_limits scene;
	struct fp_limits game;
	struct fp_limits idle;
	__u32 margin_count;
	__u32 reserved2;
	struct {
		__u32 threshold_khz;
		__u32 percent;
	} margins[FP_MAX_MARGINS];
};

struct fp_config {
	__u32 abi;
	__u32 flags;
	__u32 cpu_count;
	__u32 target_fps;
	__u64 probe_offset;
	__u8 probe_bytes[16];
	char gpu_device[64];
	struct fp_limits gpu_scene;
	struct fp_limits gpu_game;
	struct fp_limits gpu_idle;
	struct fp_cpu_config cpus[FP_MAX_CPUS];
	__u32 reserved[8];
};

#define FP_CONFIG _IOW('F', 0x80, struct fp_config)
#define FP_DISABLE _IO('F', 0x81)

#endif
