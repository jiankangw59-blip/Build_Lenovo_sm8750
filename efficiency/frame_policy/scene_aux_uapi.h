/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_AUX_UAPI_H
#define FRAME_SCENE_AUX_UAPI_H

#include "scene_policy_uapi.h"

#define FP_SCENE_AUX_ABI 1U
#define FP_SCENE_AUX_MAX_PROFILES 32U
#define FP_SCENE_AUX_KNOBS 7U
#define FP_SCENE_AUX_SIZE 26584U
#define FP_SCENE_AUX_MAX_BYTES 65536U

#define FP_AUX_UP_RATE (1U << 0)
#define FP_AUX_DOWN_RATE (1U << 1)
#define FP_AUX_HISPEED (1U << 2)
#define FP_AUX_BOOST (1U << 3)
#define FP_AUX_CORE_CTL (1U << 4)
#define FP_AUX_DIRECT_MASK (FP_AUX_UP_RATE | FP_AUX_DOWN_RATE | FP_AUX_HISPEED | \
			    FP_AUX_BOOST | FP_AUX_CORE_CTL)

#define FP_AUX_FEEDBACK_EXCLUDES (1U << 0)
#define FP_AUX_FEEDBACK_PREFER (1U << 1)
#define FP_AUX_FEEDBACK_CORE_CTL (1U << 2)
#define FP_AUX_FEEDBACK_PERFECT (1U << 3)
#define FP_AUX_FEEDBACK_SMOOTHNESS (1U << 4)
#define FP_AUX_FEEDBACK_MT (1U << 5)
#define FP_AUX_FEEDBACK_ADDITIVE (1U << 6)
#define FP_AUX_FEEDBACK_MASK (FP_AUX_FEEDBACK_EXCLUDES | FP_AUX_FEEDBACK_PREFER | \
			      FP_AUX_FEEDBACK_CORE_CTL | FP_AUX_FEEDBACK_PERFECT | \
			      FP_AUX_FEEDBACK_SMOOTHNESS | FP_AUX_FEEDBACK_MT | FP_AUX_FEEDBACK_ADDITIVE)

#define FP_AUX_OPT_SOURCE (1U << 0)
#define FP_AUX_OPT_STAT_SOURCE (1U << 1)
#define FP_AUX_OPT_GESTURE (1U << 2)
#define FP_AUX_OPT_GESTURE_DURATION (1U << 3)
#define FP_AUX_OPT_LAUNCHER (1U << 4)
#define FP_AUX_OPT_PERF_NOTIFY (1U << 5)
#define FP_AUX_OPT_PERF_IN_APPS (1U << 6)
#define FP_AUX_OPT_PERF_PRESETS (1U << 7)
#define FP_AUX_OPT_KSWAPD (1U << 8)
#define FP_AUX_OPT_PANEL_PEAK (1U << 9)
#define FP_AUX_OPT_GPU_LOCK (1U << 10)
#define FP_AUX_OPTIONS_MASK ((1U << 11) - 1)

/* Exact original context provenance. Unknown is deliberately not ONLINE. */
#define FP_AUX_SOURCE_OTHER 0U
#define FP_AUX_SOURCE_ONLINE 1U
#define FP_AUX_SOURCE_CUSTOM 2U

struct fp_scene_aux_options {
	__u32 valid_mask;
	__u32 source;
	__u32 stat_source;
	__u32 gesture_boost;
	__u32 gesture_duration_ms;
	__u32 launcher_boost;
	__u32 perf_notify;
	__u32 perf_notify_in_apps;
	__u32 perf_use_presets;
	__u32 kswapd_boost;
	/* Verified physical panel capability, not current or per-app refresh. */
	__u32 panel_peak_hz;
	__u32 gpu_lock;
};

struct fp_scene_aux_additive {
	__u32 threshold_khz;
	__s32 offset_khz;
};

