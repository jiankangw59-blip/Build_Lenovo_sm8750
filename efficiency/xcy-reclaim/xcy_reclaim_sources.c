#include <linux/errno.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "xcy_reclaim_engine.h"
#include "xcy_reclaim_protection.h"

#define XCY_HISTORY_MAX 512U

struct xcy_payload {
	struct xcy_reclaim_protection_frame frame;
	refcount_t refs;
};

struct xcy_history_row {
	char package[XCY_MEMORY_PACKAGE_BYTES];
	u64 background_since;
	u64 last_used;
	u64 seen;
	bool visible;
};

static DEFINE_MUTEX(source_lock);
static DEFINE_XARRAY(history);
static size_t history_count;
static u64 observations;
static struct xcy_payload *latest_scene;
static struct xcy_payload *latest_protection;

static void xcy_payload_put(struct xcy_payload *payload)
{
	if (payload && refcount_dec_and_test(&payload->refs)) {
		xcy_reclaim_protection_free(&payload->frame);
		kfree(payload);
	}
}

static void xcy_clear_history(void)
{
	struct xcy_history_row *row;
	unsigned long uid;

	xa_for_each(&history, uid, row) {
		xa_erase(&history, uid);
		kfree(row);
	}
	history_count = 0;
}

static void xcy_sources_reset(void)
{
	struct xcy_payload *scene, *protection;

	mutex_lock(&source_lock);
	scene = latest_scene;
	protection = latest_protection;
	latest_scene = latest_protection = NULL;
	xcy_clear_history();
	mutex_unlock(&source_lock);
	xcy_payload_put(scene);
	xcy_payload_put(protection);
}

static bool xcy_list_has(const struct xcy_reclaim_names *list, const char *name)
{
	size_t i;

	for (i = 0; i < list->count; ++i)
		if (!strcmp(list->values[i], name))
			return true;
	return false;
}

static bool xcy_scene_has(const struct xcy_reclaim_protection *state, const char *package)
{
	return xcy_list_has(&state->visible, package) || xcy_list_has(&state->active, package) ||
		(state->current_game && !strcmp(state->current_game, package));
}

static void xcy_note_visibility(struct xcy_history_row *row, bool visible, u64 now)
{
	if (visible) {
		if (!row->visible || !row->last_used)
			row->last_used = now;
		row->background_since = 0;
	} else if (!row->background_since) {
		if (row->visible)
			row->last_used = now;
		row->background_since = now;
	}
	row->visible = visible;
}

static bool xcy_scene_fresh(const struct xcy_reclaim_protection_frame *frame, u64 now)
{
	return now >= frame->scene_observed_ms && now - frame->scene_observed_ms <= frame->scene_valid_ms;
}

static bool xcy_same_names(const struct xcy_reclaim_names *first, const struct xcy_reclaim_names *next)
{
	size_t i;

	if (first->count != next->count)
		return false;
	for (i = 0; i < first->count; ++i)
		if (strcmp(first->values[i], next->values[i]))
			return false;
	return true;
}

static int xcy_sources_publish(const struct xcy_reclaim_reply *reply, const char *data, size_t length)
{
	struct xcy_payload *payload, *old_scene = NULL, *old_protection = NULL;
	struct xcy_reclaim_protection_frame *frame, *prior;
	const char *nonce = reply ? reply->nonce : XCY_RECLAIM_SCENE_NONCE;
	u64 now = ktime_to_ms(ktime_get_boottime());
	int error;

	if (!reply && length == sizeof(XCY_RECLAIM_SCENE_LOST) - 1 &&
	    !memcmp(data, XCY_RECLAIM_SCENE_LOST, length)) {
		xcy_sources_reset();
		return 0;
	}
	if (reply && (reply->operation != 'I' || now >= reply->expires_ms))
		return -ESTALE;
	payload = kzalloc(sizeof(*payload), GFP_KERNEL);
	if (!payload)
		return -ENOMEM;
	refcount_set(&payload->refs, 1);
	frame = &payload->frame;
	error = xcy_reclaim_protection_parse(data, length, nonce, reply ? reply->published_ms : 0,
					     now, frame);
	if (error)
		goto out;
	mutex_lock(&source_lock);
	prior = latest_scene ? &latest_scene->frame : NULL;
	if (prior && (frame->scene_generation < prior->scene_generation ||
		      frame->scene_observed_ms < prior->scene_observed_ms)) {
		error = -ESTALE;
		goto unlock;
	}
	if (prior && frame->scene_generation == prior->scene_generation &&
	    (!xcy_same_names(&prior->view.visible, &frame->view.visible) ||
	     !xcy_same_names(&prior->view.active, &frame->view.active))) {
		old_scene = latest_scene;
		old_protection = latest_protection;
		latest_scene = latest_protection = NULL;
		xcy_clear_history();
		error = -ESTALE;
		goto unlock;
	}
	if (prior && (frame->scene_generation - prior->scene_generation > 1 || !xcy_scene_fresh(prior, now)))
		xcy_clear_history();
	if (!prior || frame->scene_generation != prior->scene_generation) {
		struct xcy_history_row *row;
		unsigned long uid;

		xa_for_each(&history, uid, row)
			xcy_note_visibility(row, xcy_scene_has(&frame->view, row->package), now);
	}
	old_scene = latest_scene;
	refcount_inc(&payload->refs);
	latest_scene = payload;
	if (reply) {
		old_protection = latest_protection;
		refcount_inc(&payload->refs);
		latest_protection = payload;
	}
unlock:
	mutex_unlock(&source_lock);
	xcy_payload_put(old_scene);
	xcy_payload_put(old_protection);
out:
	xcy_payload_put(payload);
	return error;
}

