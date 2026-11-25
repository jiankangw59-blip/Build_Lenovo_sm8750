#include "xcy_reclaim_lease.h"

/* XiaoCaiYe opt13 lease grammar and time bounds. */
static int xcy_json_space(unsigned char c) {
	return c == ' ' || c == '\n' || c == '\r' || c == '\t';
}

static void xcy_json_skip_space(const char **p, const char *end) {
	while (*p < end && xcy_json_space((unsigned char)**p)) ++*p;
}

/* Unknown strings (including lease capability tokens) are skipped, never copied. */
static int xcy_json_string(const char **cursor, const char *end, char *out, size_t cap) {
	const char *p = *cursor;
	size_t used = 0;
	if (p >= end || *p++ != '"') return 0;
	while (p < end && *p != '"') {
		unsigned char c = (unsigned char)*p++;
		if (c < 32) return 0;
		if (c == '\\') {
			unsigned char escape;
			/* All policy strings/keys emitted by the producer are plain ASCII. */
			if (p >= end || out != NULL) return 0;
			escape = (unsigned char)*p++;
			if (escape == 'u') {
				for (int i = 0; i < 4; ++i) {
					if (p >= end || !((*p >= '0' && *p <= '9') || (*p >= 'a' && *p <= 'f') ||
									 (*p >= 'A' && *p <= 'F'))) return 0;
					++p;
				}
			} else if (strchr("\"\\/bfnrt", escape) == NULL) return 0;
		} else if (out != NULL) {
			if (used + 1 >= cap || c > 127) return 0;
			out[used++] = (char)c;
		}
	}
	if (p >= end || *p++ != '"') return 0;
	if (out != NULL) out[used] = '\0';
	*cursor = p;
	return 1;
}

static int xcy_json_uint(const char **cursor, const char *end, uint64_t *out) {
	const char *p = *cursor;
	uint64_t n = 0;
	if (p >= end || *p < '0' || *p > '9') return 0;
	if (*p == '0' && p + 1 < end && p[1] >= '0' && p[1] <= '9') return 0;
	do {
		unsigned digit = (unsigned)(*p++ - '0');
		if (n > (XCY_RECLAIM_UINT64_MAX - digit) / 10) return 0;
		n = n * 10 + digit;
	} while (p < end && *p >= '0' && *p <= '9');
	*out = n;
	*cursor = p;
	return 1;
}

/* UUID comparison with syntax validation: -1 invalid, 0 different, 1 equal.
 * Hex case is not a different boot identity. */
int xcy_boot_uuid_relation(const char *left, const char *right) {
	int equal = 1;
	if (left == NULL || right == NULL || strlen(left) != 36 || strlen(right) != 36) return -1;
	for (size_t i = 0; i < 36; ++i) {
		unsigned char a = (unsigned char)left[i], b = (unsigned char)right[i];
		if (i == 8 || i == 13 || i == 18 || i == 23) {
			if (a != '-' || b != '-') return -1;
			continue;
		}
		if (a >= 'A' && a <= 'F') a = (unsigned char)(a + ('a' - 'A'));
		if (b >= 'A' && b <= 'F') b = (unsigned char)(b + ('a' - 'A'));
		if (!((a >= '0' && a <= '9') || (a >= 'a' && a <= 'f')) ||
			!((b >= '0' && b <= '9') || (b >= 'a' && b <= 'f'))) return -1;
		if (a != b) equal = 0;
	}
	return equal;
}

/* The root lease is one bounded, flat JSON object. Never search token contents.
 * Return 1 only for active (out populated), 2 for structurally valid inactive
 * leases, and 0 for malformed/unknown or implausibly future current-boot data. */
