/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_GPU_AUX_H
#define FRAME_SCENE_GPU_AUX_H

#include <linux/kconfig.h>
#include <linux/types.h>

enum fp_scene_gpu_aux_knob {
	FP_SCENE_GPU_AUX_MIN,
	FP_SCENE_GPU_AUX_DEFAULT,
	FP_SCENE_GPU_AUX_COUNT,
};

struct fp_scene_gpu_aux;

struct fp_scene_gpu_aux_status {
	bool prepared;
	u32 target, owned_mask, conflict_mask, satisfied_mask;
	int error, knob, rollback_error;
	const char *stage;
	u64 writes, restores;
};

/* Prepare runs in the root ioctl caller and does not mutate the live state.
 * Discard is for unpublished candidates. Other calls share config_lock.
 * Commit takes the candidate after a successful release under that lock.
 * Release retries only on explicit control events and keeps failed journals.
 */
int fp_scene_gpu_aux_prepare(bool enabled, struct fp_scene_gpu_aux **candidate);
void fp_scene_gpu_aux_discard(struct fp_scene_gpu_aux *candidate);
void fp_scene_gpu_aux_commit(struct fp_scene_gpu_aux *candidate);
int fp_scene_gpu_aux_apply(bool allowed, u64 expected_generation,
		const u64 *current_generation);
int fp_scene_gpu_aux_release(void);
void fp_scene_gpu_aux_status(struct fp_scene_gpu_aux_status *out);
void fp_scene_gpu_aux_exit(void);

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
struct file;
int fp_scene_gpu_aux_test_prepare(struct file *levels, struct file *minimum,
		struct file *initial, struct fp_scene_gpu_aux **candidate);
#endif

#endif /* FRAME_SCENE_GPU_AUX_H */
