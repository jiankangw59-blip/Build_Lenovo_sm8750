// SPDX-License-Identifier: GPL-2.0
/* Event-only Scene activity. No handler or timer survives disabled/off/game. */
#include <linux/cpufreq.h>
#include <linux/cpu.h>
#include <linux/input.h>
#include <linux/input/mt.h>
#include <linux/irq_work.h>
#include <linux/jiffies.h>
#include <linux/kernel_stat.h>
#include <linux/ktime.h>
#include <linux/list.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/tick.h>
#include <linux/workqueue.h>
#include "scene_activity_backend.h"
#include "scene_activity_load.h"
#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
#include <linux/completion.h>
#endif

struct sab_handle {
	struct input_handle handle;
	struct list_head node;
	u32 contacts;
	bool touch, reliable, ready, listed;
};

/* Lock order: input_dev.event_lock -> activity_lock; never the reverse. */
static DEFINE_SPINLOCK(activity_lock);
static DEFINE_MUTEX(activity_config_lock);
static LIST_HEAD(activity_handles);
static struct workqueue_struct *activity_wq;
static struct work_struct activity_work;
static struct delayed_work activity_deadline;
static struct irq_work activity_irq;
static struct sa_state activity_state;
static struct sa_config activity_config;
static struct sab_status activity_status;
static struct sa_cpu_counter *baseline, *current_samples;
static cpumask_t performance_cpus;
static void (*activity_notify)(u64 epoch, bool inactive);
static bool accepting, handler_registered;
static bool publish_pending, baseline_valid, retry_pending;
static u64 activity_sequence, baseline_sequence, armed_token, armed_epoch;
static u64 retry_sequence;
static unsigned int first_cpu;
static u32 contact_count, touch_devices, unreliable_devices;
#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
static void (*connect_before_open_hook)(struct input_dev *dev);
#endif

static u64 activity_now(void)
{
	return ktime_get_ns() / NSEC_PER_MSEC;
}

static bool input_reliable(void)
{
	return touch_devices && !unreliable_devices;
}

static void update_status(void)
{
	activity_status.epoch = activity_state.context.epoch;
	activity_status.phase = activity_state.phase;
	activity_status.contacts = contact_count;
	activity_status.reliable = input_reliable();
	activity_status.deadline_valid = activity_state.deadline_valid || retry_pending;
	activity_status.deadline_ms = activity_state.deadline_ms;
	if (accepting && !activity_status.reliable && !activity_status.error)
		activity_status.error = -ENODEV;
	else if (activity_status.reliable && activity_status.error == -ENODEV)
		activity_status.error = 0;
}

static void apply_input(enum sa_event_kind kind)
{
	struct sa_output out;
	struct sa_event event = {
		.kind = kind, .now_ms = activity_now(), .contacts = contact_count,
		.input_reliable = input_reliable(),
	};

	if (!accepting)
		return;
	activity_sequence++;
	retry_pending = false;
	sa_apply(&activity_state, &activity_config, &activity_state.context, &event, &out);
	/* Publish ACTIVE before releasing this lock. A newer input must invalidate
	 * an already queued INACTIVE core intent before the core can consume it.
	 * notify may only take frame_lock and queue irq_work, never a WQ pool lock.
	 */
	if (out.publish) {
		activity_status.transitions++;
		activity_notify(out.epoch, out.inactive);
		publish_pending = false;
	}
	activity_status.inputs++;
	update_status();
	/* input event_lock is held: bridge before taking any WQ pool lock. */
	irq_work_queue(&activity_irq);
}

static u32 device_contacts(struct input_dev *dev)
{
	u32 contacts = 0;
	unsigned int slot;

	if (dev->mt) {
		for (slot = 0; slot < dev->mt->num_slots; slot++)
			contacts += input_mt_is_active(&dev->mt->slots[slot]);
	} else if (test_bit(BTN_TOUCH, dev->keybit)) {
		contacts = test_bit(BTN_TOUCH, dev->key);
	}
	return contacts;
}

