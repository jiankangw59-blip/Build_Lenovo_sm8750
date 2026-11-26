// SPDX-License-Identifier: GPL-2.0
#include <assert.h>
#include <stdio.h>
#include "scene_activity_load.h"

int main(void)
{
	struct sa_cpu_counter old = {10000, 2000, true}, now = {20000, 5000, true};
	sa_u64 total, idle;

	assert(sa_counter_delta(&old, &now, &total, &idle));
	assert(total == 10000 && idle == 3000 && sa_busy_permille(total, idle) == 700);
	assert(sa_busy_permille(1000, 0) == 1000 && sa_busy_permille(1000, 1000) == 0);
	assert(sa_busy_permille(10000, 3500) == 650);
	assert(sa_native_load_hold_ms(sa_busy_permille(10000, 3500)) == 0);
	assert(sa_native_load_hold_ms(sa_busy_permille(10000, 3490)) == 3000);
	assert(sa_busy_permille(UINT64_MAX, 0) == 1000);
	assert(sa_busy_permille(UINT64_MAX, UINT64_MAX) == 0);
	now.online = false; assert(!sa_counter_delta(&old, &now, &total, &idle));
	now.online = true; old.online = false; assert(!sa_counter_delta(&old, &now, &total, &idle));
	old.online = true; now.total = old.total; assert(!sa_counter_delta(&old, &now, &total, &idle));
	now.total = old.total - 1; assert(!sa_counter_delta(&old, &now, &total, &idle));
	now.total = 20000; now.idle = 19000; assert(!sa_counter_delta(&old, &now, &total, &idle));
	now.idle = old.idle - 1; assert(!sa_counter_delta(&old, &now, &total, &idle));
	puts("scene load: real-window deltas, strict bands, overflow and hotplug/reset rejection passed");
	return 0;
}
