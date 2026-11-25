/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ACE6_ADAPTIVE_POLICY_H
#define ACE6_ADAPTIVE_POLICY_H

#include <linux/ctype.h>
#include <linux/cred.h>
#include "affinity_rules.h"

#define VISIBLE_PACKAGES_LEN 1024

struct scene_context {
	char foreground[PROCESS_NAME_LEN];
	char visible[VISIBLE_PACKAGES_LEN];
	bool display_off;
	bool game;
};

enum app_stage {
	APP_STATIC,
	APP_FOREGROUND,
	APP_BACKGROUND,
	APP_DISPLAY_OFF,
};

enum thread_role {
	ROLE_INTERACTIVE,
	ROLE_RUNTIME,
	ROLE_WORKER,
	ROLE_IDLE,
};

static cpumask_t tiny_cpus, little_cpus, interactive_cpus, background_cpus;
static cpumask_t game_top_cpus, game_third_cpus;

static void init_adaptive_masks(void)
{
	unsigned int count = num_possible_cpus();
	unsigned int index = 0;
	unsigned int little = count <= 4 ? (count + 1) / 2 : count / 2;
	unsigned int interactive = count <= 4 ? count : count - 2;
	int cpu;

	for_each_possible_cpu(cpu) {
		if (index < (count <= 4 ? 1 : 2))
			cpumask_set_cpu(cpu, &tiny_cpus);
		if (index < little)
			cpumask_set_cpu(cpu, &little_cpus);
		if (index < interactive)
			cpumask_set_cpu(cpu, &interactive_cpus);
		if (index < 5)
			cpumask_set_cpu(cpu, &background_cpus);
		if (count == 8 && index >= 4)
			cpumask_set_cpu(cpu, &game_third_cpus);
		if (count == 8 && index >= 6)
			cpumask_set_cpu(cpu, &game_top_cpus);
		index++;
	}
}

static bool package_char(char c)
{
	return isalnum(c) || c == '.' || c == '_';
}

static int parse_scene_context(char *data, struct scene_context *out)
{
	char *foreground = strsep(&data, "\t");
	char *visible = strsep(&data, "\t");
	char *screen = strsep(&data, "\t\n");
	char *game = strsep(&data, "\n");
	const char *p;
	size_t token = 0;

	if (!foreground || !visible || !screen || !*foreground || !*visible ||
	    (data && *data) || (strcmp(screen, "0") && strcmp(screen, "1")) ||
	    (game && *game && strcmp(game, "0") && strcmp(game, "1")))
		return -EINVAL;
	if (strcmp(foreground, "-")) {
		for (p = foreground; *p; p++)
			if (!package_char(*p))
				return -EINVAL;
		if (strscpy(out->foreground, foreground, sizeof(out->foreground)) < 0)
			return -E2BIG;
	}
	if (strcmp(visible, "-")) {
		for (p = visible; *p; p++) {
			if (*p == ',' && token) {
				token = 0;
				continue;
			}
			if (!package_char(*p) || ++token >= PROCESS_NAME_LEN)
				return -EINVAL;
		}
		if (!token)
			return -EINVAL;
		if (strscpy(out->visible, visible, sizeof(out->visible)) < 0)
			return -E2BIG;
	}
	out->display_off = *screen == '1';
	out->game = game && *game == '1' && out->foreground[0] && !out->display_off;
	return 0;
}

static bool same_package(const char *process, const char *package, size_t len)
{
	return len && !strncmp(process, package, len) &&
		(!process[len] || process[len] == ':');
}

static bool adaptive_in_scope(const struct affinity_policy *policy,
			      struct task_struct *task, const char *process)
{
	const char *scope = policy->adaptive_scope;
	unsigned int uid;

	if (!*scope)
		return false;
	if (strcmp(scope, "*"))
		return has_glob(scope) ? glob_match(scope, process) :
			same_package(process, scope, strlen(scope));

	/* The default adaptive scope is primary-user apps. Explicit rules can
	 * still target shared system UIDs, profiles and native services.
	 */
	uid = from_kuid(&init_user_ns, task_uid(task));
	return uid >= 10000 && uid < 100000 && strchr(process, '.');
}

static enum app_stage scene_stage(const struct scene_context *scene, const char *process)
{
	const char *name;
	const char *end;