static void activity_events(struct input_handle *handle,
		const struct input_value *values, unsigned int count)
{
	struct sab_handle *entry = handle->private;
	unsigned long flags;
	unsigned int i;
	u32 contacts;
	bool meaningful = false;

	/* input_open_device may dispatch events before connect finishes. Both this
	 * read and the post-open resync/ready write hold input_dev.event_lock.
	 */
	if (!entry->ready)
		return;
	for (i = 0; i < count; i++)
		if (values[i].type == EV_KEY || values[i].type == EV_REL ||
		    (entry->touch && values[i].type == EV_ABS))
			meaningful = true;
	if (!meaningful)
		return;
	contacts = entry->touch ? device_contacts(handle->dev) : 0;
	spin_lock_irqsave(&activity_lock, flags);
	contact_count -= entry->contacts;
	entry->contacts = contacts;
	contact_count += contacts;
	/* Movement while a finger is held needs no timer, sample, or public update. */
	if (!accepting || (activity_state.phase == SA_ACTIVE && contact_count && activity_state.contacts &&
	    activity_state.input_reliable)) {
		spin_unlock_irqrestore(&activity_lock, flags);
		return;
	}
	apply_input(SA_INPUT);
	spin_unlock_irqrestore(&activity_lock, flags);
}

static bool activity_match(struct input_handler *handler, struct input_dev *dev)
{
	/* Ignore ABS-only sensors; keys and pointer events still restore active. */
	return (test_bit(EV_ABS, dev->evbit) &&
		(test_bit(ABS_MT_POSITION_X, dev->absbit) || test_bit(BTN_TOUCH, dev->keybit))) ||
		test_bit(EV_KEY, dev->evbit) || test_bit(EV_REL, dev->evbit);
}

static int activity_connect(struct input_handler *handler, struct input_dev *dev,
		const struct input_device_id *id)
{
	struct sab_handle *entry;
	unsigned long flags;
	int error;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;
	entry->handle.dev = dev;
	entry->handle.handler = handler;
	entry->handle.name = "scene-activity";
	entry->handle.private = entry;
	entry->touch = test_bit(ABS_MT_POSITION_X, dev->absbit) || test_bit(BTN_TOUCH, dev->keybit);
	/* Type A without BTN_TOUCH cannot prove that all contacts were released. */
	entry->reliable = !entry->touch || dev->mt || test_bit(BTN_TOUCH, dev->keybit);
	error = input_register_handle(&entry->handle);
	if (error)
		goto free;
#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
	if (connect_before_open_hook)
		connect_before_open_hook(dev);
#endif
	error = input_open_device(&entry->handle);
	if (error) {
		input_unregister_handle(&entry->handle);
		goto free;
	}
	/* Opening may start a driver and change contacts. Snapshot AFTER open,
	 * atomically with enabling callbacks; never trust a closed-handle snapshot.
	 */
	spin_lock_irqsave(&dev->event_lock, flags);
	spin_lock(&activity_lock);
	entry->contacts = entry->touch ? device_contacts(dev) : 0;
	contact_count += entry->contacts;
	touch_devices += entry->touch;
	unreliable_devices += !entry->reliable;
	activity_status.devices++;
	list_add_tail(&entry->node, &activity_handles);
	entry->listed = true;
	entry->ready = true;
	apply_input(SA_RESYNC);
	spin_unlock(&activity_lock);
	spin_unlock_irqrestore(&dev->event_lock, flags);
	return 0;
free:
	kfree(entry);
	return error;
}

static void activity_disconnect(struct input_handle *handle)
{
	struct sab_handle *entry = handle->private;
	unsigned long flags;

	input_close_device(handle);
	input_unregister_handle(handle);
	spin_lock_irqsave(&activity_lock, flags);
	entry->ready = false;
	if (entry->listed) {
		list_del(&entry->node);
		entry->listed = false;
		contact_count -= entry->contacts;
		touch_devices -= entry->touch;
		unreliable_devices -= !entry->reliable;
		activity_status.devices--;
		apply_input(SA_RESYNC);
	}
	spin_unlock_irqrestore(&activity_lock, flags);
	kfree(entry);
}

static const struct input_device_id activity_ids[] = {
	{ .driver_info = 1 }, { },
};
static struct input_handler activity_handler = {
	.events = activity_events, .match = activity_match,
	.connect = activity_connect, .disconnect = activity_disconnect,
	.name = "scene-activity", .id_table = activity_ids,
};

