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
#define MAX_RULES 8192
#define MAX_RULE_BYTES (512 * 1024)

struct affinity_rule {
	struct hlist_node node;
	cpumask_t mask;
	char process[PROCESS_NAME_LEN];
	char thread[THREAD_PATTERN_LEN];
	unsigned int index;
	unsigned int process_score;
	unsigned int thread_score;
	unsigned int thread_order;
	unsigned int source;
	bool manual;
};

struct affinity_policy {
	struct hlist_head exact[RULE_BUCKETS];
	struct hlist_head child[RULE_BUCKETS];
	struct hlist_head patterns;
	struct hlist_head global;
	char adaptive_scope[PROCESS_NAME_LEN];
	unsigned int count;
	struct affinity_rule rules[];
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

static int parse_rule(struct affinity_policy *policy, char *line)
{
	struct affinity_rule *rule = &policy->rules[policy->count];
	struct hlist_head *head;
	char *comment, *name, *mask, *thread, *end;
	ssize_t len;
	int ret;

	if (line[0] < '0' || line[0] > '4' || line[1] != '\t')
		return -EINVAL;
	rule->source = line[0] - '0';
	rule->manual = rule->source == 4;
	name = line + 2;
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
	name = strim(name);
	if (!*name)
		return -EINVAL;
	len = strscpy(rule->process, name, sizeof(rule->process));
	if (len < 0)
		return len;
	if (!strcmp(name, "*")) {
		head = &policy->global;
	} else if (!has_glob(name)) {
		head = &policy->exact[process_bucket(name, len)];
	} else if (len > 2 && strchr(name, ':') == name + len - 2 && name[len - 1] == '*' &&
		   strpbrk(name, "*?[") == name + len - 1) {
		head = &policy->child[process_bucket(name, len - 2)];
	} else {
		head = &policy->patterns;
	}
	rule->process_score = specificity(name);
	rule->thread_score = specificity(rule->thread);
	rule->thread_order = wildcard_order(rule->thread);
	rule->index = policy->count++;
	hlist_add_head(&rule->node, head);
	return 0;
}

static struct affinity_policy *parse_policy(char *data, size_t size)
{
	struct affinity_policy *policy;
	unsigned int lines = 0;
	char *line;
	size_t i;
	int ret;

	for (i = 0; i < size; i++)
		lines += data[i] == '\n';
	if (size && data[size - 1] != '\n')
		lines++;
	if (lines > MAX_RULES)
		return ERR_PTR(-E2BIG);
	policy = kvzalloc(struct_size(policy, rules, lines), GFP_KERNEL);
	if (!policy)
		return ERR_PTR(-ENOMEM);
	while ((line = strsep(&data, "\n")) != NULL) {
		if (!*line)
			continue;
		if (!strncmp(line, "?adapt\t", 7) && !policy->count && !policy->adaptive_scope[0]) {
			const char *scope = line + 7;

			if (!*scope || strpbrk(scope, " \t\r") ||
			    strscpy(policy->adaptive_scope, scope, sizeof(policy->adaptive_scope)) < 0) {
				ret = -EINVAL;
				goto invalid;
			}
			continue;
		}
		ret = parse_rule(policy, line);
		if (ret)
			goto invalid;
	}
	if (policy->adaptive_scope[0] && !policy->count) {
		ret = -EINVAL;
		goto invalid;
	}
	return policy;
invalid:
	kvfree(policy);
	return ERR_PTR(ret);
}

struct affinity_match {
	const struct affinity_rule *rule;
	unsigned int score;
};

enum match_scope {
	PROCESS_EXACT,
	PROCESS_PATTERN,
	PROCESS_GLOBAL,
};

static void match_bucket(const struct hlist_head *head, const char *process,
			 const char *thread, enum match_scope scope, struct affinity_match *match)
{
	const struct affinity_rule *rule;

	hlist_for_each_entry(rule, head, node) {
		unsigned int score;
		bool preferred;

		if (scope == PROCESS_EXACT) {
			if (strcmp(rule->process, process))
				continue;
		} else if (scope == PROCESS_PATTERN) {
			if (!glob_match(rule->process, process))
				continue;
		}
		if (rule->thread[0] && (rule->thread_score >= 1000 ?
		    strcmp(rule->thread, thread) != 0 :
		    !glob_match(rule->thread, thread)))
			continue;

		if (scope == PROCESS_PATTERN) {
			score = rule->process_score * 4096 + rule->thread_score;
			if (rule->thread[0])
				score += 4000000;
			preferred = !match->rule || rule->index < match->rule->index;
		} else {
			score = rule->thread_score * 8 + rule->thread_order;
			if (scope == PROCESS_EXACT && rule->manual)
				score = 20000000 + rule->thread_score * 8;
			preferred = !match->rule || rule->index > match->rule->index;
		}
		if (!match->rule || score > match->score ||
		    (score == match->score && preferred)) {
			match->score = score;
			match->rule = rule;
		}
	}
}

static const struct affinity_rule *select_rule(const struct affinity_policy *p,
					       const char *process, const char *thread)
{
	struct affinity_match match = {};
	const char *child = strchr(process, ':');

	match_bucket(&p->exact[process_bucket(process, strlen(process))],
		     process, thread, PROCESS_EXACT, &match);
	if (match.rule)
		return match.rule;
	if (child)
		match_bucket(&p->child[process_bucket(process, child - process)],
			     process, thread, PROCESS_PATTERN, &match);
	match_bucket(&p->patterns, process, thread, PROCESS_PATTERN, &match);
	if (match.rule)
		return match.rule;
	match_bucket(&p->global, process, thread, PROCESS_GLOBAL, &match);
	return match.rule;
}

#endif