	if (scene->display_off)
		return APP_DISPLAY_OFF;
	if (same_package(process, scene->foreground, strlen(scene->foreground)))
		return APP_FOREGROUND;
	for (name = scene->visible; *name; name = end + 1) {
		end = strchr(name, ',');
		if (same_package(process, name, end ? end - name : strlen(name)))
			return APP_FOREGROUND;
		if (!end)
			break;
	}
	return scene->foreground[0] || scene->visible[0] ? APP_BACKGROUND : APP_STATIC;
}

static bool starts_with(const char *name, const char *prefix)
{
	return !strncmp(name, prefix, strlen(prefix));
}

static bool contains_word(const char *name, const char *word)
{
	size_t length = strlen(word);

	for (; *name; name++)
		if (!strncasecmp(name, word, length))
			return true;
	return false;
}

static bool critical_runtime_thread(const char *name)
{
	return !strcmp(name, "Jit thread pool") || !strcmp(name, "HeapTaskDaemon") ||
		!strcmp(name, "FinalizerWatchd") || !strcmp(name, "FinalizerDaemon") ||
		!strcmp(name, "ReferenceQueueD") ||
		starts_with(name, "Binder:") || starts_with(name, "binder:") ||
		starts_with(name, "HwBinder:") || starts_with(name, "hwbinder:") ||
		starts_with(name, "vndbinder:") || strstr(name, "-binder-");
}

static bool is_sync_thread(const char *name)
{
	const char *p;

	for (p = name; *p; p++)
		if (!strncasecmp(p, "sync", 4) &&
		    (p == name || tolower(p[-1]) != 'a'))
			return true;
	return false;
}

static enum thread_role thread_role(struct task_struct *task, const char *name)
{
	if (task_pid_nr(task) == task_tgid_nr(task) ||
	    starts_with(name, "RenderThread") || strstr(name, "GLThread") ||
	    contains_word(name, "input") || contains_word(name, "surface") ||
	    starts_with(name, "UnityGfx") || !strcmp(name, "UnityMain") ||
	    !strcmp(name, "GameThread") || !strcmp(name, "RHIThread") ||
	    !strcmp(name, "Chrome_IOThread") || !strcmp(name, "UnityChoreograp") ||
	    !strcmp(name, "UEGameThread") || !strcmp(name, "MainThread-UE4"))
		return ROLE_INTERACTIVE;
	if (contains_word(name, "mediacodec") || contains_word(name, "audiotrack") ||
	    contains_word(name, "exoplayer") || contains_word(name, "ccodec") ||
	    contains_word(name, "codeclooper") || contains_word(name, "ijk") ||
	    contains_word(name, "dex2oat") || contains_word(name, "finalizer") ||
	    contains_word(name, "referencequeue") ||
	    !strcmp(name, "Profile Saver"))
		return ROLE_RUNTIME;
	if (contains_word(name, "analytics") || contains_word(name, "telemetry") ||
	    contains_word(name, "upload") || is_sync_thread(name) ||
	    contains_word(name, "push") || contains_word(name, "idle") ||
	    contains_word(name, "watchdog") || contains_word(name, "report") ||
	    contains_word(name, "crash") || contains_word(name, "bugly") ||
	    contains_word(name, "beacon"))
		return ROLE_IDLE;
	return ROLE_WORKER;
}

static const struct cpumask *adaptive_mask(struct task_struct *task, enum app_stage stage,
					   const char *name, const struct affinity_match *match)
{
	const struct affinity_rule *rule = match->rule;
	bool specific = rule && match->selector->scope != PROCESS_GLOBAL;
	enum thread_role role;

	if (stage == APP_STATIC || (rule && rule->source >= 3))
		return NULL;
	if (critical_runtime_thread(name))
		return stage == APP_FOREGROUND ? &interactive_cpus : &little_cpus;
	if (specific && rule->thread[0])
		return NULL;
	role = thread_role(task, name);
	if (stage == APP_FOREGROUND) {
		if (role == ROLE_INTERACTIVE)
			return specific ? NULL : &interactive_cpus;
		return role == ROLE_IDLE && !contains_word(name, "push") ? &tiny_cpus : &little_cpus;
	}
	if (role == ROLE_INTERACTIVE && stage == APP_BACKGROUND)
		return specific ? NULL : &background_cpus;
	return role <= ROLE_RUNTIME || contains_word(name, "push") ? &little_cpus : &tiny_cpus;
}

#endif
