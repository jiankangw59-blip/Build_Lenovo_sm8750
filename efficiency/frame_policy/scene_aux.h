/* SPDX-License-Identifier: GPL-2.0 */
#ifndef FRAME_SCENE_AUX_H
#define FRAME_SCENE_AUX_H
#include "scene_aux_policy.h"

struct fp_aux_status {
	int profile, error, cpu, knob;
	const char *stage;
	u32 owned, conflicts, owned_mask, conflict_mask;
	u64 writes, rollbacks;
	bool loaded;
};

/* All functions are called with the parent's config_lock held. No private
 * worker, timer, spinlock, or recurring retry exists in this backend.
 * replace takes ownership of next only on success.
 */
int fp_scene_aux_replace(struct fp_scene_aux_table *next,
		const struct fp_scene_table *scene, u64 scene_epoch);
/* Two-phase publication for the combined CPU/GPU snapshot. Prepare pins and
 * validates every CPU resource before either old backend is restored.
 * Discard preserves caller ownership of next; successful commit owns next.
 */
struct aux_runtime;
int fp_scene_aux_prepare(struct fp_scene_aux_table *next, const struct fp_scene_table *scene,
		u64 scene_epoch, struct aux_runtime **candidate);
void fp_scene_aux_discard(struct aux_runtime *candidate);
int fp_scene_aux_commit(struct fp_scene_aux_table *next, struct aux_runtime *candidate);
int fp_scene_aux_clear(void);
void fp_scene_aux_exit(void);
void fp_scene_aux_apply(const char *package, bool game, bool off, bool inactive, bool allowed,
		u64 expected_generation, const u64 *current_generation);
bool fp_scene_aux_activity_needed(const char *package, bool game, bool off, bool allowed);
void fp_scene_aux_status(struct fp_aux_status *status);
const struct fp_scene_aux_profile *fp_scene_aux_current(void);
const struct fp_scene_aux_profile *fp_scene_aux_for(const char *package,
		bool game, bool off, bool inactive);
const struct fp_scene_aux_options *fp_scene_aux_options(void);
void fp_scene_aux_core_update(u32 active_clusters, bool restore,
		u64 expected_generation, const u64 *current_generation);
int fp_scene_aux_core_seed(u32 *enabled_clusters, u32 *retained);

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
struct file;
int fp_scene_aux_test_replace(struct fp_scene_aux_table *next,
		const struct fp_scene_table *scene, u64 scene_epoch,
		struct file *files[FP_MAX_CPUS][FP_SCENE_AUX_KNOBS]);
void fp_scene_aux_test_reset(void);
#endif
#endif
