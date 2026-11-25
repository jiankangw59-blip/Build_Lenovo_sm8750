#include <linux/bpf.h>
#include <linux/bsearch.h>
#include <linux/ktime.h>
#include <linux/mutex.h>
#include <linux/sort.h>
#include <linux/xarray.h>

#include "xcy_reclaim_traffic.h"

#define XCY_TRAFFIC_HOLDS_MAX 4096U

struct xcy_netd_uid_stats {
	u64 rx_packets;
	u64 rx_bytes;
	u64 tx_packets;
	u64 tx_bytes;
};

struct xcy_traffic_sample {
	u32 uid;
	u64 rx_bytes;
	u64 observed;
};

struct xcy_reclaim_traffic {
	struct bpf_map *map;
	size_t count;
	struct xcy_traffic_sample rows[];
};

static DEFINE_MUTEX(traffic_lock);
static struct bpf_map *traffic_map;
static DEFINE_XARRAY(download_holds);
static size_t hold_count;

int xcy_reclaim_traffic_bind(unsigned int fd)
{
	struct bpf_map *map = bpf_map_get_with_uref(fd);
	struct bpf_map *previous;
	unsigned long uid;
	void *entry;

	if (IS_ERR(map))
		return PTR_ERR(map);
	if (map->map_type != BPF_MAP_TYPE_HASH || map->key_size != sizeof(u32) ||
	    map->value_size != sizeof(struct xcy_netd_uid_stats) ||
	    strcmp(map->name, "app_uid_stats_m")) {
		bpf_map_put_with_uref(map);
		return -EINVAL;
	}
	mutex_lock(&traffic_lock);
	previous = traffic_map;
	WRITE_ONCE(traffic_map, map);
	if (map != previous) {
		xa_for_each(&download_holds, uid, entry)
			xa_erase(&download_holds, uid);
		hold_count = 0;
	}
	mutex_unlock(&traffic_lock);
	if (previous)
		bpf_map_put_with_uref(previous);
	return 0;
}

static u64 xcy_netd_rx(struct bpf_map *map, u32 uid)
{
	const struct xcy_netd_uid_stats *stats;
	u64 rx;

	rcu_read_lock();
	stats = map->ops->map_lookup_elem(map, &uid);
	rx = stats ? READ_ONCE(stats->rx_bytes) : 0;
	rcu_read_unlock();
	return rx;
}

bool xcy_reclaim_traffic_held(u32 uid, u64 now)
{
	void *entry = xa_load(&download_holds, uid);

	return entry && xa_to_value(entry) > now;
}

static void xcy_net_hold(struct bpf_map *map, u32 uid, u64 now)
{
	void *entry;
	unsigned long key;

	mutex_lock(&traffic_lock);
	if (map != traffic_map)
		goto out;
	if (hold_count >= XCY_TRAFFIC_HOLDS_MAX) {
		xa_for_each(&download_holds, key, entry) {
			if (xa_to_value(entry) > now)
				continue;
			xa_erase(&download_holds, key);
			--hold_count;
		}
	}
	if (hold_count >= XCY_TRAFFIC_HOLDS_MAX && !xa_load(&download_holds, uid))
		goto out;
	entry = xa_store(&download_holds, uid, xa_mk_value(now + XCY_DOWNLOAD_HOLD_MS), GFP_NOWAIT);
	if (!entry)
		++hold_count;
out:
	mutex_unlock(&traffic_lock);
}

static int xcy_compare_uid(const void *a, const void *b)
{
	const struct xcy_traffic_sample *left = a, *right = b;

	return (left->uid > right->uid) - (left->uid < right->uid);
}

int xcy_reclaim_traffic_start(const struct xcy_reclaim_plan *plan, struct xcy_reclaim_traffic **result)
{
	struct xcy_reclaim_traffic *traffic;
	size_t i, unique = 0;
	u64 observed;

	traffic = xcy_snapshot_calloc(struct_size(traffic, rows, plan->member_count * 2), 1);
	if (!traffic)
		return -ENOMEM;
	mutex_lock(&traffic_lock);
	traffic->map = traffic_map;
	if (traffic->map)
		bpf_map_inc_with_uref(traffic->map);
	mutex_unlock(&traffic_lock);
	if (!traffic->map) {
		xcy_snapshot_free(traffic);
		return -ENOKEY;
	}
	for (i = 0; i < plan->member_count; ++i) {
		const struct xcy_reclaim_member *member = &plan->members[i];

		traffic->rows[traffic->count++].uid = member->uid;
		if (member->uid != member->kernel_uid)
			traffic->rows[traffic->count++].uid = member->kernel_uid;
	}
	sort(traffic->rows, traffic->count, sizeof(*traffic->rows), xcy_compare_uid, NULL);
	observed = ktime_to_ms(ktime_get_boottime());
	for (i = 0; i < traffic->count; ++i) {
		u32 uid = traffic->rows[i].uid;

		if (unique && traffic->rows[unique - 1].uid == uid)
			continue;
		traffic->rows[unique++] = (struct xcy_traffic_sample){
			.uid = uid, .rx_bytes = xcy_netd_rx(traffic->map, uid), .observed = observed,
		};
	}
	traffic->count = unique;
	*result = traffic;
	return 0;
}

static bool xcy_download_uid(struct xcy_reclaim_traffic *traffic, u32 uid, u64 now, bool advance)
{
	struct xcy_traffic_sample key = {.uid = uid};
	struct xcy_traffic_sample *sample;
	u64 rx;
	bool busy;

	if (xcy_reclaim_traffic_held(uid, now))
		return true;
	sample = bsearch(&key, traffic->rows, traffic->count, sizeof(*traffic->rows), xcy_compare_uid);
	if (!sample || now < sample->observed)
		return true;
	rx = xcy_netd_rx(traffic->map, uid);
	busy = xcy_download_rate_busy(sample->rx_bytes, rx, now - sample->observed);
	if (busy)
		xcy_net_hold(traffic->map, uid, now);
	if (advance && now > sample->observed) {
		sample->observed = now;
		sample->rx_bytes = rx;
	}
	return busy;
}

bool xcy_reclaim_traffic_busy(struct xcy_reclaim_traffic *traffic, const struct xcy_reclaim_plan *plan,
			      size_t selected, bool advance)
{
	const struct xcy_reclaim_group *group = &plan->groups[selected];
	u64 now = ktime_to_ms(ktime_get_boottime());
	size_t i;

	if (!traffic || traffic->map != READ_ONCE(traffic_map))
		return true;
	/* All group members have the same package UID. Isolated Linux UIDs may
	 * own their own sockets, and are independently checked. */
	if (xcy_download_uid(traffic, plan->members[group->first].uid, now, advance))
		return true;
	for (i = group->first; i < group->first + group->count; ++i) {
		const struct xcy_reclaim_member *member = &plan->members[i];

		if (member->kernel_uid != member->uid &&
		    xcy_download_uid(traffic, member->kernel_uid, now, advance))
			return true;
	}
	return traffic->map != READ_ONCE(traffic_map);
}

void xcy_reclaim_traffic_put(struct xcy_reclaim_traffic *traffic)
{
	if (traffic) {
		bpf_map_put_with_uref(traffic->map);
		xcy_snapshot_free(traffic);
	}
}