static void read_counters(struct sa_cpu_counter *samples)
{
	unsigned int cpu;

	for_each_possible_cpu(cpu) {
		struct kernel_cpustat stat;
		u64 idle, wait, busy;
		bool online = cpu_online(cpu);

		kcpustat_cpu_fetch(&stat, cpu);
		idle = stat.cpustat[CPUTIME_IDLE];
		wait = stat.cpustat[CPUTIME_IOWAIT];
		if (online) {
			u64 usec = get_cpu_idle_time_us(cpu, NULL);
			u64 iowait = get_cpu_iowait_time_us(cpu, NULL);

			if (usec != -1ULL) idle = usec * NSEC_PER_USEC;
			if (iowait != -1ULL) wait = iowait * NSEC_PER_USEC;
		}
		busy = stat.cpustat[CPUTIME_USER] + stat.cpustat[CPUTIME_NICE] +
			stat.cpustat[CPUTIME_SYSTEM] + stat.cpustat[CPUTIME_IRQ] +
			stat.cpustat[CPUTIME_SOFTIRQ] + stat.cpustat[CPUTIME_STEAL];
		samples[cpu] = (struct sa_cpu_counter){busy + idle + wait, idle + wait, online};
	}
}

static bool load_from_window(u32 *load)
{
	u64 all_total = 0, all_idle = 0;
	u32 peak = 0;
	unsigned int cpu;

	read_counters(current_samples);
	for_each_possible_cpu(cpu) {
		u64 total, idle;

		if (!baseline[cpu].online && !current_samples[cpu].online)
			continue;
		if (!sa_counter_delta(&baseline[cpu], &current_samples[cpu], &total, &idle))
			return false;
		all_total += total;
		all_idle += idle;
		if (cpumask_test_cpu(cpu, &performance_cpus))
			peak = max(peak, sa_busy_permille(total, idle));
	}
	if (!all_total)
		return false;
	*load = max(peak, sa_busy_permille(all_total, all_idle));
	return true;
}

static void activity_irq_fn(struct irq_work *work)
{
	if (READ_ONCE(accepting))
		queue_work(activity_wq, &activity_work);
}

/* The only timer is a captured absolute deadline, rearmed only by real events. */
static void schedule_deadline(const struct sa_output *out, bool retry, bool from_deadline)
{
	u64 now = activity_now(), at = retry ? now + 200 : out->deadline_ms;
	unsigned long flags;

	spin_lock_irqsave(&activity_lock, flags);
	armed_token = out->token;
	armed_epoch = out->epoch;
	spin_unlock_irqrestore(&activity_lock, flags);
	if (retry || out->deadline_valid) {
		unsigned long delay = msecs_to_jiffies(at > now ? at - now : 0);

		/* Self-rearm only queues a fresh instance, never steals pending work.
		 * Sync-cancel's PENDING bit makes queue_delayed_work fail cleanly.
		 * The separate activity_work is synchronously canceled first.
		 */
		if (from_deadline)
			queue_delayed_work(activity_wq, &activity_deadline, delay);
		else
			mod_delayed_work(activity_wq, &activity_deadline, delay);
	} else {
		cancel_delayed_work(&activity_deadline);
	}
}

static void run_activity(struct work_struct *work)
{
	struct sa_output out;
	unsigned long flags;
	u64 seq, epoch;
	bool capture, publish;

	spin_lock_irqsave(&activity_lock, flags);
	if (!accepting) {
		spin_unlock_irqrestore(&activity_lock, flags);
		return;
	}
	seq = activity_sequence;
	epoch = activity_state.context.epoch;
	capture = !activity_state.contacts && baseline_sequence != seq;
	spin_unlock_irqrestore(&activity_lock, flags);
	if (capture)
		read_counters(baseline);
	spin_lock_irqsave(&activity_lock, flags);
	if (!accepting || seq != activity_sequence || epoch != activity_state.context.epoch) {
		spin_unlock_irqrestore(&activity_lock, flags);
		return; /* New input has its own IRQ work queued. */
	}
	if (capture) {
		baseline_sequence = seq;
		baseline_valid = true;
	}
	out = sa_snapshot(&activity_state);
	publish = publish_pending;
	publish_pending = false;
	if (publish) {
		activity_status.transitions++;
		activity_notify(out.epoch, out.inactive);
	}
	update_status();
	spin_unlock_irqrestore(&activity_lock, flags);
	schedule_deadline(&out, false, false);
}

