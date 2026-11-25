#ifdef __KERNEL__
#include <linux/errno.h>
#else
#include <errno.h>
#endif

#include "xcy_reclaim_protection.h"

#define XCY_AUDIO_TTL_MS 5000U

void xcy_reclaim_protection_free(struct xcy_reclaim_protection_frame *frame)
{
	xcy_snapshot_free(frame->uids);
	xcy_snapshot_free(frame->names);
	xcy_snapshot_free(frame->storage);
	memset(frame, 0, sizeof(*frame));
}

int xcy_reclaim_audio_parse(const char *data, size_t length, const char *nonce,
			    uint64_t earliest_ms, uint64_t now_ms,
			    struct xcy_reclaim_protection_frame *frame)
{
	struct xcy_reclaim_protection_frame parsed = {0};
	xcy_memory_token_t words[5];
	uint64_t observed, expires, count;
	size_t offset = 0, i;
	int error = -EINVAL;

	if (!length || length > 4096 || data[length - 1] != '\n' || memchr(data, 0, length) ||
	    !xcy_memory_line(data, length, &offset, words, 5) ||
	    !xcy_memory_token_equals(words[0], "XCY_AUDIO_V1") || !xcy_memory_token_equals(words[1], nonce) ||
	    !xcy_memory_uint(words[2], &observed) || !observed || observed < earliest_ms || observed > now_ms ||
	    !xcy_memory_uint(words[3], &expires) || expires <= now_ms || expires - observed > XCY_AUDIO_TTL_MS ||
	    !xcy_memory_uint(words[4], &count) || count > 128)
		return -EINVAL;
	if (count) {
		parsed.uids = xcy_snapshot_calloc(count, sizeof(*parsed.uids));
		if (!parsed.uids)
			return -ENOMEM;
	}
	for (i = 0; i < count; ++i) {
		uint64_t uid;

		if (!xcy_memory_line(data, length, &offset, words, 2) ||
		    !xcy_memory_token_equals(words[0], "U") ||
		    !xcy_memory_uint(words[1], &uid) || uid > XCY_RECLAIM_INT32_MAX || uid % 100000 < 10000)
			goto out;
		parsed.uids[i] = uid;
	}
	if (!xcy_memory_line(data, length, &offset, words, 2) || offset != length ||
	    !xcy_memory_token_equals(words[0], "END") || !xcy_memory_token_equals(words[1], nonce))
		goto out;
	strcpy(parsed.nonce, nonce);
	parsed.view.observed_ms = observed;
	parsed.view.expires_ms = expires;
	parsed.view.playback_count = count;
	parsed.view.playback_uids = parsed.uids;
	parsed.view.ready = true;
	*frame = parsed;
	return 0;
out:
	xcy_reclaim_protection_free(&parsed);
	return error;
}
