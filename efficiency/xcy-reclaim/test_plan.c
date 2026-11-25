#include <assert.h>
#include <errno.h>
#include <stdio.h>

#include "xcy_reclaim_cooldown.h"
#include "xcy_reclaim_plan.h"
#include "xcy_reclaim_traffic.h"

static void test_respawn_cooldown(void)
{
	xcy_kill_cooldown_entry_t holds[2] = {0};
	const char app[] = "org.example.large";

	assert(xcy_kill_cooldown_record(holds, 2, 10000, app, 1000));
	assert(xcy_kill_cooldown_blocked(holds, 2, 10000, app, 120999, 0));
	assert(!xcy_kill_cooldown_blocked(holds, 2, 10000, app, 121000, 0));
	assert(xcy_kill_cooldown_record(holds, 2, 10000, app, 122000));
	assert(!xcy_kill_cooldown_blocked(holds, 2, 10000, app, 124000, 123000));
}

static void test_download_rate(void)
{
	const uint64_t original = 100000;

	assert(!xcy_download_rate_busy(original, original, 0));
	assert(!xcy_download_rate_busy(original, original + XCY_DOWNLOAD_RATE - 1, 1000));
	assert(xcy_download_rate_busy(original, original + XCY_DOWNLOAD_RATE, 1000));
	assert(xcy_download_rate_busy(original, original + XCY_DOWNLOAD_RATE / 2, 500));
	assert(xcy_download_rate_busy(original, original + 1, 0));
	assert(xcy_download_rate_busy(original, original - 1, 1000));
}

static void fill(xcy_memory_process_t *row, unsigned int pid, unsigned int uid,
		 const char *package, uint64_t pss)
{
	memset(row, 0, sizeof(*row));
	row->pid = pid;
	row->uid = uid;
	row->starttime = 123;
	row->flags = XCY_MEMORY_IDENTITY_VALID | XCY_MEMORY_PSS_VALID;
	row->classification = XCY_MEMORY_CLASS_THIRD_PARTY;
	row->importance = 400;
	row->procstate = 17;
	row->pss_kb = pss;
	row->rss_kb = pss;
	row->sampled_elapsed_ms = 150000;
	strcpy(row->pkg, package);
	strcpy(row->pkg_list, package);
	strcpy(row->process, package);
	strcpy(row->reason_component, "-");
}

static struct xcy_reclaim_age background(const xcy_memory_process_t *row,
					uint64_t now_ms, void *context)
{
	const int *which = context;
	return (struct xcy_reclaim_age){row->uid == 10000 ? 120000 : 10000,
					now_ms > 0 && row->uid == (unsigned int)*which};
}