static bool xcy_view_current(const struct xcy_reclaim_source_view *view, u64 now)
{
	return latest_scene && latest_protection && latest_protection == view->reference &&
		latest_scene->frame.scene_generation == view->generation &&
		view->protection.observed_ms <= now && now < view->protection.expires_ms &&
		xcy_scene_fresh(&latest_scene->frame, now);
}

static int xcy_sources_get(const struct xcy_reclaim_reply *reply, struct xcy_reclaim_source_view *view)
{
	u64 now = ktime_to_ms(ktime_get_boottime());
	int error = -ESTALE;

	mutex_lock(&source_lock);
	if (!latest_protection || strcmp(latest_protection->frame.nonce, reply->nonce))
		goto out;
	view->protection = latest_protection->frame.view;
	view->generation = latest_protection->frame.scene_generation;
	view->reference = latest_protection;
	if (!xcy_view_current(view, now))
		goto out;
	refcount_inc(&latest_protection->refs);
	error = 0;
out:
	mutex_unlock(&source_lock);
	return error;
}

static void xcy_sources_put(struct xcy_reclaim_source_view *view)
{
	xcy_payload_put(view->reference);
}

static bool xcy_sources_valid(const struct xcy_reclaim_source_view *view, u64 now)
{
	bool valid;

	mutex_lock(&source_lock);
	valid = xcy_view_current(view, now);
	mutex_unlock(&source_lock);
	return valid;
}

static void xcy_sources_observe(const xcy_memory_snapshot_t *snapshot, const struct xcy_reclaim_source_view *view)
{
	u64 now = ktime_to_ms(ktime_get_boottime());
	struct xcy_history_row *row;
	unsigned long uid;
	size_t i;

	mutex_lock(&source_lock);
	if (!xcy_view_current(view, now))
		goto out;
	++observations;
	for (i = 0; i < snapshot->count; ++i) {
		const xcy_memory_process_t *process = &snapshot->items[i];
		bool visible;

		if (process->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
		    !(process->flags & XCY_MEMORY_IDENTITY_VALID))
			continue;
		row = xa_load(&history, process->uid);
		if (row && strcmp(row->package, process->pkg)) {
			xa_erase(&history, process->uid);
			kfree(row);
			--history_count;
			row = NULL;
		}
		if (!row) {
			if (history_count >= XCY_HISTORY_MAX)
				continue;
			row = kzalloc(sizeof(*row), GFP_KERNEL);
			if (!row)
				continue;
			strscpy(row->package, process->pkg, sizeof(row->package));
			if (xa_err(xa_store(&history, process->uid, row, GFP_KERNEL))) {
				kfree(row);
				continue;
			}
			++history_count;
		}
		visible = (row->seen == observations && row->visible) ||
			xcy_scene_has(&view->protection, process->pkg) ||
			(process->importance > 0 && process->importance <= 200) ||
			(process->procstate >= 0 && process->procstate <= 6);
		xcy_note_visibility(row, visible, now);
		row->seen = observations;
	}
	xa_for_each(&history, uid, row) {
		if (row->seen == observations)
			continue;
		xa_erase(&history, uid);
		kfree(row);
		--history_count;
	}
out:
	mutex_unlock(&source_lock);
}

static struct xcy_reclaim_age xcy_sources_age(const xcy_memory_process_t *owner, u64 now)
{
	struct xcy_reclaim_age age = {0};
	struct xcy_history_row *row;

	mutex_lock(&source_lock);
	if (!latest_scene || !latest_protection || !xcy_scene_fresh(&latest_scene->frame, now))
		goto out;
	row = xa_load(&history, owner->uid);
	if (!row || row->visible || strcmp(row->package, owner->pkg) ||
	    !row->background_since || row->background_since > now)
		goto out;
	age.background_since_ms = row->background_since;
	age.recent = row->last_used > now ||
		now - max(row->background_since, row->last_used) < latest_protection->frame.grace_ms;
out:
	mutex_unlock(&source_lock);
	return age;
}

static const struct xcy_reclaim_sources xcy_sources = {
	.reset = xcy_sources_reset,
	.publish = xcy_sources_publish,
	.observe = xcy_sources_observe,
	.get = xcy_sources_get,
	.put = xcy_sources_put,
	.valid_now = xcy_sources_valid,
	.age = xcy_sources_age,
};

static int __init xcy_reclaim_init(void)
{
	return xcy_reclaim_engine_register(&xcy_sources);
}
late_initcall(xcy_reclaim_init);
