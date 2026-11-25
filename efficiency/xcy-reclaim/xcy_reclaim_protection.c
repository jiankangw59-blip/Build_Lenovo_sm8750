#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#include "xcy_reclaim_protection.h"

#define XCY_AUDIO_TTL_MS 5000U
#define XCY_SOURCE_ROWS_MAX 16384U

enum xcy_source_list {
	XCY_VISIBLE,
	XCY_ACTIVE,
	XCY_NORMAL,
	XCY_FULL,
	XCY_EXACT,
	XCY_PLAYBACK,
	XCY_LIST_COUNT,
};

static const char list_tags[] = "VANFEU";

void xcy_reclaim_protection_free(struct xcy_reclaim_protection_frame *frame)
{
	xcy_snapshot_free(frame->uids);
	xcy_snapshot_free(frame->names);
	xcy_snapshot_free(frame->storage);
	memset(frame, 0, sizeof(*frame));
}

static int parse_list(struct xcy_reclaim_protection_frame *parsed, const char *data,
		      size_t length, size_t *offset, enum xcy_source_list list,
		      size_t count, size_t *first_name)
{
	size_t i;
	for (i = 0; i < count; ++i) {
		xcy_memory_token_t words[2];
		xcy_memory_token_t value;
		size_t value_offset;

		if (!xcy_memory_line(data, length, offset, words, 2) || words[0].len != 1 ||
		    words[0].text[0] != list_tags[list])
			return -EINVAL;
		value = words[1];
		if (list == XCY_PLAYBACK) {
			uint64_t uid;
			if (!xcy_memory_uint(value, &uid) || uid > XCY_RECLAIM_UINT32_MAX)
				return -EINVAL;
			parsed->uids[i] = uid;
			continue;
		}
		if (list == XCY_EXACT) {
			if (value.len >= XCY_MEMORY_PROCESS_BYTES || !xcy_memory_ascii_word(value))
				return -EINVAL;
		} else if (value.len >= XCY_MEMORY_PACKAGE_BYTES || !xcy_memory_package(value)) {
			return -EINVAL;
		}
		value_offset = value.text - data;
		parsed->storage[value_offset + value.len] = '\0';
		parsed->names[(*first_name)++] = &parsed->storage[value_offset];
	}
	return 0;
}

int xcy_reclaim_protection_parse(const char *data, size_t length, const char *nonce,
				 uint64_t earliest_ms, uint64_t now_ms,
				 struct xcy_reclaim_protection_frame *frame)
{
	struct xcy_reclaim_protection_frame parsed = {0};
	struct xcy_reclaim_names *lists[] = {
		&parsed.view.visible, &parsed.view.active, &parsed.view.package_whitelist,
		&parsed.view.full_whitelist, &parsed.view.exact_whitelist,
	};
	xcy_memory_token_t words[18];
	uint64_t fields[15], total_names = 0;
	size_t offset = 0, i, first_name = 0;
	int error = -EINVAL;
	bool scene_only;

	if (!data || !frame || !nonce || !length || length > XCY_RECLAIM_STATE_BYTES ||
	    data[length - 1] != '\n' || memchr(data, '\0', length) ||
	    !xcy_memory_line(data, length, &offset, words, 18) ||
	    !xcy_memory_token_equals(words[0], "XCY_PROTECT_V1") ||
	    !xcy_memory_identifier(words[1]) || !xcy_memory_token_equals(words[1], nonce) ||
	    (!xcy_memory_token_equals(words[17], "-") &&
	     (words[17].len >= XCY_MEMORY_PACKAGE_BYTES || !xcy_memory_package(words[17]))))
		return -EINVAL;
	for (i = 0; i < 15; ++i)
		if (!xcy_memory_uint(words[2 + i], &fields[i]))
			return -EINVAL;
	scene_only = strcmp(nonce, XCY_RECLAIM_SCENE_NONCE) == 0;
	/* observed, expires, Scene timestamp, Scene generation, grace, sync UID,
	 * sync expiry, then counts V/A/N/F/E/U, source-ready and Scene max age. */
	if (!fields[0] || fields[0] < earliest_ms || fields[0] > now_ms ||
	    fields[1] <= now_ms || fields[1] - fields[0] > XCY_AUDIO_TTL_MS ||
	    !fields[2] || fields[2] > fields[0] || !fields[3] ||
	    !fields[14] || fields[14] > XCY_RECLAIM_UINT32_MAX || now_ms - fields[2] > fields[14] ||
	    !fields[4] || fields[4] > XCY_RECLAIM_UINT32_MAX ||
	    fields[5] > XCY_RECLAIM_UINT32_MAX || (!!fields[5] != !!fields[6]) ||
	    (!fields[7] && !fields[8]) || fields[13] != !scene_only)
		return -EINVAL;
	if (scene_only && (fields[5] || fields[9] || fields[10] || fields[11] || fields[12] ||
	    !xcy_memory_token_equals(words[17], "-")))
		return -EINVAL;
	for (i = 0; i < XCY_LIST_COUNT; ++i) {
		if (fields[7 + i] > XCY_SOURCE_ROWS_MAX)
			return -E2BIG;
		if (i != XCY_PLAYBACK)
			total_names += fields[7 + i];
	}
	if (total_names + fields[12] > XCY_SOURCE_ROWS_MAX)
		return -E2BIG;
	parsed.storage = xcy_snapshot_calloc(length + 1, 1);
	parsed.names = xcy_snapshot_calloc(total_names, sizeof(*parsed.names));
	if (fields[12])
		parsed.uids = xcy_snapshot_calloc(fields[12], sizeof(*parsed.uids));
	if (!parsed.storage || !parsed.names || (fields[12] && !parsed.uids)) {
		error = -ENOMEM;
		goto out;
	}
	memcpy(parsed.storage, data, length);
	if (!xcy_memory_token_equals(words[17], "-")) {
		size_t game = words[17].text - data;
		parsed.storage[game + words[17].len] = '\0';
		parsed.view.current_game = &parsed.storage[game];
	}
	for (i = 0; i < XCY_LIST_COUNT; ++i) {
		if (i != XCY_PLAYBACK) {
			lists[i]->values = &parsed.names[first_name];
			lists[i]->count = fields[7 + i];
		}
		error = parse_list(&parsed, data, length, &offset, i, fields[7 + i], &first_name);
		if (error)
			goto out;
	}
	if (!xcy_memory_line(data, length, &offset, words, 2) || offset != length ||
	    !xcy_memory_token_equals(words[0], "END") || !xcy_memory_token_equals(words[1], nonce)) {
		error = -EINVAL;
		goto out;
	}
	strcpy(parsed.nonce, nonce);
	parsed.view.observed_ms = fields[0];
	parsed.view.expires_ms = fields[1];
	parsed.scene_observed_ms = fields[2];
	parsed.scene_valid_ms = fields[14];
	parsed.scene_generation = fields[3];
	parsed.grace_ms = fields[4];
	parsed.view.sync_uid = fields[5];
	parsed.view.sync_expires_ms = fields[6];
	parsed.view.playback_count = fields[12];
	parsed.view.playback_uids = parsed.uids;
	parsed.view.ready = !scene_only;
	*frame = parsed;
	return 0;
out:
	xcy_reclaim_protection_free(&parsed);
	return error;
}
