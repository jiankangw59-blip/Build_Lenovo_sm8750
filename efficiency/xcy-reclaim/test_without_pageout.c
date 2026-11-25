#include <assert.h>
#include <stdio.h>

#include "xcy_reclaim_state.h"

int main(void)
{
	xcy_system_reclaim_episode_t episode = {0};
	xcy_system_reclaim_episode_t after_kill;
	xcy_system_pressure_level_t stress = xcy_system_ram_pressure_classify(10000, 1200);
	xcy_system_pressure_level_t critical = xcy_system_ram_pressure_classify(10000, 1199);
	xcy_system_pressure_level_t healthy = xcy_system_ram_pressure_classify(10000, 3000);
	xcy_reclaim_s64 now = 2000, next_round;
	unsigned int action;

	assert(stress == XCY_SYSTEM_PRESSURE_STRESS);
	assert(critical == XCY_SYSTEM_PRESSURE_CRITICAL);
	assert(healthy == XCY_SYSTEM_PRESSURE_HEALTHY);
	assert(xcy_system_ram_deficit(10001, 1200) == 1);
	assert(xcy_system_ram_deficit(10001, 1201) == 0);
	assert(xcy_system_ram_deficit(10000, 900) == 300);
	assert(xcy_system_ram_deficit(10000, -1) == 0);
	assert(xcy_reclaim_observe_without_pageout(&episode, stress, 1000) == XCY_SYSTEM_RECLAIM_NONE);
	assert(xcy_system_reclaim_deadline(&episode) == 0);
	assert(episode.critical_reclaim_rounds == 0);
	for (action = 0; action < 4; ++action) {
		assert(xcy_reclaim_observe_without_pageout(&episode, critical, now) == XCY_SYSTEM_RECLAIM_READY);
		assert(episode.phase == XCY_SYSTEM_RECLAIM_ACTION_PENDING);
		assert(xcy_reclaim_complete_batch(&episode, ++now) == XCY_SYSTEM_RECLAIM_WAIT);
		now += XCY_SYSTEM_RECLAIM_OBSERVE_MS;
		assert(xcy_system_reclaim_deadline(&episode) == now);
		assert(xcy_reclaim_observe_without_pageout(&episode, critical, now - 1) == XCY_SYSTEM_RECLAIM_WAIT);
		assert(episode.critical_reclaim_rounds == 0);
	}
	after_kill = episode;

	assert(xcy_reclaim_observe_without_pageout(&episode, stress, now) == XCY_SYSTEM_RECLAIM_NONE);
	assert(xcy_system_reclaim_deadline(&episode) == 0);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, now + 1) == XCY_SYSTEM_RECLAIM_READY);
	assert(xcy_system_reclaim_complete(&episode, now + 2, 0) == XCY_SYSTEM_RECLAIM_FALLBACK);
	assert(xcy_system_reclaim_deadline(&episode) == 0);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, now + 60000) == XCY_SYSTEM_RECLAIM_FALLBACK);

	episode = after_kill;
	assert(xcy_reclaim_observe_without_pageout(&episode, healthy, now - 1) == XCY_SYSTEM_RECLAIM_NONE);
	assert(xcy_system_reclaim_deadline(&episode) == 0);
	next_round = episode.last_action_ms + XCY_SYSTEM_RECLAIM_EPISODE_GAP_MS;
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, next_round - 1) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(xcy_system_reclaim_deadline(&episode) == next_round);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, next_round) == XCY_SYSTEM_RECLAIM_READY);

	episode = (xcy_system_reclaim_episode_t){0};
	assert(xcy_reclaim_observe_without_pageout(&episode, XCY_SYSTEM_PRESSURE_UNKNOWN, 1) ==
	       XCY_SYSTEM_RECLAIM_FALLBACK);
	assert(xcy_system_reclaim_deadline(&episode) == 0);
	assert(!episode.action_seen);
	puts("kill-only pressure lifecycle passed");
	return 0;
}
