/* SPDX-License-Identifier: GPL-2.0 */
#ifndef ACE6_AFFINITY_RULES_H
#define ACE6_AFFINITY_RULES_H

#include <linux/cpumask.h>
#include <linux/glob.h>
#include <linux/jhash.h>
#include <linux/list.h>
#include <linux/slab.h>
#include <linux/string.h>

#define PROCESS_NAME_LEN 128
#define THREAD_PATTERN_LEN 64
#define RULE_BUCKETS 256
#define THREAD_BUCKETS 16
#define MAX_PROFILES 512
#define MAX_RULES 8192
#define MAX_RULE_BYTES (512 * 1024)

enum match_scope {
	PROCESS_EXACT,
	PROCESS_PATTERN,
	PROCESS_GLOBAL,
	PROCESS_SCOPES,
};

struct affinity_rule {
	struct hlist_node node;
	cpumask_t mask;
	char thread[THREAD_PATTERN_LEN];
	unsigned int index;
	unsigned int thread_score;
	unsigned int thread_order;
	unsigned int source;
	bool manual;
};

struct affinity_thread_set {
	struct hlist_head named[THREAD_BUCKETS];
	struct hlist_head fallback;
	unsigned int fallback_max[PROCESS_SCOPES];
};

struct affinity_selector {
	struct list_head owned;
	struct hlist_node node;
	struct affinity_thread_set threads;
	struct list_head bindings;
	char name[PROCESS_NAME_LEN];
	size_t key_length;
	unsigned int score;
	enum match_scope scope;
};

struct affinity_profile {
	struct list_head owned;
	struct affinity_thread_set threads;
	unsigned int count;
};

struct affinity_binding {
	struct list_head node;
	const struct affinity_profile *profile;
	unsigned int first_index;
};

struct affinity_policy {
	struct hlist_head exact[RULE_BUCKETS];
	struct hlist_head child[RULE_BUCKETS];
	struct hlist_head patterns;
	struct hlist_head global;
	struct list_head selectors;
	struct list_head profiles;
	char adaptive_scope[PROCESS_NAME_LEN];
	unsigned int count;
	unsigned int stored_rules;
	unsigned int wire_lines;
	unsigned int profile_count;
	unsigned int binding_count;
	struct affinity_rule rules[];
};

struct affinity_match {
	const struct affinity_rule *rule;
	const struct affinity_selector *selector;
	unsigned int score;
	unsigned int index;
};

static bool has_glob(const char *pattern)
{
	return strpbrk(pattern, "*?[") != NULL;
}

static unsigned int specificity(const char *pattern)
{
	unsigned int chars = 0;
	unsigned int base = 100;

	if (!*pattern)
		return 0;
	if (!has_glob(pattern))
		return 1000 + strlen(pattern);
	if (strchr(pattern, '['))
		base = 500;
	else if (strchr(pattern, '?'))
		base = 300;
	for (; *pattern; pattern++) {
		if (*pattern != '*' && *pattern != '?' && *pattern != '[')
			chars++;
	}
	return base + chars;
}

static unsigned int process_bucket(const char *name, size_t len)
{
	return jhash(name, len, 0) % RULE_BUCKETS;
}

static unsigned int thread_bucket(const char *name)
{
	return jhash(name, strlen(name), 0) % THREAD_BUCKETS;
}

/* AppOpt visits these wildcard classes in this order on equal scores. */
static unsigned int wildcard_order(const char *pattern)
{
	const char *first = strchr(pattern, '*');
	const char *second;

	if (strchr(pattern, '?') || strchr(pattern, '['))
		return 5;
	if (!first)
		return 0;
	second = strchr(first + 1, '*');
	if (second)
		return first == pattern && second > first + 1 && !second[1] ? 3 : 5;
	if (first == pattern)
		return first[1] ? 2 : 3;
	return first[1] ? 4 : 1;
}

static struct affinity_selector *get_selector(struct affinity_policy *policy, const char *name)
{
	struct affinity_selector *selector;
	struct hlist_head *head;
	const char *glob = strpbrk(name, "*?[");
	size_t len = strlen(name);
	size_t key_length = len;
	enum match_scope scope;

