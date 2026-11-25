#ifndef XCY_RECLAIM_LEASE_H
#define XCY_RECLAIM_LEASE_H

#include "xcy_reclaim_port.h"

typedef struct {
	uint32_t uid;
	uint32_t pid;
	uint64_t start_ticks;
	uint64_t expires_ms;
	int headless;
} xcy_sync_lease_t;

int xcy_boot_uuid_relation(const char *left, const char *right);
int xcy_parse_sync_lease(const char *data, size_t length, const char *boot_id,
			 uint64_t now_ms, xcy_sync_lease_t *out);

#endif