static void run_deadline(struct work_struct *work)
{
	struct sa_event event = { .kind = SA_DEADLINE };
	struct sa_output out;
	unsigned long flags;
	u64 token, epoch, seq;
	u32 load = 0;
	bool known, publish;

	spin_lock_irqsave(&activity_lock, flags);
	token = armed_token;
	epoch = armed_epoch;
	seq = activity_sequence;
	if (!accepting || activity_state.token != token || activity_state.context.epoch != epoch) {
		spin_unlock_irqrestore(&activity_lock, flags);
		return;
	}
	spin_unlock_irqrestore(&activity_lock, flags);
	known = baseline_valid && baseline_sequence == seq && load_from_window(&load);
	if (!known && retry_sequence != seq)
		read_counters(baseline);
	spin_lock_irqsave(&activity_lock, flags);
	if (!accepting || seq != activity_sequence || activity_state.token != token ||
	    activity_state.context.epoch != epoch) {
		spin_unlock_irqrestore(&activity_lock, flags);
		return;
	}
	activity_status.load_samples++;
	if (!known) {
		activity_status.load_errors++;
		/* One resampling deadline per input epoch, never periodic polling. */
		if (retry_sequence == seq) {
			retry_pending = false;
			sa_cancel(&activity_state);
			activity_status.error = -EAGAIN;
			update_status();
			spin_unlock_irqrestore(&activity_lock, flags);
			return;
		}
		retry_sequence = seq;
		baseline_sequence = seq;
		baseline_valid = true;
		retry_pending = true;
		out = sa_snapshot(&activity_state);
		spin_unlock_irqrestore(&activity_lock, flags);
		schedule_deadline(&out, true, true);
		return;
	}
	retry_pending = false;
	activity_status.error = 0;
	activity_status.load_permille = load;
	event.now_ms = activity_now();
	event.token = token;
	event.load_known = true;
	event.load_permille = load;
	/* Retry retains the original FSM deadline; it never creates a new window. */
	sa_apply(&activity_state, &activity_config, &activity_state.context, &event, &out);
	publish = out.publish || publish_pending;
	publish_pending = false;
	if (publish) {
		activity_status.transitions++;
		activity_notify(out.epoch, out.inactive);
	}
	update_status();
	spin_unlock_irqrestore(&activity_lock, flags);
	schedule_deadline(&out, false, true);
}

int scene_activity_backend_init(void (*notify)(u64 epoch, bool inactive))
{
	if (!notify)
		return -EINVAL;
	baseline = kcalloc(nr_cpu_ids, sizeof(*baseline), GFP_KERNEL);
	current_samples = kcalloc(nr_cpu_ids, sizeof(*current_samples), GFP_KERNEL);
	activity_wq = alloc_ordered_workqueue("scene_activity", WQ_FREEZABLE | WQ_POWER_EFFICIENT);
	if (!baseline || !current_samples || !activity_wq) {
		if (activity_wq) destroy_workqueue(activity_wq);
		activity_wq = NULL;
		kfree(baseline); kfree(current_samples);
		baseline = NULL; current_samples = NULL;
		return -ENOMEM;
	}
	activity_notify = notify;
	activity_config = sa_native_defaults();
	INIT_WORK(&activity_work, run_activity);
	INIT_DELAYED_WORK(&activity_deadline, run_deadline);
	init_irq_work(&activity_irq, activity_irq_fn);
	return 0;
}

int scene_activity_backend_configure(const struct sa_context *context, unsigned int cpu)
{
	struct cpufreq_policy *policy;
	struct sa_event event = { .kind = SA_CONTEXT };
	struct sa_output out;
	unsigned long flags;
	bool enable;
	int error = 0;

	if (!activity_wq)
		return -ENODEV;
	mutex_lock(&activity_config_lock);
	enable = context->enabled && context->valid && !context->game && !context->screen_off;
	spin_lock_irqsave(&activity_lock, flags);
	if (activity_state.initialized && sa_context_equal(&activity_state.context, context) &&
	    first_cpu == cpu && handler_registered == enable) {
		spin_unlock_irqrestore(&activity_lock, flags);
		goto out;
	}
	accepting = false;
	spin_unlock_irqrestore(&activity_lock, flags);
	/* No core policy_wq work is flushed here: notifier never takes config_lock. */
	if (handler_registered && !enable) {
		input_unregister_handler(&activity_handler);
		handler_registered = false;
	}
	irq_work_sync(&activity_irq);
	cancel_work_sync(&activity_work);
	cancel_delayed_work_sync(&activity_deadline);
	if (!handler_registered && enable) {
		error = input_register_handler(&activity_handler);
		handler_registered = !error;
	}
	cpumask_copy(&performance_cpus, cpu_possible_mask);
	policy = enable ? cpufreq_cpu_get(cpu) : NULL;
	if (policy) {
		cpumask_andnot(&performance_cpus, &performance_cpus, policy->related_cpus);
		cpufreq_cpu_put(policy);
	} else {
		/* Unknown topology: use aggregate only rather than guess little CPUs. */
		cpumask_clear(&performance_cpus);
	}
	spin_lock_irqsave(&activity_lock, flags);
	first_cpu = cpu;
	activity_sequence++;
	baseline_sequence = 0;
	baseline_valid = false;
	retry_pending = false;
	retry_sequence = 0;
	event.now_ms = activity_now();
	event.contacts = contact_count;
	event.input_reliable = !error && input_reliable();
	/* Configuration epoch changes are required even if visible flags match. */
	memset(&activity_state, 0, sizeof(activity_state));
	sa_apply(&activity_state, &activity_config, context, &event, &out);
	publish_pending = out.publish;
	accepting = enable && !error;
	activity_status.registered = handler_registered;
	activity_status.error = error;
	update_status();
	spin_unlock_irqrestore(&activity_lock, flags);
	if (accepting)
		queue_work(activity_wq, &activity_work);
out:
	mutex_unlock(&activity_config_lock);
	return error;
}

