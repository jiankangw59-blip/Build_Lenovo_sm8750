#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>

#include "xcy_reclaim_identity.h"
#include "xcy_reclaim_channel.h"
#include "xcy_reclaim_pss.h"
#include "xcy_reclaim_signal.h"

int xcy_reclaim_targets_pin(struct xcy_reclaim_plan *plan,
			     struct xcy_reclaim_targets *targets)
{
	size_t selected, survivors = 0;
	bool departed = false;

	if (targets->pids || !plan->member_count)
		return -EINVAL;
	targets->pids = xcy_snapshot_calloc(plan->member_count, sizeof(*targets->pids));
	if (!targets->pids)
		return -ENOMEM;
	for (selected = 0; selected < plan->group_count; ++selected) {
		struct xcy_reclaim_group group = plan->groups[selected];
		size_t before = targets->count;
		size_t i;

		for (i = 0; i < group.count; ++i) {
			const struct xcy_reclaim_member *member = &plan->members[group.first + i];
			u32 app_id = member->uid % 100000;
			struct pid *pid;

			if (app_id < 10000 || app_id > 19999)
				break;
			pid = xcy_reclaim_pin_member(member);
			if (!pid) {
				departed = true;
				break;
			}
			targets->pids[targets->count++] = pid;
		}
		if (i != group.count) {
			while (targets->count > before)
				put_pid(targets->pids[--targets->count]);
			continue;
		}
		memmove(&plan->members[before], &plan->members[group.first],
			group.count * sizeof(*plan->members));
		group.first = before;
		plan->groups[survivors++] = group;
	}
	plan->member_count = targets->count;
	plan->group_count = survivors;
	if (departed)
		xcy_reclaim_notify(XCY_RECLAIM_NOTICE_IDENTITY, 'I');
	return 0;
}

bool xcy_reclaim_group_current(const struct xcy_reclaim_plan *plan, size_t selected,
			       const struct xcy_reclaim_targets *targets)
{
	const struct xcy_reclaim_group *group;
	size_t i;

	if (!targets->pids || selected >= plan->group_count)
		return false;
	group = &plan->groups[selected];
	if (!group->count || group->departed || group->first + group->count > targets->count)
		return false;
	for (i = group->first; i < group->first + group->count; ++i)
		if (!xcy_reclaim_member_current(targets->pids[i], &plan->members[i]))
			return false;
	return true;
}

void xcy_reclaim_targets_put(struct xcy_reclaim_targets *targets)
{
	while (targets->count)
		put_pid(targets->pids[--targets->count]);
	xcy_snapshot_free(targets->pids);
	memset(targets, 0, sizeof(*targets));
}

struct xcy_signal_round {
	const struct xcy_reclaim_plan *plan;
	const struct xcy_reclaim_targets *targets;
	struct xcy_reclaim_batch *batch;
	xcy_reclaim_guard_fn valid_guard;
	void *context;
};

static enum xcy_reclaim_outcome xcy_signal_group(const struct xcy_signal_round *round,
						 struct xcy_reclaim_pick *pick, u64 expires_ms, bool urgent)
{
	const struct xcy_reclaim_group *group = &round->plan->groups[pick->group];
	const struct xcy_reclaim_member *members = &round->plan->members[group->first];
	struct pid *const *pids = &round->targets->pids[group->first];
	enum xcy_reclaim_outcome outcome = XCY_RECLAIM_NO_CANDIDATE;
	size_t i, j;

	for (i = 0; i < group->count; ++i) {
		u64 now = ktime_to_ms(ktime_get_boottime());
		size_t end = i ? i + 1 : group->count;

		if (!round->valid_guard(round->context, pick->group, now, urgent))
			break;
		/* Confirm the complete group immediately before its first action.
		 * Subsequent checks cover the remaining target without requiring
		 * already-signalled members to stay alive. */
		for (j = i; j < end; ++j)
			if (!xcy_reclaim_member_current(pids[j], &members[j]))
				break;
		if (j < end) {
			xcy_reclaim_notify(XCY_RECLAIM_NOTICE_IDENTITY, 'K');
			break;
		}
		if (ktime_to_ms(ktime_get_boottime()) >= expires_ms) {
			xcy_reclaim_notify(XCY_RECLAIM_NOTICE_TIMEOUT, 'K');
			break;
		}
		if (kill_pid(pids[i], SIGKILL, 1)) {
			if (outcome != XCY_RECLAIM_ACTION_SENT)
				outcome = XCY_RECLAIM_ACTION_FAILED;
			break;
		}
		if (!pick->first_signal_ms)
			pick->first_signal_ms = ktime_to_ms(ktime_get_boottime());
		outcome = XCY_RECLAIM_ACTION_SENT;
	}
	return outcome;
}

static enum xcy_reclaim_outcome xcy_send_signals(void *data, u64 expires_ms)
{
	struct xcy_signal_round *round = data;
	s64 total = totalram_pages(), available = si_mem_available();
	u64 urgent_above = (u64)(xcy_system_ram_reserve(total) - xcy_reclaim_urgent_reserve(total)) << (PAGE_SHIFT - 10);
	u64 needed = (u64)xcy_system_ram_deficit(total, available) << (PAGE_SHIFT - 10);
	enum xcy_reclaim_outcome outcome = XCY_RECLAIM_NO_CANDIDATE;
	size_t i;

	for (i = 0; needed && i < round->batch->count; ++i) {
		struct xcy_reclaim_pick *pick = &round->batch->picks[i];
		enum xcy_reclaim_outcome result;

		if (ktime_to_ms(ktime_get_boottime()) >= expires_ms)
			break;
		result = xcy_signal_group(round, pick, expires_ms, needed > urgent_above);
		if (result == XCY_RECLAIM_ACTION_SENT) {
			outcome = result;
			/* Charge the whole estimate even if only part of a group was signalled. */
			needed -= min(needed, pick->pss_kb);
		} else if (result == XCY_RECLAIM_ACTION_FAILED && outcome == XCY_RECLAIM_NO_CANDIDATE) {
			outcome = result;
		}
	}
	return outcome;
}

int xcy_reclaim_execute(struct xcy_reclaim_runtime *runtime, u64 request,
			const struct xcy_reclaim_plan *plan, const struct xcy_reclaim_targets *targets,
			struct xcy_reclaim_batch *batch, xcy_reclaim_guard_fn valid_guard, void *context)
{
	struct xcy_signal_round round = {
		.plan = plan, .targets = targets, .batch = batch,
		.valid_guard = valid_guard, .context = context,
	};
	size_t i;

	if (!targets->pids || !valid_guard || !batch->count || batch->count > plan->group_count)
		return -EINVAL;
	for (i = 0; i < batch->count; ++i) {
		const struct xcy_reclaim_group *group;

		if (batch->picks[i].group >= plan->group_count)
			return -EINVAL;
		group = &plan->groups[batch->picks[i].group];
		if (!group->count || group->first + group->count > targets->count)
			return -EINVAL;
	}
	return xcy_reclaim_runtime_act(runtime, request, xcy_send_signals, &round);
}
