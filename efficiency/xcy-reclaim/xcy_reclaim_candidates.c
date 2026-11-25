#include "xcy_reclaim_candidates.h"

static int xcy_reclaim_name_present(const struct xcy_reclaim_names *list, const char *name)
{
	size_t i;

	for (i = 0; i < list->count; ++i)
		if (strcmp(list->values[i], name) == 0)
			return 1;
	return 0;
}

static int xcy_reclaim_list_contains_package(const char *packages, const char *package)
{
	size_t length = strlen(package);
	const char *position = packages;

	while (*position) {
		const char *end = strchr(position, ',');
		size_t token_length = end ? (size_t)(end - position) : strlen(position);

		if (token_length == length && memcmp(position, package, length) == 0)
			return 1;
		if (!end)
			break;
		position = end + 1;
	}
	return 0;
}

static int xcy_reclaim_protected_uid(const struct xcy_reclaim_protection *protection,
				     uint32_t uid, uint64_t now_ms)
{
	size_t i;

	if (uid == protection->sync_uid && now_ms < protection->sync_expires_ms)
		return 1;
	for (i = 0; i < protection->playback_count; ++i)
		if (protection->playback_uids[i] == uid)
			return 1;
	return 0;
}

int xcy_reclaim_collect_package(const xcy_memory_snapshot_t *inventory,
				const xcy_policy_result_t *results,
				const struct xcy_reclaim_protection *protection,
				uint64_t now_ms, size_t representative,
				struct xcy_reclaim_candidate *candidate)
{
	const xcy_memory_process_t *owner;
	const char *pkg;
	size_t count = 0, i;
	uint32_t uid;

	if (!candidate)
		return 0;
	candidate->count = 0;
	if (!inventory || !results || !protection || !protection->ready ||
	    inventory->wire_version != 4 || representative >= inventory->count ||
	    now_ms < protection->observed_ms || now_ms >= protection->expires_ms)
		return 0;
	owner = &inventory->items[representative];
	uid = owner->uid;
	pkg = owner->pkg;
	if (owner->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
	    !(owner->flags & XCY_MEMORY_IDENTITY_VALID) || !*pkg ||
	    xcy_reclaim_name_present(&protection->package_whitelist, pkg) ||
	    xcy_reclaim_name_present(&protection->full_whitelist, pkg) ||
	    xcy_reclaim_name_present(&protection->visible, pkg) ||
	    xcy_reclaim_name_present(&protection->active, pkg) ||
	    (protection->current_game && strcmp(protection->current_game, pkg) == 0) ||
	    xcy_reclaim_protected_uid(protection, uid, now_ms) ||
	    !xcy_policy_kill_package_results(inventory, results, pkg, uid))
		return 0;

	for (i = 0; i < inventory->count; ++i) {
		const xcy_memory_process_t *member = &inventory->items[i];

		if (member->uid != uid && (member->uid / 100000 != uid / 100000 ||
		    !xcy_reclaim_list_contains_package(member->pkg_list, pkg)))
			continue;
		if (member->uid != uid || strcmp(member->pkg, pkg) ||
		    strcmp(member->pkg_list, pkg) || !*member->process ||
		    strcmp(member->process, "-") == 0 ||
		    member->classification != XCY_MEMORY_CLASS_THIRD_PARTY ||
		    !(member->flags & XCY_MEMORY_IDENTITY_VALID) ||
		    xcy_reclaim_name_present(&protection->exact_whitelist, member->process) ||
		    xcy_reclaim_protected_uid(protection, xcy_memory_kernel_uid(member), now_ms) ||
		    count == XCY_RECLAIM_MEMBERS_MAX)
			return 0;
		candidate->members[count++] = i;
	}
	candidate->uid = uid;
	candidate->representative = representative;
	candidate->count = count;
	return count > 0;
}
