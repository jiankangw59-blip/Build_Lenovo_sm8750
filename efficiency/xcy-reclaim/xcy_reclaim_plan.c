#ifdef __KERNEL__
#include <linux/bsearch.h>
#include <linux/errno.h>
#include <linux/sort.h>
#else
#include <errno.h>
#endif

#include "xcy_reclaim_plan.h"

struct xcy_plan_key {
	uint32_t key;
	uint32_t row;
};

static int compare_keys(const void *a, const void *b)
{
	const struct xcy_plan_key *left = a, *right = b;

	if (left->key != right->key)
		return (left->key > right->key) - (left->key < right->key);
	return 0;
}

static void sort_keys(struct xcy_plan_key *keys, size_t count)
{
#ifdef __KERNEL__
	sort(keys, count, sizeof(*keys), compare_keys, NULL);
#else
	qsort(keys, count, sizeof(*keys), compare_keys);
#endif
}

static int valid_frame(const xcy_memory_snapshot_t *frame)
{
	return frame && frame->wire_version == 4 && frame->items && frame->count &&
		frame->count <= XCY_MEMORY_MAX_PROCESSES;
}

static int same_member(const struct xcy_reclaim_member *identity,
		       const xcy_memory_process_t *row)
{
	return identity->pid == row->pid && identity->uid == row->uid &&
		identity->kernel_uid == xcy_memory_kernel_uid(row) &&
		identity->starttime == row->starttime &&
		(row->flags & XCY_MEMORY_IDENTITY_VALID);
}

int xcy_reclaim_plan_build(struct xcy_reclaim_plan *plan,
			   const xcy_memory_snapshot_t *inventory,
			   const struct xcy_reclaim_protection *protection, uint64_t now_ms,
			   xcy_reclaim_age_fn age, void *context)
{
	xcy_policy_result_t *policy;
	struct xcy_reclaim_candidate *candidate;
	struct xcy_plan_key *keys;
	uint8_t *first;
	size_t i, j;
	int error = -ENOMEM;

	memset(plan, 0, sizeof(*plan));
	if (!valid_frame(inventory) || !age)
		return -EINVAL;
	candidate = xcy_snapshot_calloc(1, sizeof(*candidate));
	policy = xcy_snapshot_calloc(inventory->count, sizeof(*policy));
	keys = xcy_snapshot_calloc(inventory->count, sizeof(*keys));
	first = xcy_snapshot_calloc(inventory->count, sizeof(*first));
	if (!candidate || !policy || !keys || !first)
		goto out;
	if (!xcy_policy_snapshot(inventory, policy, inventory->count)) {
		error = -EINVAL;
		goto out;
	}
	for (i = 0; i < inventory->count; ++i)
		keys[i] = (struct xcy_plan_key){inventory->items[i].uid, i};
	sort_keys(keys, inventory->count);
	for (i = 0; i < inventory->count; i = j) {
		uint32_t minimum = keys[i].row;

		for (j = i + 1; j < inventory->count && keys[j].key == keys[i].key; ++j)
			if (keys[j].row < minimum)
				minimum = keys[j].row;
		first[minimum] = 1;
	}
	for (i = 0; i < inventory->count; ++i) {
		const xcy_memory_process_t *row = &inventory->items[i];
		struct xcy_reclaim_group *group;
		struct xcy_reclaim_age observed;

		if (!first[i] || !xcy_reclaim_collect_package(inventory, policy, protection,
							     now_ms, i, candidate))
			continue;
		observed = age(row, now_ms, context);
		if (!observed.background_since_ms || observed.background_since_ms > now_ms)
			continue;
		/* Truncation would change the size pool and may change the winner. */
		if (candidate->count > XCY_RECLAIM_MEMBERS_MAX - plan->member_count) {
			error = -E2BIG;
			goto out;
		}
		group = &plan->groups[plan->group_count++];
		group->representative = i;
		group->first = plan->member_count;
		group->count = candidate->count;
		for (j = 0; j < candidate->count; ++j) {
			const xcy_memory_process_t *member = &inventory->items[candidate->members[j]];

			plan->members[plan->member_count++] = (struct xcy_reclaim_member){
				.pid = member->pid, .uid = member->uid,
				.kernel_uid = xcy_memory_kernel_uid(member), .starttime = member->starttime,
			};
		}
	}
	error = 0;
out:
	xcy_snapshot_free(first);
	xcy_snapshot_free(keys);
	xcy_snapshot_free(policy);
	xcy_snapshot_free(candidate);
	if (error)
		plan->group_count = plan->member_count = 0;
	return error;
}

