#include <linux/err.h>
#include <linux/psi.h>
#include <linux/workqueue.h>

#include "xcy_reclaim_psi.h"

static int xcy_reclaim_psi_wake(wait_queue_entry_t *wait, unsigned int mode,
				int flags, void *key)
{
	struct xcy_reclaim_psi *monitor = container_of(wait, struct xcy_reclaim_psi, wait);

	if (!xchg(&monitor->trigger->event, 0))
		return 0;
	queue_work(system_freezable_power_efficient_wq, monitor->work);
	return 1;
}

/* The caller owns the work item and serializes start and stop. The kernel
 * uses the authenticated publisher file only as PSI's credential source. */
int xcy_reclaim_psi_start(struct xcy_reclaim_psi *monitor, struct file *publisher,
			 struct work_struct *work)
{
	char threshold[] = "some 150000 1000000";
	struct psi_trigger *trigger;

	trigger = psi_trigger_create(&psi_system, threshold, PSI_MEM, publisher, NULL);
	if (IS_ERR(trigger))
		return PTR_ERR(trigger);
	monitor->trigger = trigger;
	monitor->work = work;
	init_waitqueue_func_entry(&monitor->wait, xcy_reclaim_psi_wake);
	add_wait_queue(&trigger->event_wait, &monitor->wait);
	/* Catch a threshold breach between trigger creation and waiter install. */
	xcy_reclaim_psi_wake(&monitor->wait, 0, 0, NULL);
	return 0;
}

void xcy_reclaim_psi_stop(struct xcy_reclaim_psi *monitor)
{
	struct psi_trigger *trigger = monitor->trigger;

	if (!trigger)
		return;
	remove_wait_queue(&trigger->event_wait, &monitor->wait);
	psi_trigger_destroy(trigger);
	monitor->trigger = NULL;
}
