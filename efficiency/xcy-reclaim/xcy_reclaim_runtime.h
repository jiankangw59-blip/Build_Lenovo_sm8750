#ifndef XCY_RECLAIM_RUNTIME_H
#define XCY_RECLAIM_RUNTIME_H

#include <linux/mutex.h>
#include <linux/workqueue.h>

#include "xcy_reclaim_psi.h"
#include "xcy_reclaim_state.h"

enum xcy_reclaim_outcome {
	XCY_RECLAIM_NO_CANDIDATE,
	XCY_RECLAIM_SAMPLED_RETRY,
	XCY_RECLAIM_ACTION_FAILED,
	XCY_RECLAIM_ACTION_SENT,
};

struct xcy_reclaim_runtime {
	struct mutex lock;
	struct xcy_reclaim_psi psi;
	struct work_struct pressure;
	struct delayed_work deadline;
	xcy_system_reclaim_episode_t policy;
	int (*request_state)(void *owner, u64 request, s64 expires_ms);
	void *owner;
	u64 sequence;
	u64 pending;
	s64 expires_ms;
	bool running;
};

/* Start/stop are serialized by the caller. The request callback is invoked
 * under the runtime mutex and may wake the publisher without waiting for it. */
int xcy_reclaim_runtime_start(struct xcy_reclaim_runtime *runtime, struct file *publisher,
			      int (*request_state)(void *owner, u64 request, s64 expires_ms),
			      void *owner);
void xcy_reclaim_runtime_stop(struct xcy_reclaim_runtime *runtime);
void xcy_reclaim_runtime_notify(struct xcy_reclaim_runtime *runtime);
int xcy_reclaim_runtime_finish(struct xcy_reclaim_runtime *runtime, u64 request,
			       enum xcy_reclaim_outcome outcome);
int xcy_reclaim_runtime_renew(struct xcy_reclaim_runtime *runtime, u64 request,
			      s64 *expires_ms);
int xcy_reclaim_runtime_act(struct xcy_reclaim_runtime *runtime, u64 request,
			     enum xcy_reclaim_outcome (*act)(void *context), void *context);

#endif
