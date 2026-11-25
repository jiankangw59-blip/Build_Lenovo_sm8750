#include <assert.h>
#include <stdio.h>

#include "xcy_reclaim_candidates.h"

static void fill(xcy_memory_process_t *row, unsigned int pid, unsigned int uid,
		 const char *package, const char *process)
{
	memset(row, 0, sizeof(*row));
	row->pid = pid;
	row->uid = uid;
	row->starttime = 123;
	row->flags = XCY_MEMORY_IDENTITY_VALID;
	row->classification = XCY_MEMORY_CLASS_THIRD_PARTY;
	row->importance = 400;
	row->procstate = 17;
	strcpy(row->pkg, package);
	strcpy(row->pkg_list, package);
	strcpy(row->process, process);
	strcpy(row->reason_component, "-");
}

int main(void)
{
	xcy_memory_process_t rows[3];
	xcy_memory_snapshot_t inventory = {.wire_version = 4, .count = 3, .items = rows};
	xcy_policy_result_t policy[3];
	struct xcy_reclaim_candidate candidate;
	struct xcy_reclaim_protection protection = {.ready = 1, .observed_ms = 100, .expires_ms = 200};
	const char *package[] = {"org.example.first"};
	const char *exact[] = {"org.example.first:helper"};
	uint32_t playback[] = {99123};

	fill(&rows[0], 10, 10000, package[0], package[0]);
	fill(&rows[1], 11, 10000, package[0], exact[0]);
	fill(&rows[2], 12, 10010, "org.example.other", "org.example.other");
	rows[1].kernel_uid_present = 1;
	rows[1].kernel_uid = playback[0];
	assert(xcy_policy_snapshot(&inventory, policy, 3));
	assert(xcy_reclaim_collect_package(&inventory, policy, &protection, 110, 0, &candidate));
	assert(candidate.uid == 10000 && candidate.count == 2 && candidate.members[1] == 1);
	protection.visible = (struct xcy_reclaim_names){package, 1};
	assert(!xcy_reclaim_collect_package(&inventory, policy, &protection, 110, 0, &candidate));
	protection.visible.count = 0;
	protection.exact_whitelist = (struct xcy_reclaim_names){exact, 1};
	assert(!xcy_reclaim_collect_package(&inventory, policy, &protection, 110, 0, &candidate));
	protection.exact_whitelist.count = 0;
	protection.playback_uids = playback;
	protection.playback_count = 1;
	assert(!xcy_reclaim_collect_package(&inventory, policy, &protection, 110, 0, &candidate));
	protection.playback_count = 0;
	protection.sync_uid = 10000;
	protection.sync_expires_ms = 140;
	assert(!xcy_reclaim_collect_package(&inventory, policy, &protection, 139, 0, &candidate));
	assert(xcy_reclaim_collect_package(&inventory, policy, &protection, 140, 0, &candidate));
	assert(!xcy_reclaim_collect_package(&inventory, policy, &protection, 200, 0, &candidate));
	strcpy(rows[2].pkg_list, "org.example.other,org.example.first");
	assert(xcy_policy_snapshot(&inventory, policy, 3));
	assert(!xcy_reclaim_collect_package(&inventory, policy, &protection, 140, 0, &candidate));
	puts("foreground, exact whitelist, isolated playback, sync lease, and ambiguous owner rejected");
	return 0;
}
