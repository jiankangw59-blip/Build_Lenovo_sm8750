#include <linux/errno.h>
#include <linux/ktime.h>
#include <linux/mm.h>
#include <linux/pid.h>
#include <linux/sched/signal.h>

#include "xcy_reclaim_identity.h"
#include "xcy_reclaim_pss.h"
#include "xcy_reclaim_signal.h"

int xcy_reclaim_targets_pin(const struct xcy_reclaim_plan *plan, size_t selected,
			     struct xcy_reclaim_targets *targets)
{
	const struct xcy_reclaim_group *group;
	size_t i;

	if (targets->pids || selected >= plan->group_count)
		return -EINVAL;
	group = &plan->groups[selected];
	if (!group->count || group->first + group->count > plan->member_count)
		return -EINVAL;
	targets->pids = xcy_snapshot_calloc(group->count, sizeof(*targets->pids));
	if (!targets->pids)
		return -ENOMEM;
	targets->selected = selected;
	for (i = 0; i < group->count; ++i) {
		const struct xcy_reclaim_member *member = &plan->members[group->first + i];
		u32 app_id = member->uid % 100000;
		struct pid *pid;

		if (app_id < 10000 || app_id > 19999)
			goto stale;
		pid = xcy_reclaim_pin_member(member);
		if (!pid)
			goto stale;
		targets->pids[targets->count++] = pid;
	}
	return 0;
stale:
	xcy_reclaim_targets_put(targets);
	return -ESTALE;
}

void xcy_reclaim_targets_put(struct xcy_reclaim_targets *targets)
{
	while (targets->count)
		put_pid(targets->pids[--targets->count]);
	xcy_snapshot_free(targets->pids);
	memset(targets, 0, sizeof(*targets));
}

struct xcy_signal_round {
	const struct xcy_reclaim_member *members;
	const struct xcy_reclaim_targets *targets;
	xcy_reclaim_guard_fn valid_guard;
	void *context;
};

static enum xcy_reclaim_outcome xcy_send_signals(void *data)
{
	struct xcy_signal_round *round = data;
	const struct xcy_reclaim_targets *targets = round->targets;
	enum xcy_reclaim_outcome outcome = XCY_RECLAIM_NO_CANDIDATE;
	size_t i;

	for (i = 0; i < targets->count; ++i) {
		s64 total = totalram_pages();
		s64 available = si_mem_available();
		u64 now = ktime_to_ms(ktime_get_boottime());

		if (xcy_system_ram_pressure_classify(total, available) != XCY_SYSTEM_PRESSURE_CRITICAL ||
		    !round->valid_guard(round->context, now, xcy_reclaim_urgent(total, available)) ||
		    !xcy_reclaim_member_current(targets->pids[i], &round->members[i]))
			break;
		if (kill_pid(targets->pids[i], SIGKILL, 1)) {
			if (outcome != XCY_RECLAIM_ACTION_SENT)
				outcome = XCY_RECLAIM_ACTION_FAILED;
			break;
		}
		outcome = XCY_RECLAIM_ACTION_SENT;
	}
	return outcome;
}

int xcy_reclaim_execute(struct xcy_reclaim_runtime *runtime, u64 request,
			const struct xcy_reclaim_plan *plan, const struct xcy_reclaim_targets *targets,
			xcy_reclaim_guard_fn valid_guard, void *context)
{
	const struct xcy_reclaim_group *group;
	struct xcy_signal_round round;

	if (!targets->pids || !valid_guard || targets->selected >= plan->group_count)
		return -EINVAL;
	group = &plan->groups[targets->selected];
	if (!group->count || targets->count != group->count ||
	    group->first + group->count > plan->member_count)
		return -EINVAL;
	round = (struct xcy_signal_round){
		.members = &plan->members[group->first], .targets = targets,
		.valid_guard = valid_guard, .context = context,
	};
	return xcy_reclaim_runtime_act(runtime, request, xcy_send_signals, &round);
}