	if (len >= PROCESS_NAME_LEN)
		return ERR_PTR(-E2BIG);
	if (!strcmp(name, "*")) {
		head = &policy->global;
		scope = PROCESS_GLOBAL;
	} else if (!glob) {
		head = &policy->exact[process_bucket(name, len)];
		scope = PROCESS_EXACT;
	} else {
		scope = PROCESS_PATTERN;
		if (len > 2 && strchr(name, ':') == name + len - 2 &&
		    name[len - 1] == '*' && glob == name + len - 1) {
			key_length = len - 2;
			head = &policy->child[process_bucket(name, key_length)];
		} else {
			head = &policy->patterns;
		}
	}
	hlist_for_each_entry(selector, head, node) {
		if (!strcmp(selector->name, name))
			return selector;
	}
	selector = kzalloc(sizeof(*selector), GFP_KERNEL);
	if (!selector)
		return ERR_PTR(-ENOMEM);
	memcpy(selector->name, name, len + 1);
	selector->key_length = key_length;
	selector->scope = scope;
	selector->score = specificity(name);
	INIT_LIST_HEAD(&selector->bindings);
	hlist_add_head(&selector->node, head);
	list_add(&selector->owned, &policy->selectors);
	return selector;
}

static int parse_rule_fields(struct affinity_rule *rule, char *name, char **process)
{
	char *comment, *mask, *thread, *end;
	int ret;

	mask = strchr(name, '=');
	if (!mask)
		return -EINVAL;
	*mask++ = '\0';
	comment = strchr(mask, '#');
	if (comment)
		*comment = '\0';
	ret = cpulist_parse(strim(mask), &rule->mask);
	if (ret || cpumask_empty(&rule->mask) ||
	    !cpumask_subset(&rule->mask, cpu_possible_mask))
		return -EINVAL;

	thread = strchr(name, '{');
	if (thread) {
		*thread++ = '\0';
		end = strchr(thread, '}');
		if (!end)
			return -EINVAL;
		*end++ = '\0';
		if (*strim(end))
			return -EINVAL;
		if (strscpy(rule->thread, strim(thread), sizeof(rule->thread)) < 0)
			return -E2BIG;
	}
	*process = strim(name);
	rule->thread_score = specificity(rule->thread);
	rule->thread_order = wildcard_order(rule->thread);
	return 0;
}

static unsigned int thread_weight(enum match_scope scope, const struct affinity_rule *rule)
{
	if (scope == PROCESS_PATTERN)
		return rule->thread_score + (rule->thread[0] ? 4000000 : 0);
	if (scope == PROCESS_EXACT && rule->manual)
		return 20000000 + rule->thread_score * 8;
	return rule->thread_score * 8 + rule->thread_order;
}

static void add_thread_rule(struct affinity_thread_set *threads, struct affinity_rule *rule)
{
	enum match_scope scope;

	if (rule->thread_score >= 1000) {
		hlist_add_head(&rule->node, &threads->named[thread_bucket(rule->thread)]);
		return;
	}
	hlist_add_head(&rule->node, &threads->fallback);
	for (scope = 0; scope < PROCESS_SCOPES; scope++)
		threads->fallback_max[scope] = max(threads->fallback_max[scope], thread_weight(scope, rule));
}

static int parse_rule(struct affinity_policy *policy, char *line)
{
	struct affinity_rule *rule = &policy->rules[policy->stored_rules];
	struct affinity_selector *selector;
	char *process;
	int ret;

	if (line[0] < '0' || line[0] > '4' || line[1] != '\t')
		return -EINVAL;
	if (policy->count == MAX_RULES)
		return -E2BIG;
	rule->source = line[0] - '0';
	rule->manual = rule->source == 4;
	ret = parse_rule_fields(rule, line + 2, &process);
	if (ret)
		return ret;
	if (!*process)
		return -EINVAL;
	selector = get_selector(policy, process);
	if (IS_ERR(selector))
		return PTR_ERR(selector);
	rule->index = policy->count++;
	policy->stored_rules++;
	add_thread_rule(&selector->threads, rule);
	return 0;
}