int xcy_parse_sync_lease(const char *data, size_t length, const char *boot_id,
							  uint64_t now_ms, xcy_sync_lease_t *out) {
	const char *p, *end;
	uint64_t version = 0, uid = 0, pid = 0, start = 0, expires = 0;
	char owner[24] = "", boot[48] = "", state[24] = "", key[48];
	unsigned seen = 0;
	size_t fields = 0;
	int boot_relation, active;
	if (data == NULL || boot_id == NULL || out == NULL || length == 0 || length > 4096 ||
		memchr(data, 0, length) != NULL || strlen(boot_id) != 36) return 0;
	p = data; end = data + length;
	xcy_json_skip_space(&p, end);
	if (p >= end || *p++ != '{') return 0;
	xcy_json_skip_space(&p, end);
	if (p < end && *p == '}') return 0;
	for (;;) {
		unsigned bit = 0;
		uint64_t *number = NULL;
		char *text = NULL;
		size_t text_cap = 0;
		if (++fields > 24 || !xcy_json_string(&p, end, key, sizeof(key))) return 0;
		xcy_json_skip_space(&p, end);
		if (p >= end || *p++ != ':') return 0;
		xcy_json_skip_space(&p, end);
		if (strcmp(key, "version") == 0) { bit = 1; number = &version; }
		else if (strcmp(key, "uid") == 0) { bit = 2; number = &uid; }
		else if (strcmp(key, "pid") == 0) { bit = 4; number = &pid; }
		else if (strcmp(key, "startTime") == 0) { bit = 8; number = &start; }
		else if (strcmp(key, "expiresAt") == 0) { bit = 16; number = &expires; }
		else if (strcmp(key, "owner") == 0) { bit = 32; text = owner; text_cap = sizeof(owner); }
		else if (strcmp(key, "bootId") == 0) { bit = 64; text = boot; text_cap = sizeof(boot); }
		else if (strcmp(key, "state") == 0) { bit = 128; text = state; text_cap = sizeof(state); }
		if (bit != 0 && (seen & bit) != 0) return 0;
		seen |= bit;
		if (number != NULL) {
			int quoted_start = bit == 8 && p < end && *p == '"';
			if (quoted_start) ++p;
			if (!xcy_json_uint(&p, end, number)) return 0;
			if (quoted_start && (p >= end || *p++ != '"')) return 0;
		} else if (text != NULL) {
			if (!xcy_json_string(&p, end, text, text_cap)) return 0;
		} else if (p < end && *p == '"') {
			if (!xcy_json_string(&p, end, NULL, 0)) return 0;
		} else {
			uint64_t ignored;
			if (end - p >= 4 && (memcmp(p, "true", 4) == 0 || memcmp(p, "null", 4) == 0)) p += 4;
			else if (end - p >= 5 && memcmp(p, "false", 5) == 0) p += 5;
			else if (!xcy_json_uint(&p, end, &ignored)) return 0;
		}
		xcy_json_skip_space(&p, end);
		if (p >= end) return 0;
		if (*p == '}') { ++p; break; }
		if (*p++ != ',') return 0;
		xcy_json_skip_space(&p, end);
	}
	xcy_json_skip_space(&p, end);
	if (p != end || seen != 255 || version != 1 || uid > XCY_RECLAIM_INT32_MAX || uid % 100000 < 10000 ||
		pid == 0 || pid > XCY_RECLAIM_INT32_MAX || start == 0 ||
		(strcmp(owner, "reader") != 0 && strcmp(owner, "headless") != 0)) return 0;
	boot_relation = xcy_boot_uuid_relation(boot, boot_id);
	active = strcmp(state, "active") == 0;
	if (boot_relation < 0 || (!active && strcmp(state, "released") != 0 &&
		strcmp(state, "recovering") != 0)) return 0;
	/* BOOTTIME values from a valid previous boot are not comparable with
	 * this boot's clock. Validate the full schema before declaring it stale. */
	if (boot_relation == 0) return 2;
	if (expires > now_ms && expires - now_ms > 120000) return 0;
	if (!active || expires <= now_ms) return 2;
	memset(out, 0, sizeof(*out));
	out->uid = (uint32_t)uid;
	out->pid = (uint32_t)pid;
	out->start_ticks = start;
	out->expires_ms = expires;
	out->headless = strcmp(owner, "headless") == 0;
	return 1;
}
