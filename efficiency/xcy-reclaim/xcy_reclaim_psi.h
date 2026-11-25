#ifndef XCY_RECLAIM_PSI_H
#define XCY_RECLAIM_PSI_H

#include <linux/wait.h>

struct file;
struct psi_trigger;
struct work_struct;

struct xcy_reclaim_psi {
	struct psi_trigger *trigger;
	struct work_struct *work;
	wait_queue_entry_t wait;
};

int xcy_reclaim_psi_start(struct xcy_reclaim_psi *monitor, struct file *publisher,
			 struct work_struct *work);
void xcy_reclaim_psi_stop(struct xcy_reclaim_psi *monitor);

#endif
