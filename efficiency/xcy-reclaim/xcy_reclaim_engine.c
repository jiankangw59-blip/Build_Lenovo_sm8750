#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/mm.h>

#include "xcy_reclaim_channel.h"
#include "xcy_reclaim_engine.h"
#include "xcy_reclaim_pss.h"
#include "xcy_reclaim_signal.h"
#include "xcy_reclaim_traffic.h"

enum xcy_engine_stage {
	XCY_ENGINE_INITIAL,
	XCY_ENGINE_PSS,
	XCY_ENGINE_FINAL,
	XCY_ENGINE_DONE,
};

struct xcy_engine_round {
	struct xcy_reclaim_plan plan;
	xcy_memory_snapshot_t initial;
	struct xcy_reclaim_targets targets;
	struct xcy_reclaim_traffic *traffic;
	struct xcy_reclaim_batch batch;
	u64 request;
	enum xcy_engine_stage stage;
};

static const struct xcy_reclaim_sources *engine_sources;

static u64 xcy_now(void)
{
	return ktime_to_ms(ktime_get_boottime());
}

static bool xcy_is_urgent(void)
{
	return xcy_reclaim_urgent(totalram_pages(), si_mem_available());
}

static struct xcy_reclaim_age xcy_age(const xcy_memory_process_t *owner, u64 now, void *context)
{
	return engine_sources->age(owner, now);
}

static void xcy_engine_clear(struct xcy_engine_round *round)
{
	xcy_reclaim_traffic_put(round->traffic);
	round->traffic = NULL;
	xcy_reclaim_targets_put(&round->targets);
	xcy_memory_snapshot_free(&round->initial);
	round->batch.count = 0;
	round->stage = XCY_ENGINE_DONE;
}

static int xcy_engine_open(struct xcy_reclaim_channel *channel, void **context)
{
	*context = xcy_snapshot_calloc(1, sizeof(struct xcy_engine_round));
	if (*context)
		engine_sources->reset();
	return *context ? 0 : -ENOMEM;
}

static void xcy_engine_close(void *context)
{
	struct xcy_engine_round *round = context;

	xcy_engine_clear(round);
	engine_sources->reset();
	xcy_snapshot_free(round);
}

static void xcy_engine_retire(void *context, u64 request)
{
	struct xcy_engine_round *round = context;

	if (round->request <= request)
		xcy_engine_clear(round);
}

static int xcy_stop_round(struct xcy_reclaim_channel *channel, struct xcy_engine_round *round,
			  enum xcy_reclaim_outcome outcome)
{
	int error = xcy_reclaim_runtime_finish(xcy_reclaim_channel_runtime(channel), round->request, outcome);

	xcy_engine_clear(round);
	return error;
}

static int xcy_continue_round(struct xcy_reclaim_channel *channel, struct xcy_engine_round *round,
			      enum xcy_engine_stage stage, char operation,
			      const struct xcy_reclaim_member *members, size_t count)
{
	int error = xcy_reclaim_channel_next(channel, round->request, operation, members, count);

	if (!error) {
		round->stage = stage;
		return 0;
	}
	xcy_stop_round(channel, round, XCY_RECLAIM_SAMPLED_RETRY);
	return error;
}

static int xcy_initial_reply(struct xcy_reclaim_channel *channel, struct xcy_engine_round *round,
			     const struct xcy_reclaim_reply *reply, xcy_memory_snapshot_t *snapshot)
{
	struct xcy_reclaim_source_view view = {0};
	int error = engine_sources->get(reply, &view);

	if (error)
		return xcy_stop_round(channel, round, XCY_RECLAIM_SAMPLED_RETRY);
	engine_sources->observe(snapshot, &view);
	error = xcy_reclaim_plan_build(&round->plan, snapshot, &view.protection, xcy_now(), xcy_age, NULL);
	engine_sources->put(&view);
	if (error || !round->plan.member_count)
		return xcy_stop_round(channel, round, XCY_RECLAIM_NO_CANDIDATE);
	error = xcy_reclaim_targets_pin(&round->plan, &round->targets);
	if (error || !round->plan.member_count)
		return xcy_stop_round(channel, round, XCY_RECLAIM_NO_CANDIDATE);
	error = xcy_reclaim_traffic_start(&round->plan, &round->traffic);
	if (error)
		return xcy_stop_round(channel, round, XCY_RECLAIM_SAMPLED_RETRY);
	round->initial = *snapshot;
	memset(snapshot, 0, sizeof(*snapshot));
	return xcy_continue_round(channel, round, XCY_ENGINE_PSS, 'P', round->plan.members,
				  round->plan.member_count);
}

static int xcy_pss_reply(struct xcy_reclaim_channel *channel, struct xcy_engine_round *round,
			 const struct xcy_reclaim_reply *reply, const xcy_memory_snapshot_t *snapshot)
{
	s64 total;
	u64 needed, urgent_above;
	size_t i;
	bool departed = reply->identity_changed;
	int error;

	for (i = 0; i < round->plan.group_count; ++i) {
		if (!xcy_reclaim_group_current(&round->plan, i, &round->targets)) {
			round->plan.groups[i].departed = true;
			departed = true;
		} else {
			round->plan.groups[i].download_busy =
				xcy_reclaim_traffic_busy(round->traffic, &round->plan, i, true);
		}
	}
	if (departed)
		xcy_reclaim_notify(XCY_RECLAIM_NOTICE_IDENTITY, 'P');
	total = totalram_pages();
	needed = (u64)xcy_system_ram_deficit(total, si_mem_available()) << (PAGE_SHIFT - 10);
	urgent_above = (u64)(xcy_system_ram_reserve(total) - xcy_reclaim_urgent_reserve(total)) << (PAGE_SHIFT - 10);
	error = xcy_reclaim_plan_pick(&round->plan, &round->initial, snapshot, xcy_now(),
				      xcy_age, NULL, needed, urgent_above, &round->batch);
	if (error || !round->batch.count)
		return xcy_stop_round(channel, round, XCY_RECLAIM_NO_CANDIDATE);
	return xcy_continue_round(channel, round, XCY_ENGINE_FINAL, 'I', NULL, 0);
}

