#include <linux/errno.h>
#include <linux/jiffies.h>
#include <linux/ktime.h>
#include <linux/mm.h>

#include "xcy_reclaim_runtime.h"

/* Opt13 uses a 3 s transport timeout. A timeout never authorizes a signal. */
#define XCY_FRAMEWORK_REQUEST_MS 3000

static s64 xcy_now_ms(void)
{
	return ktime_to_ms(ktime_get_boottime());
}

static xcy_system_pressure_level_t xcy_ram_pressure(void)
{
	return xcy_system_ram_pressure_classify(totalram_pages(), si_mem_available());
}

static void xcy_schedule_deadline(struct xcy_reclaim_runtime *runtime, s64 now)
{
	s64 deadline = runtime->pending ? runtime->expires_ms :
		xcy_system_reclaim_deadline(&runtime->policy);

	/* Rate wait gates the next pressure event. It does not start another
	 * scan by itself after an empty candidate set or a lost publisher. */
	if (!deadline || (!runtime->pending && runtime->policy.phase == XCY_SYSTEM_RECLAIM_RATE_WAIT)) {
		cancel_delayed_work(&runtime->deadline);
		return;
	}
	mod_delayed_work(system_freezable_power_efficient_wq, &runtime->deadline,
			 msecs_to_jiffies(max_t(s64, 1, deadline - now)));
}

static void xcy_retry_later(struct xcy_reclaim_runtime *runtime, s64 now)
{
	runtime->policy.phase = XCY_SYSTEM_RECLAIM_RATE_WAIT;
	runtime->policy.episode_actions = 0;
	xcy_system_reclaim_set_deadline(&runtime->policy, now, XCY_SYSTEM_RECLAIM_EPISODE_GAP_MS);
}

static void xcy_observe(struct xcy_reclaim_runtime *runtime)
{
	xcy_system_reclaim_decision_t decision;
	xcy_system_pressure_level_t pressure;
	s64 now;

	mutex_lock(&runtime->lock);
	if (!runtime->running)
		goto out;
	now = xcy_now_ms();
	pressure = xcy_ram_pressure();
	if (runtime->pending) {
		if (pressure != XCY_SYSTEM_PRESSURE_CRITICAL) {
			runtime->pending = 0;
			xcy_system_reclaim_observe(&runtime->policy, pressure, now);
		} else if (now >= runtime->expires_ms) {
			runtime->pending = 0;
			xcy_retry_later(runtime, now);
		}
		goto schedule;
	}
	decision = xcy_reclaim_observe_without_pageout(&runtime->policy, pressure, now);
	if (decision == XCY_SYSTEM_RECLAIM_READY) {
		/* Sequence zero is reserved for no outstanding request. */
		if (!++runtime->sequence)
			++runtime->sequence;
		runtime->pending = runtime->sequence;
		runtime->expires_ms = now + XCY_FRAMEWORK_REQUEST_MS;
		if (runtime->request_state(runtime->owner, runtime->pending, runtime->expires_ms)) {
			runtime->pending = 0;
			xcy_retry_later(runtime, now);
		}
	}
schedule:
	xcy_schedule_deadline(runtime, now);
out:
	mutex_unlock(&runtime->lock);
}

static void xcy_pressure_work(struct work_struct *work)
{
	struct xcy_reclaim_runtime *runtime = container_of(work, struct xcy_reclaim_runtime, pressure);

	xcy_observe(runtime);
}

static void xcy_deadline_work(struct work_struct *work)
{
	struct xcy_reclaim_runtime *runtime = container_of(to_delayed_work(work),
							  struct xcy_reclaim_runtime, deadline);

	xcy_observe(runtime);
}

int xcy_reclaim_runtime_start(struct xcy_reclaim_runtime *runtime, struct file *publisher,
			      int (*request_state)(void *owner, u64 request, s64 expires_ms),
			      void *owner)
{
	int error;

	if (!publisher || !request_state)
		return -EINVAL;
	mutex_init(&runtime->lock);
	INIT_WORK(&runtime->pressure, xcy_pressure_work);
	INIT_DELAYED_WORK(&runtime->deadline, xcy_deadline_work);
	runtime->request_state = request_state;
	runtime->owner = owner;
	runtime->running = true;
	error = xcy_reclaim_psi_start(&runtime->psi, publisher, &runtime->pressure);
	if (error) {
		runtime->running = false;
		return error;
	}
	xcy_reclaim_runtime_notify(runtime);
	return 0;
}

void xcy_reclaim_runtime_notify(struct xcy_reclaim_runtime *runtime)
{
	queue_work(system_freezable_power_efficient_wq, &runtime->pressure);
}

void xcy_reclaim_runtime_stop(struct xcy_reclaim_runtime *runtime)
{
	mutex_lock(&runtime->lock);
	runtime->running = false;
	runtime->pending = 0;
	mutex_unlock(&runtime->lock);
	xcy_reclaim_psi_stop(&runtime->psi);
	cancel_work_sync(&runtime->pressure);
	cancel_delayed_work_sync(&runtime->deadline);
}

int xcy_reclaim_runtime_finish(struct xcy_reclaim_runtime *runtime, u64 request,
			       enum xcy_reclaim_outcome outcome)
{
	s64 now;
	int error = 0;

	mutex_lock(&runtime->lock);
	now = xcy_now_ms();
	if (!request || !runtime->running || runtime->pending != request || now >= runtime->expires_ms) {
		error = -ESTALE;
		goto out;
	}
	if (outcome < XCY_RECLAIM_NO_CANDIDATE || outcome > XCY_RECLAIM_ACTION_SENT) {
		error = -EINVAL;
		goto out;
	}
	runtime->pending = 0;
	if (outcome == XCY_RECLAIM_NO_CANDIDATE || outcome == XCY_RECLAIM_SAMPLED_RETRY)
		xcy_retry_later(runtime, now);
	else
		xcy_system_reclaim_complete(&runtime->policy, now, outcome == XCY_RECLAIM_ACTION_SENT);
	xcy_schedule_deadline(runtime, now);
out:
	mutex_unlock(&runtime->lock);
	return error;
}