static int define_profile(struct affinity_policy *policy, struct affinity_profile **profiles,
			  struct affinity_profile **current_profile, const char *text)
{
	struct affinity_profile *profile;
	unsigned int id;

	if (kstrtouint(text, 10, &id) || !id || id >= MAX_PROFILES || profiles[id])
		return -EINVAL;
	profile = kzalloc(sizeof(*profile), GFP_KERNEL);
	if (!profile)
		return -ENOMEM;
	list_add(&profile->owned, &policy->profiles);
	profiles[id] = profile;
	*current_profile = profile;
	policy->profile_count++;
	return 0;
}

static int bind_profile(struct affinity_policy *policy, struct affinity_profile **profiles,
			char *text)
{
	char *number = strsep(&text, " ");
	struct affinity_profile *profile;
	struct affinity_binding *binding;
	struct affinity_selector *selector;
	unsigned int id;

	if (!text || !*text || strpbrk(text, " \t\r{}=") ||
	    kstrtouint(number, 10, &id) || !id || id >= MAX_PROFILES || !profiles)
		return -EINVAL;
	profile = profiles[id];
	if (!profile || !profile->count)
		return -EINVAL;
	if (profile->count > MAX_RULES - policy->count)
		return -E2BIG;
	selector = get_selector(policy, text);
	if (IS_ERR(selector))
		return PTR_ERR(selector);
	binding = kmalloc(sizeof(*binding), GFP_KERNEL);
	if (!binding)
		return -ENOMEM;
	binding->profile = profile;
	binding->first_index = policy->count;
	policy->count += profile->count;
	policy->binding_count++;
	list_add(&binding->node, &selector->bindings);
	return 0;
}

static int parse_profile_rule(struct affinity_policy *policy, struct affinity_profile *profile,
			      char *line)
{
	struct affinity_rule *rule = &policy->rules[policy->stored_rules];
	char *process;
	int ret;

	if (!profile)
		return -EINVAL;
	ret = parse_rule_fields(rule, line, &process);
	if (ret || *process)
		return ret ?: -EINVAL;
	rule->source = 1;
	rule->index = profile->count++;
	policy->stored_rules++;
	add_thread_rule(&profile->threads, rule);
	return 0;
}

static void free_policy(struct affinity_policy *policy)
{
	struct affinity_selector *selector, *next;
	struct affinity_profile *profile, *next_profile;

	if (!policy)
		return;
	list_for_each_entry_safe(selector, next, &policy->selectors, owned) {
		struct affinity_binding *binding, *next_binding;

		list_for_each_entry_safe(binding, next_binding, &selector->bindings, node) {
			list_del(&binding->node);
			kfree(binding);
		}
		list_del(&selector->owned);
		kfree(selector);
	}
	list_for_each_entry_safe(profile, next_profile, &policy->profiles, owned) {
		list_del(&profile->owned);
		kfree(profile);
	}
	kvfree(policy);
}

static struct affinity_policy *parse_policy(char *data, size_t size)
{
	struct affinity_policy *policy;
	struct affinity_profile **profiles = NULL, *current_profile = NULL;
	unsigned int lines = 0, slots = 0;
	char *line;
	size_t i;
	int ret;