int main(void)
{
	static xcy_memory_process_t initial_rows[3], measured_rows[3], final_rows[3];
	static struct xcy_reclaim_plan plan;
	static struct xcy_reclaim_batch batch;
	xcy_memory_snapshot_t initial = {.wire_version = 4, .count = 3, .items = initial_rows, .epoch = "boot"};
	xcy_memory_snapshot_t pss = {.wire_version = 4, .count = 3, .items = measured_rows,
				     .epoch = "boot", .observed_elapsed_ms = 150000};
	xcy_memory_snapshot_t final = {.wire_version = 4, .count = 3, .items = final_rows, .epoch = "boot"};
	struct xcy_reclaim_protection protection = {.ready = 1, .observed_ms = 100000, .expires_ms = 200000};
	const char *visible[] = {"org.example.older"};
	bool changed = false;
	int recent = 0;

	test_respawn_cooldown();
	test_download_rate();
	fill(&initial_rows[0], 20, 10000, "org.example.large", 800);
	fill(&initial_rows[1], 30, 10001, "org.example.older", 600);
	fill(&initial_rows[2], 21, 10000, "org.example.large", 200);
	measured_rows[0] = initial_rows[2];
	measured_rows[1] = initial_rows[1];
	measured_rows[2] = initial_rows[0];
	memcpy(final_rows, measured_rows, sizeof(final_rows));
	assert(!xcy_reclaim_plan_build(&plan, &initial, &protection, 150000, background, &recent));
	assert(plan.group_count == 2 && plan.member_count == 3);
	assert(plan.members[0].pid == 20 && plan.members[1].pid == 21);
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 0, 2000, &batch));
	assert(!batch.count);
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 600, 2000, &batch));
	assert(batch.count == 1 && batch.picks[0].group == 1 && batch.picks[0].reclaim_kb == 600);
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1100, 2000, &batch));
	assert(batch.count == 2 && batch.picks[0].group == 1 && batch.picks[1].group == 0);
	assert(batch.picks[1].reclaim_kb == 1000);
	/* High Framework PSS can come from swapped pages. Resident limits for
	 * each PID keep this group's budget well below its 1000 KiB rank. */
	measured_rows[0].rss_kb = 20;
	measured_rows[2].rss_kb = 300;
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 700, 0, &batch));
	assert(batch.count == 2 && batch.picks[0].group == 0 && batch.picks[0].reclaim_kb == 320);
	assert(batch.picks[1].group == 1 && batch.picks[1].reclaim_kb == 600);
	measured_rows[0] = initial_rows[2];
	measured_rows[2] = initial_rows[0];
	plan.groups[1].download_busy = true;
	assert(!xcy_reclaim_plan_revalidate(&plan, &batch, &initial, &final, &protection, 150010,
					    1, background, &recent, &changed));
	assert(batch.count == 1 && batch.picks[0].group == 0 && !changed);
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 2000, 0, &batch));
	assert(batch.count == 1 && batch.picks[0].group == 0);
	plan.groups[1].download_busy = false;
	/* A departed child remains in its owner's inventory, with no usable PSS. */
	xcy_memory_discard_identity(&measured_rows[0]);
	assert(measured_rows[0].uid == 10000 && !strcmp(measured_rows[0].pkg, "org.example.large"));
	assert(!measured_rows[0].pss_kb && !measured_rows[0].sampled_elapsed_ms);
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1600, 0, &batch));
	assert(batch.count == 1 && batch.picks[0].group == 1);
	measured_rows[0] = initial_rows[2];
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1600, 2000, &batch));
	assert(batch.count == 2);
	xcy_memory_discard_identity(&final_rows[0]);
	assert(!xcy_reclaim_plan_revalidate(&plan, &batch, &initial, &final, &protection, 150010,
					    1, background, &recent, &changed));
	assert(batch.count == 1 && batch.picks[0].group == 1);
	final_rows[0] = initial_rows[2];
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1600, 2000, &batch));
	protection.visible = (struct xcy_reclaim_names){visible, 1};
	assert(!xcy_reclaim_plan_revalidate(&plan, &batch, &initial, &final, &protection, 150010,
					    0, background, &recent, &changed));
	assert(batch.count == 1 && batch.picks[0].group == 0);
	protection.visible.count = 0;
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1600, 2000, &batch));
	changed = false;
	++final_rows[1].starttime;
	assert(!xcy_reclaim_plan_revalidate(&plan, &batch, &initial, &final, &protection, 150010,
					    0, background, &recent, &changed));
	assert(batch.count == 1 && batch.picks[0].group == 0 && changed);
	--final_rows[1].starttime;
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1600, 2000, &batch));
	plan.groups[1].departed = true;
	changed = false;
	assert(!xcy_reclaim_plan_revalidate(&plan, &batch, &initial, &final, &protection, 150010,
					    0, background, &recent, &changed));
	assert(batch.count == 1 && batch.picks[0].group == 0 && changed);
	plan.groups[1].departed = false;
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1600, 2000, &batch));
	recent = 10001;
	assert(!xcy_reclaim_plan_revalidate(&plan, &batch, &initial, &final, &protection, 150010,
					    0, background, &recent, &changed));
	assert(batch.count == 1 && batch.picks[0].group == 0);
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1200, 500, &batch));
	assert(batch.count == 1 && batch.picks[0].group == 0);
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 1600, 0, &batch));
	assert(batch.count == 2 && batch.picks[0].group == 0 && batch.picks[1].group == 1);
	measured_rows[0].flags = XCY_MEMORY_IDENTITY_VALID;
	assert(!xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 600, 2000, &batch));
	assert(!batch.count);
	measured_rows[0].pid = measured_rows[2].pid;
	assert(xcy_reclaim_plan_pick(&plan, &initial, &pss, 150000, background, &recent, 600, 2000, &batch) == -EINVAL);
	puts("PSS batch budget, foreground/download protection and process reuse passed");
	return 0;
}
