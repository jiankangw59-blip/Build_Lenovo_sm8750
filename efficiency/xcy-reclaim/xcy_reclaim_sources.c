#include <linux/errno.h>
#include <linux/init.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/refcount.h>
#include <linux/slab.h>
#include <linux/xarray.h>

#include "xcy_reclaim_cooldown.h"
#include "xcy_reclaim_engine.h"
#include "xcy_reclaim_policy.h"
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
	bool scene_visible;
};

static DEFINE_MUTEX(source_lock);
static DEFINE_XARRAY(history);
static xcy_kill_cooldown_entry_t cooldown[128];
static size_t history_count;
static u64 observations;
static u64 latest_grace_ms;
static u64 scene_observed_ms;
static u64 scene_valid_ms;
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
	struct xcy_payload *protection;

	mutex_lock(&source_lock);
	protection = latest_protection;
	latest_protection = NULL;
	scene_observed_ms = 0;
	scene_valid_ms = 0;
	latest_grace_ms = 0;
	memset(cooldown, 0, sizeof(cooldown));
	xcy_clear_history();
	mutex_unlock(&source_lock);
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

static bool xcy_scene_fresh(u64 now)
{
	return scene_observed_ms && now >= scene_observed_ms && now - scene_observed_ms <= scene_valid_ms;
}

static void xcy_scene_changed(const struct xcy_reclaim_protection_frame *frame)
{
	u64 now = ktime_to_ms(ktime_get_boottime());
	struct xcy_history_row *row;
	unsigned long uid;

	mutex_lock(&source_lock);
	if (!frame || !xcy_scene_fresh(now))
		xcy_clear_history();
	if (frame) {
		xa_for_each(&history, uid, row) {
			bool visible = xcy_scene_has(&frame->view, row->package);

			if (row->scene_visible != visible)
				xcy_note_visibility(row, visible, now);
			row->scene_visible = visible;
		}
		scene_observed_ms = frame->scene_observed_ms;
		scene_valid_ms = frame->scene_valid_ms;
	} else {
		scene_observed_ms = 0;
		scene_valid_ms = 0;
	}
	mutex_unlock(&source_lock);
}

static int xcy_sources_publish(const struct xcy_reclaim_reply *reply, const char *data, size_t length)
{
	struct xcy_payload *payload = NULL, *old;
	u64 now = ktime_to_ms(ktime_get_boottime());
	int error;

	if (reply) {
		if (reply->operation != 'I' || now >= reply->expires_ms)
			return -ESTALE;
		payload = kzalloc(sizeof(*payload), GFP_KERNEL);
		if (!payload)
			return -ENOMEM;
		refcount_set(&payload->refs, 1);
		error = xcy_reclaim_audio_parse(data, length, reply->nonce, reply->published_ms, now, &payload->frame);
		if (error) {
			xcy_payload_put(payload);
			return error;
		}
	} else if (length != sizeof(XCY_RECLAIM_AUDIO_LOST) - 1 || memcmp(data, XCY_RECLAIM_AUDIO_LOST, length)) {
		return -EINVAL;
	}
	mutex_lock(&source_lock);
	old = latest_protection;
	latest_protection = payload;
	mutex_unlock(&source_lock);
	xcy_payload_put(old);
	return 0;
}

static bool xcy_view_current(const struct xcy_reclaim_source_view *view, u64 now)
{
	return latest_protection && latest_protection == view->reference &&
		view->protection.observed_ms <= now && now < view->protection.expires_ms;
}

static int xcy_sources_get(const struct xcy_reclaim_reply *reply, struct xcy_reclaim_source_view *view)
{
	u64 now = ktime_to_ms(ktime_get_boottime());
	int error = -ESTALE;

	mutex_lock(&source_lock);
	if (!latest_protection || strcmp(latest_protection->frame.nonce, reply->nonce))
		goto out;
	view->protection = latest_protection->frame.view;
	view->reference = latest_protection;
	if (!xcy_view_current(view, now))
		goto out;
	refcount_inc(&latest_protection->refs);
	error = 0;
out:
	mutex_unlock(&source_lock);
	if (error)
		return error;
	error = xcy_reclaim_policy_get(&view->protection, &view->grace_ms, &view->policy);
	if (error) {
		xcy_reclaim_policy_put(view->policy);
		xcy_payload_put(view->reference);
		memset(view, 0, sizeof(*view));
	}
	return error;
}

static void xcy_sources_put(struct xcy_reclaim_source_view *view)
{
	xcy_reclaim_policy_put(view->policy);
	xcy_payload_put(view->reference);
}

static bool xcy_sources_valid(const struct xcy_reclaim_source_view *view, u64 now)
{
	bool valid;

	if (!xcy_reclaim_policy_current(view->policy))
		return false;
	now = ktime_to_ms(ktime_get_boottime());
	mutex_lock(&source_lock);
	valid = xcy_view_current(view, now) && xcy_scene_fresh(now);
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
	latest_grace_ms = view->grace_ms;
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
		row->scene_visible = xcy_scene_has(&view->protection, process->pkg);
		visible = (row->seen == observations && row->visible) || row->scene_visible ||
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
	if (!xcy_scene_fresh(now) || !latest_grace_ms)
		goto out;
	row = xa_load(&history, owner->uid);
	if (!row || row->visible || strcmp(row->package, owner->pkg) ||
	    !row->background_since || row->background_since > now)
		goto out;
	if (xcy_kill_cooldown_blocked(cooldown, ARRAY_SIZE(cooldown), owner->uid, owner->pkg, now, row->last_used))
		goto out;
	age.background_since_ms = row->background_since;
	age.recent = row->last_used > now ||
		now - max(row->background_since, row->last_used) < latest_grace_ms;
out:
	mutex_unlock(&source_lock);
	return age;
}

static void xcy_sources_remember_kill(const xcy_memory_process_t *owner, u64 now)
{
	mutex_lock(&source_lock);
	xcy_kill_cooldown_record(cooldown, ARRAY_SIZE(cooldown), owner->uid, owner->pkg, now);
	mutex_unlock(&source_lock);
}

static const struct xcy_reclaim_sources xcy_sources = {
	.reset = xcy_sources_reset,
	.publish = xcy_sources_publish,
	.observe = xcy_sources_observe,
	.get = xcy_sources_get,
	.put = xcy_sources_put,
	.valid_now = xcy_sources_valid,
	.age = xcy_sources_age,
	.remember_kill = xcy_sources_remember_kill,
};

static int __init xcy_reclaim_init(void)
{
	int error = xcy_reclaim_policy_register(xcy_scene_changed);

	if (error)
		return error;
	error = xcy_reclaim_engine_register(&xcy_sources);
	if (error)
		xcy_reclaim_policy_unregister();
	return error;
}
late_initcall(xcy_reclaim_init);