enum fp_scene_aux_knob {
	FP_AUX_KNOB_UP_RATE,
	FP_AUX_KNOB_DOWN_RATE,
	FP_AUX_KNOB_HISPEED,
	FP_AUX_KNOB_BOOST,
	FP_AUX_KNOB_CORE_ENABLE,
	FP_AUX_KNOB_CORE_MIN,
	FP_AUX_KNOB_CORE_MAX,
};

struct fp_scene_aux_cpu {
	__u32 valid_mask;
	__u32 up_us;
	__u32 down_us;
	__u32 hispeed_khz;
	__s32 walt_boost;
	__u32 core_enable;
	__u32 min_cpus;
	__u32 max_cpus;
	__u64 excludes_mask;
	__u32 prefer;
	/* Ordered original cluster indices, one byte per entry; unused bytes zero.
	 * Scene's core_ctl [0,1] is a cluster list, never CPU min/max counts.
	 * The verified Sun feedback adapter consumes this ordered cluster list.
	 */
	__u32 limiter_cluster_count;
	__u32 limiter_clusters_packed;
	__u32 feedback_mask;
	/* Native values, independent of the legacy frequency table's percentages.
	 * Perfect0 and smoothness0 select the proven original defaults.
	 * MT uses thousandths of one percent: 100000 means100 percent.
	 */
	__u32 perfect_khz;
	__u32 smoothness;
	__u32 mt_milli_percent;
	__u32 additive_count;
	struct fp_scene_aux_additive additive[FP_MAX_MARGINS];
};

struct fp_scene_aux_profile {
	struct fp_scene_aux_cpu cpus[FP_MAX_CPUS];
};

struct fp_scene_aux_table {
	__u32 abi;
	__u32 count;
	__u32 cpu_count;
	__u32 profile_count;
	__u32 cpu_ids[FP_MAX_CPUS];
	__u32 defaults[FP_SCENE_STATES];
	__u32 reserved;
	/* Disk: zero/-1. The client fills this snapshot's live table epoch/FDs. */
	__u64 expected_scene_epoch;
	__s32 fds[FP_MAX_CPUS][FP_SCENE_AUX_KNOBS];
	struct fp_scene_aux_options options;
	struct fp_scene_aux_profile profiles[FP_SCENE_AUX_MAX_PROFILES];
	struct fp_scene_rule rules[FP_SCENE_MAX_RULES];
};

/* Fixed descriptor keeps typed policy growth independent of ioctl size bits.
 * flags/reserved must be zero; length must exactly match the typed ABI blob.
 */
struct fp_scene_aux_request {
	__u32 abi;
	__u32 flags;
	__u32 length;
	__u32 reserved;
	__u64 user_ptr;
};

/* No existing FP_ABI/FP_SCENE_ABI structure or ioctl is changed. */
#define FP_SCENE_AUX _IOW('F', 0x85, struct fp_scene_aux_request)

_Static_assert(sizeof(struct fp_scene_aux_cpu) == 136, "Scene aux CPU ABI");
_Static_assert(sizeof(struct fp_scene_aux_options) == 48, "Scene aux global options ABI");
_Static_assert(__builtin_offsetof(struct fp_scene_aux_options, gpu_lock) == 44, "Scene aux GPU option offset");
_Static_assert(__builtin_offsetof(struct fp_scene_aux_table, fds) == 56, "Scene aux FD offset");
_Static_assert(__builtin_offsetof(struct fp_scene_aux_table, options) == 168, "Scene aux options offset");
_Static_assert(__builtin_offsetof(struct fp_scene_aux_table, profiles) == 216, "Scene aux profile offset");
_Static_assert(__builtin_offsetof(struct fp_scene_aux_table, rules) == 17624, "Scene aux rule offset");
_Static_assert(sizeof(struct fp_scene_aux_table) == FP_SCENE_AUX_SIZE, "Scene aux table ABI");
_Static_assert(sizeof(struct fp_scene_aux_request) == 24, "Scene aux request ABI");
_Static_assert(_IOC_SIZE(FP_SCENE_AUX) == 24, "Scene aux ioctl size");
_Static_assert(FP_SCENE_AUX_SIZE <= FP_SCENE_AUX_MAX_BYTES, "Scene aux policy cap");

#endif