struct xcy_final_guard {
	const struct xcy_reclaim_source_view *view;
	const struct xcy_engine_round *round;
};

static bool xcy_guard_current(void *data, size_t selected, u64 now, bool urgent)
{
	struct xcy_final_guard *guard = data;
	const struct xcy_engine_round *round = guard->round;
	const struct xcy_reclaim_group *group = &round->plan.groups[selected];
	struct xcy_reclaim_age history = engine_sources->age(&round->initial.items[group->representative], now);

	return history.background_since_ms && history.background_since_ms <= now &&
		(urgent || !history.recent) && engine_sources->valid_now(guard->view, now) &&
		!xcy_reclaim_traffic_busy(round->traffic, &round->plan, selected, false);
}

static int xcy_final_reply(struct xcy_reclaim_channel *channel, struct xcy_engine_round *round,
			   const struct xcy_reclaim_reply *reply, const xcy_memory_snapshot_t *snapshot)
{
	struct xcy_reclaim_source_view view = {0};
	struct xcy_final_guard guard = {.view = &view, .round = round};
	bool departed = reply->identity_changed;
	size_t i;
	int error;

	for (i = 0; i < round->batch.count; ++i) {
		size_t selected = round->batch.picks[i].group;

		if (!xcy_reclaim_group_current(&round->plan, selected, &round->targets)) {
			round->plan.groups[selected].departed = true;
			departed = true;
		}
	}
	error = engine_sources->get(reply, &view);
	if (error) {
		if (departed)
			xcy_reclaim_notify(XCY_RECLAIM_NOTICE_IDENTITY, 'F');
		return xcy_stop_round(channel, round, XCY_RECLAIM_SAMPLED_RETRY);
	}
	engine_sources->observe(snapshot, &view);
	error = xcy_reclaim_plan_revalidate(&round->plan, &round->batch, &round->initial,
					   snapshot, &view.protection, xcy_now(), xcy_is_urgent(),
					   xcy_age, NULL, &departed);
	if (departed)
		xcy_reclaim_notify(XCY_RECLAIM_NOTICE_IDENTITY, 'F');
	if (error || !round->batch.count) {
		engine_sources->put(&view);
		return xcy_stop_round(channel, round, XCY_RECLAIM_NO_CANDIDATE);
	}
	error = xcy_reclaim_execute(xcy_reclaim_channel_runtime(channel), round->request, &round->plan,
				    &round->targets, &round->batch, xcy_guard_current, &guard);
	for (i = 0; i < round->batch.count; ++i) {
		const struct xcy_reclaim_pick *pick = &round->batch.picks[i];
		const struct xcy_reclaim_group *group = &round->plan.groups[pick->group];

		if (pick->first_signal_ms)
			engine_sources->remember_kill(&round->initial.items[group->representative],
						      pick->first_signal_ms);
	}
	engine_sources->put(&view);
	xcy_engine_clear(round);
	return error;
}

static int xcy_engine_receive(struct xcy_reclaim_channel *channel, void *context,
			       const struct xcy_reclaim_reply *reply, xcy_memory_snapshot_t *snapshot)
{
	struct xcy_engine_round *round = context;

	if (reply->request != round->request) {
		xcy_engine_clear(round);
		round->request = reply->request;
		round->stage = XCY_ENGINE_INITIAL;
	}
	if (round->stage == XCY_ENGINE_INITIAL && reply->operation == 'I') {
		if (reply->identity_changed)
			xcy_reclaim_notify(XCY_RECLAIM_NOTICE_IDENTITY, 'I');
		return xcy_initial_reply(channel, round, reply, snapshot);
	}
	if (round->stage == XCY_ENGINE_PSS && reply->operation == 'P')
		return xcy_pss_reply(channel, round, reply, snapshot);
	if (round->stage == XCY_ENGINE_FINAL && reply->operation == 'I')
		return xcy_final_reply(channel, round, reply, snapshot);
	return -ESTALE;
}

static int xcy_engine_publish(const struct xcy_reclaim_reply *reply, const char *frame, size_t length)
{
	return engine_sources->publish(reply, frame, length);
}

static const struct xcy_reclaim_channel_ops engine_ops = {
	.open = xcy_engine_open,
	.close = xcy_engine_close,
	.retire = xcy_engine_retire,
	.publish = xcy_engine_publish,
	.receive = xcy_engine_receive,
};

int xcy_reclaim_engine_register(const struct xcy_reclaim_sources *sources)
{
	int error;

	if (!sources || !sources->reset || !sources->publish || !sources->observe ||
	    !sources->get || !sources->put || !sources->valid_now || !sources->age ||
	    !sources->remember_kill || engine_sources)
		return -EINVAL;
	engine_sources = sources;
	error = xcy_reclaim_channel_register(&engine_ops);
	if (error)
		engine_sources = NULL;
	return error;
}
