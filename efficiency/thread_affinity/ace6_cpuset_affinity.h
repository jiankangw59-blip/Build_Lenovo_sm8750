/* SPDX-License-Identifier: GPL-2.0 */
#ifndef _ACE6_CPUSET_AFFINITY_H
#define _ACE6_CPUSET_AFFINITY_H

#include <linux/tracepoint.h>

struct task_struct;

DECLARE_TRACE(ace6_cpuset_attach_task, TP_PROTO(struct task_struct *task),
	      TP_ARGS(task));

#endif
