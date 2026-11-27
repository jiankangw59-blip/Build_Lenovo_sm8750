/* SPDX-License-Identifier: GPL-2.0 */
#ifndef SCENE_REFRESH_CONFIG_H
#define SCENE_REFRESH_CONFIG_H

#include <stdatomic.h>
#include "scene_refresh_policy.h"

enum srcfg_status {
	SRCFG_OK = 0,
	SRCFG_INVALID_ARGUMENT,
	SRCFG_SYNTAX,
	SRCFG_VALUE,
	SRCFG_OVERFLOW,
	SRCFG_NOMEM,
	SRCFG_EMPTY,
	SRCFG_UNSUPPORTED_RATE,
	SRCFG_CATALOG,
	SRCFG_TOO_LARGE,
	SRCFG_IO,
	SRCFG_CHANGED,
};

enum srcfg_source { SRCFG_SOURCE_NONE, SRCFG_SOURCE_APPS, SRCFG_SOURCE_GLOBAL,
	SRCFG_SOURCE_CATALOG };

enum srcfg_flags {
	SRCFG_ALLOW_EMPTY_APPS = 1U << 0,
	SRCFG_ALLOW_EMPTY_GLOBAL = 1U << 1,
};

struct srcfg_error {
	enum srcfg_status code;
	enum srcfg_source source;
	size_t line;
	int sys_errno;
};

struct srcfg_options {
	unsigned int flags;
	/* Combined input bytes; zero selects 16 MiB. No fixed rule-count limit. */
	size_t max_bytes;
	/* Platform defaults are supplied explicitly; all are false on audited Ace6. */
	bool default_strict;
	bool default_compatible;
	bool video_optimize;
};

struct srcfg_entry {
	char *key;
	char *value;
	size_t line;
	size_t occurrences;
	bool known;
};

/* Owned immutable snapshot. Its catalog owns a copy of the caller's mode list. */
struct srcfg_snapshot {
	struct srp_config policy;
	struct srp_catalog catalog;
	struct srp_app_rule *rules;
	size_t rule_count;
	/* All effective global keys, including unknown keys and original raw values. */
	struct srcfg_entry *entries;
	size_t entry_count;
	size_t unknown_key_count;
	size_t duplicate_app_count;
	size_t duplicate_key_count;
	size_t normalized_value_count;
};

/*
 * Callers provide detected high/middle/low/peak ids for one resolution group.
 * This adapter never guesses high=maximum and never treats display_modes as Hz.
 * On failure *out is untouched. Inputs need not be NUL terminated.
 */
enum srcfg_status srcfg_parse_buffers(const char *apps, size_t apps_len,
		const char *global, size_t global_len, const struct srp_catalog *catalog,
		const struct srcfg_options *options, struct srcfg_snapshot **out,
		struct srcfg_error *error);

/* One read-only attempt, with both open-file and path metadata checked after read. */
enum srcfg_status srcfg_load_paths(const char *apps_path, const char *global_path,
		const struct srp_catalog *catalog, const struct srcfg_options *options,
		struct srcfg_snapshot **out, struct srcfg_error *error);

/*
 * Publishes only a fully validated candidate. Failure leaves *current untouched.
 * Returns the old snapshot in *retired WITHOUT freeing it: caller must reclaim
 * after its readers finish (single owner event loop, lock, or grace period).
 * Atomic pointer loading alone is not a lifetime/reference-counting protocol.
 */
enum srcfg_status srcfg_replace_buffers(_Atomic(struct srcfg_snapshot *) *current,
		const char *apps, size_t apps_len, const char *global, size_t global_len,
		const struct srp_catalog *catalog, const struct srcfg_options *options,
		struct srcfg_snapshot **retired, struct srcfg_error *error);

enum srcfg_status srcfg_replace_paths(_Atomic(struct srcfg_snapshot *) *current,
		const char *apps_path, const char *global_path,
		const struct srp_catalog *catalog, const struct srcfg_options *options,
		struct srcfg_snapshot **retired, struct srcfg_error *error);

void srcfg_snapshot_free(struct srcfg_snapshot *snapshot);

#endif /* SCENE_REFRESH_CONFIG_H */