static uint64_t group_pss(const struct xcy_reclaim_plan *plan,
			  const struct xcy_reclaim_group *group,
			  const xcy_memory_snapshot_t *pss, const struct xcy_plan_key *keys,
			  const char *package)
{
	uint64_t total = 0;
	size_t i;

	for (i = 0; i < group->count; ++i) {
		const struct xcy_reclaim_member *member = &plan->members[group->first + i];
		struct xcy_plan_key wanted = {.key = member->pid};
		const struct xcy_plan_key *found = bsearch(&wanted, keys, pss->count,
							   sizeof(*keys), compare_keys);
		const xcy_memory_process_t *row;

		if (!found)
			return 0;
		row = &pss->items[found->row];
		if (!same_member(member, row) ||
		    row->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
		    !(row->flags & XCY_MEMORY_PSS_VALID) || strcmp(row->pkg, package) ||
		    strcmp(row->pkg_list, package) || !row->pss_kb || !row->sampled_elapsed_ms ||
		    row->sampled_elapsed_ms > pss->observed_elapsed_ms ||
		    row->pss_kb > (uint64_t)XCY_RECLAIM_INT64_MAX - total)
			return 0;
		total += row->pss_kb;
	}
	return total;
}

int xcy_reclaim_plan_pick(const struct xcy_reclaim_plan *plan,
			  const xcy_memory_snapshot_t *inventory,
			  const xcy_memory_snapshot_t *pss, uint64_t now_ms, int urgent,
			  xcy_reclaim_age_fn age, void *context, size_t *selected)
{
	struct xcy_reclaim_rank_entry *rank;
	struct xcy_plan_key *keys;
	size_t i;
	int error = -ENOMEM;

	*selected = SIZE_MAX;
	if (!valid_frame(inventory) || !valid_frame(pss) || !age ||
	    strcmp(inventory->epoch, pss->epoch) || pss->count > plan->member_count)
		return -EINVAL;
	rank = xcy_snapshot_calloc(plan->group_count, sizeof(*rank));
	keys = xcy_snapshot_calloc(pss->count, sizeof(*keys));
	if (!rank || !keys)
		goto out;
	for (i = 0; i < pss->count; ++i)
		keys[i] = (struct xcy_plan_key){pss->items[i].pid, i};
	sort_keys(keys, pss->count);
	for (i = 1; i < pss->count; ++i)
		if (keys[i - 1].key == keys[i].key) {
			error = -EINVAL;
			goto out;
		}
	for (i = 0; i < plan->group_count; ++i) {
		const struct xcy_reclaim_group *group = &plan->groups[i];
		const xcy_memory_process_t *row = &inventory->items[group->representative];
		struct xcy_reclaim_age observed;

		if (group->departed || group->download_busy)
			continue;
		observed = age(row, now_ms, context);
		if (!observed.background_since_ms || observed.background_since_ms > now_ms)
			continue;
		rank[i].pss_kb = group_pss(plan, group, pss, keys, row->pkg);
		rank[i].valid = !!rank[i].pss_kb;
		rank[i].age_ms = now_ms - observed.background_since_ms;
		rank[i].recent = observed.recent;
	}
	*selected = xcy_reclaim_rank(rank, plan->group_count, urgent);
	error = 0;
out:
	xcy_snapshot_free(keys);
	xcy_snapshot_free(rank);
	return error;
}

int xcy_reclaim_plan_revalidate(const struct xcy_reclaim_plan *plan, size_t selected,
			       const xcy_memory_snapshot_t *first, const xcy_memory_snapshot_t *fresh,
			       const struct xcy_reclaim_protection *protection, uint64_t now_ms,
			       int urgent, xcy_reclaim_age_fn age, void *context)
{
	const struct xcy_reclaim_group *group;
	const xcy_memory_process_t *owner;
	struct xcy_reclaim_candidate *candidate;
	struct xcy_reclaim_age observed;
	xcy_policy_result_t *policy;
	size_t i, j;
	int error = -ESTALE;

	if (selected >= plan->group_count || !valid_frame(first) || !valid_frame(fresh) || !age ||
	    strcmp(first->epoch, fresh->epoch))
		return -ESTALE;
	group = &plan->groups[selected];
	if (group->departed)
		return -ESRCH;
	if (group->download_busy)
		return -ESTALE;
	owner = &first->items[group->representative];
	for (i = 0; i < fresh->count; ++i)
		if (fresh->items[i].uid == owner->uid)
			break;
	if (i == fresh->count || strcmp(fresh->items[i].pkg, owner->pkg))
		return -ESRCH;
	observed = age(&fresh->items[i], now_ms, context);
	if (!observed.background_since_ms || observed.background_since_ms > now_ms ||
	    (!urgent && observed.recent))
		return -ESTALE;
	candidate = xcy_snapshot_calloc(1, sizeof(*candidate));
	policy = xcy_snapshot_calloc(fresh->count, sizeof(*policy));
	if (!candidate || !policy) {
		error = -ENOMEM;
		goto out;
	}
	if (!xcy_policy_snapshot(fresh, policy, fresh->count) ||
	    !xcy_reclaim_collect_package(fresh, policy, protection, now_ms, i, candidate))
		goto out;
	if (candidate->count != group->count) {
		error = -ESRCH;
		goto out;
	}
	for (i = 0; i < candidate->count; ++i) {
		const xcy_memory_process_t *row = &fresh->items[candidate->members[i]];

		for (j = 0; j < group->count; ++j)
			if (same_member(&plan->members[group->first + j], row))
				break;
		if (j == group->count) {
			error = -ESRCH;
			goto out;
		}
	}
	error = 0;
out:
	xcy_snapshot_free(policy);
	xcy_snapshot_free(candidate);
	return error;
}
