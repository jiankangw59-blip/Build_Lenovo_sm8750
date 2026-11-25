#include <assert.h>
#include <stdio.h>

#include "xcy_reclaim_state.h"

int main(void)
{
	xcy_system_reclaim_episode_t episode = {0};
	xcy_system_reclaim_episode_t cooldown;
	xcy_system_pressure_level_t stress = xcy_system_ram_pressure_classify(10000, 1200);
	xcy_system_pressure_level_t critical = xcy_system_ram_pressure_classify(10000, 1199);
	xcy_system_pressure_level_t healthy = xcy_system_ram_pressure_classify(10000, 3000);

	assert(stress == XCY_SYSTEM_PRESSURE_STRESS);
	assert(critical == XCY_SYSTEM_PRESSURE_CRITICAL);
	assert(healthy == XCY_SYSTEM_PRESSURE_HEALTHY);
	assert(xcy_reclaim_observe_without_pageout(&episode, stress, 1000) == XCY_SYSTEM_RECLAIM_NONE);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 0);
	assert(episode.critical_reclaim_rounds == 0);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 2000) == XCY_SYSTEM_RECLAIM_READY);
	assert(episode.phase == XCY_SYSTEM_RECLAIM_ACTION_PENDING);
	assert(xcy_reclaim_complete_without_pageout(&episode, 2001, 1) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 5001);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 5000) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 5001) == XCY_SYSTEM_RECLAIM_READY);
	assert(episode.critical_reclaim_rounds == 0);
	assert(xcy_reclaim_complete_without_pageout(&episode, 5002, 1) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(episode.episode_actions == XCY_SYSTEM_RECLAIM_MAX_ACTIONS);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 35002);
	cooldown = episode;
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 8002) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 35001) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 35002) == XCY_SYSTEM_RECLAIM_READY);
	assert(episode.episode_actions == 0);
	assert(xcy_reclaim_complete_without_pageout(&episode, 35003, 1) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(episode.episode_actions == 1);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 38003);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 38003) == XCY_SYSTEM_RECLAIM_READY);
	assert(xcy_reclaim_complete_without_pageout(&episode, 38004, 1) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 68004);

	episode = cooldown;
	assert(xcy_reclaim_observe_without_pageout(&episode, stress, 35002) == XCY_SYSTEM_RECLAIM_NONE);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 0);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 36000) == XCY_SYSTEM_RECLAIM_READY);
	assert(xcy_reclaim_complete_without_pageout(&episode, 36001, 0) == XCY_SYSTEM_RECLAIM_FALLBACK);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 0);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 70000) == XCY_SYSTEM_RECLAIM_FALLBACK);

	episode = cooldown;
	assert(xcy_reclaim_observe_without_pageout(&episode, healthy, 8003) == XCY_SYSTEM_RECLAIM_NONE);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 0);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 35001) == XCY_SYSTEM_RECLAIM_WAIT);
	assert(xcy_system_reclaim_deadline(&episode) == 35002);
	assert(xcy_reclaim_wakeup_deadline(&episode) == 0);
	assert(xcy_reclaim_observe_without_pageout(&episode, critical, 35002) == XCY_SYSTEM_RECLAIM_READY);
	assert(xcy_reclaim_observe_without_pageout(&episode, healthy, 35003) == XCY_SYSTEM_RECLAIM_NONE);

	episode = (xcy_system_reclaim_episode_t){0};
	assert(xcy_reclaim_observe_without_pageout(&episode, XCY_SYSTEM_PRESSURE_UNKNOWN, 1) ==
	       XCY_SYSTEM_RECLAIM_FALLBACK);
	assert(xcy_system_reclaim_deadline(&episode) == 0);
	assert(!episode.action_seen);
	puts("kill-only pressure lifecycle passed");
	return 0;
}