void scene_activity_backend_status(struct sab_status *status)
{
	unsigned long flags;

	spin_lock_irqsave(&activity_lock, flags);
	*status = activity_status;
	spin_unlock_irqrestore(&activity_lock, flags);
}

void scene_activity_backend_exit(void)
{
	struct sa_context off = {0};

	if (!activity_wq)
		return;
	off.epoch = activity_state.context.epoch + 1;
	scene_activity_backend_configure(&off, 0);
	destroy_workqueue(activity_wq);
	activity_wq = NULL;
	kfree(baseline); kfree(current_samples);
	baseline = NULL; current_samples = NULL;
}

#if IS_ENABLED(CONFIG_FRAME_POLICY_KUNIT_TEST)
static DECLARE_COMPLETION(test_worker_entered);
static DECLARE_COMPLETION(test_worker_resume);
static void test_hold_worker(struct work_struct *work)
{
	complete(&test_worker_entered);
	wait_for_completion(&test_worker_resume);
}
static DECLARE_WORK(test_worker_blocker, test_hold_worker);

int scene_activity_backend_test_pause_worker(void)
{
	reinit_completion(&test_worker_entered);
	reinit_completion(&test_worker_resume);
	queue_work(activity_wq, &test_worker_blocker);
	if (wait_for_completion_timeout(&test_worker_entered, msecs_to_jiffies(1000)))
		return 0;
	complete(&test_worker_resume);
	flush_work(&test_worker_blocker);
	return -ETIMEDOUT;
}

void scene_activity_backend_test_resume_worker(void)
{
	complete(&test_worker_resume);
	flush_work(&test_worker_blocker);
}

void scene_activity_backend_test_connect_hook(void (*hook)(struct input_dev *dev))
{
	/* Tests set this only outside registration, on the serialized owner. */
	connect_before_open_hook = hook;
}

void scene_activity_backend_test_flush(void)
{
	irq_work_sync(&activity_irq);
	flush_work(&activity_work);
}

/* Only the test kernel has this deterministic known-load/deadline fixture. */
int scene_activity_backend_test_deadline(u32 load)
{
	struct sa_event event = { .kind = SA_DEADLINE, .load_known = true };
	struct sa_output out;
	unsigned long flags;
	bool publish;

	if (load > 1000) return -EINVAL;
	scene_activity_backend_test_flush();
	cancel_delayed_work_sync(&activity_deadline);
	spin_lock_irqsave(&activity_lock, flags);
	if (!accepting || !activity_state.deadline_valid) {
		spin_unlock_irqrestore(&activity_lock, flags);
		return -EINVAL;
	}
	event.now_ms = activity_now();
	activity_state.deadline_ms = event.now_ms;
	event.token = activity_state.token;
	event.load_permille = load;
	sa_apply(&activity_state, &activity_config, &activity_state.context, &event, &out);
	publish = out.publish || publish_pending;
	publish_pending = false;
	if (publish)
		activity_notify(out.epoch, out.inactive);
	update_status();
	spin_unlock_irqrestore(&activity_lock, flags);
	schedule_deadline(&out, false, false);
	return 0;
}
#endif
