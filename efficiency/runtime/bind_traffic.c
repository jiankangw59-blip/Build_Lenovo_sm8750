#include <fcntl.h>
#include <linux/bpf.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#include <sys/system_properties.h>
#include <unistd.h>

#include "xcy_reclaim_traffic.h"

struct unlock_state {
	uint32_t serial;
	bool unlocked;
};

static void read_unlock(void *cookie, const char *name, const char *value, uint32_t serial)
{
	struct unlock_state *state = cookie;

	(void)name;
	state->serial = serial;
	state->unlocked = !strcmp(value, "true");
}

static int wait_for_unlock(void)
{
	static const char property[] = "sys.user.0.ce_available";
	const prop_info *info;
	uint32_t old_serial, new_serial;

	for (;;) {
		struct unlock_state state = {};

		old_serial = __system_property_area_serial();
		info = __system_property_find(property);
		if (info) {
			__system_property_read_callback(info, read_unlock, &state);
			if (state.unlocked)
				return 0;
			old_serial = state.serial;
		}
		if (!__system_property_wait(info, old_serial, &new_serial, NULL))
			return 1;
	}
}

int main(int argc, char **argv)
{
	static const char map_path[] = "/sys/fs/bpf/netd_shared/map_netd_app_uid_stats_map";
	union bpf_attr attr = {
		.pathname = (uintptr_t)map_path,
		.file_flags = BPF_F_RDONLY,
	};
	const char *mode;
	int policy, map, rc = 1;
	size_t size;

	if (argc != 2)
		return 2;
	if (!strcmp(argv[1], "wait-unlock"))
		return wait_for_unlock();
	if (!strcmp(argv[1], "observe"))
		mode = "observe\n";
	else if (!strcmp(argv[1], "enable"))
		mode = "enable\n";
	else
		return 2;
	map = syscall(__NR_bpf, BPF_OBJ_GET, &attr, sizeof(attr));
	if (map < 0) {
		perror("Android UID traffic map");
		return 1;
	}
	policy = open("/dev/xcy_reclaim_policy", O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
	if (policy < 0) {
		perror("XiaoCaiYe policy device");
		close(map);
		return 1;
	}
	size = strlen(mode);
	if (ioctl(policy, XCY_RECLAIM_BIND_TRAFFIC, map) < 0)
		perror("Bind UID traffic map");
	else if (write(policy, mode, size) != (ssize_t)size)
		perror("Set XiaoCaiYe policy mode");
	else
		rc = 0;
	close(policy);
	close(map);
	return rc;
}