	for (i = 0; i < size; i++) {
		if (i && data[i - 1] != '\n')
			continue;
		lines++;
		if (data[i] != '\n' && data[i] != '@' && data[i] != '?')
			slots++;
	}
	if (lines > MAX_RULES)
		return ERR_PTR(-E2BIG);
	policy = kvzalloc(struct_size(policy, rules, slots), GFP_KERNEL);
	if (!policy)
		return ERR_PTR(-ENOMEM);
	INIT_LIST_HEAD(&policy->selectors);
	INIT_LIST_HEAD(&policy->profiles);
	while ((line = strsep(&data, "\n")) != NULL) {
		if (!*line)
			continue;
		if (!strncmp(line, "?adapt\t", 7) && !policy->wire_lines && !policy->adaptive_scope[0]) {
			const char *scope = line + 7;

			if (!*scope || strpbrk(scope, " \t\r") ||
			    strscpy(policy->adaptive_scope, scope, sizeof(policy->adaptive_scope)) < 0) {
				ret = -EINVAL;
				goto invalid;
			}
			continue;
		}
		policy->wire_lines++;
		if (!strncmp(line, "@profile ", 9)) {
			if (!profiles)
				profiles = kcalloc(MAX_PROFILES, sizeof(*profiles), GFP_KERNEL);
			if (!profiles) {
				ret = -ENOMEM;
				goto invalid;
			}
			ret = define_profile(policy, profiles, &current_profile, line + 9);
		} else if (*line == '{' || *line == '=') {
			ret = parse_profile_rule(policy, current_profile, line);
		} else {
			current_profile = NULL;
			if (!strncmp(line, "@use ", 5))
				ret = bind_profile(policy, profiles, line + 5);
			else
				ret = parse_rule(policy, line);
		}
		if (ret)
			goto invalid;
	}
	if ((policy->wire_lines || policy->adaptive_scope[0]) && !policy->count) {
		ret = -EINVAL;
		goto invalid;
	}
	kfree(profiles);
	return policy;
invalid:
	kfree(profiles);
	free_policy(policy);
	return ERR_PTR(ret);
}

static bool outranks(const struct affinity_match *best, const struct affinity_selector *selector,
		     unsigned int score, unsigned int index)
{
	if (!best->rule || score != best->score)
		return !best->rule || score > best->score;
	return selector->scope == PROCESS_PATTERN ? index < best->index : index > best->index;
}

static void match_threads(const struct affinity_selector *selector,
			  const struct affinity_thread_set *threads,
			  const char *thread, unsigned int bucket, unsigned int base,
			  struct affinity_match *best)
{
	const struct affinity_rule *rule;
	enum match_scope scope = selector->scope;
	unsigned int process_weight = scope == PROCESS_PATTERN ? selector->score * 4096 : 0;

	hlist_for_each_entry(rule, &threads->named[bucket], node) {
		unsigned int score = process_weight + thread_weight(scope, rule);
		unsigned int index = base + rule->index;

		if (outranks(best, selector, score, index) && !strcmp(rule->thread, thread))
			*best = (struct affinity_match){ rule, selector, score, index };
	}
	if (best->rule && process_weight + threads->fallback_max[scope] < best->score)
		return;
	hlist_for_each_entry(rule, &threads->fallback, node) {
		unsigned int score = process_weight + thread_weight(scope, rule);
		unsigned int index = base + rule->index;

		if (outranks(best, selector, score, index) &&
		    (!rule->thread[0] || glob_match(rule->thread, thread)))
			*best = (struct affinity_match){ rule, selector, score, index };
	}
}

static void match_selector(const struct affinity_selector *selector, const char *thread,
			   unsigned int bucket, struct affinity_match *best)
{
	const struct affinity_binding *binding;

	match_threads(selector, &selector->threads, thread, bucket, 0, best);
	list_for_each_entry(binding, &selector->bindings, node)
		match_threads(selector, &binding->profile->threads, thread, bucket,
			      binding->first_index, best);
}

static struct affinity_match select_rule(const struct affinity_policy *p,
					 const char *process, const char *thread)
{
	const struct affinity_selector *selector;
	struct affinity_match best = {};
	const char *child = strchr(process, ':');
	size_t length = strlen(process);
	unsigned int bucket = thread_bucket(thread);

	hlist_for_each_entry(selector, &p->exact[process_bucket(process, length)], node) {
		if (selector->key_length != length || strcmp(selector->name, process))
			continue;
		match_selector(selector, thread, bucket, &best);
		if (best.rule)
			return best;
		break;
	}
	if (child) {
		length = child - process;
		hlist_for_each_entry(selector, &p->child[process_bucket(process, length)], node) {
			if (glob_match(selector->name, process))
				match_selector(selector, thread, bucket, &best);
		}
	}
	hlist_for_each_entry(selector, &p->patterns, node) {
		if (glob_match(selector->name, process))
			match_selector(selector, thread, bucket, &best);
	}
	if (best.rule)
		return best;
	hlist_for_each_entry(selector, &p->global, node)
		match_selector(selector, thread, bucket, &best);
	return best;
}

#endif
