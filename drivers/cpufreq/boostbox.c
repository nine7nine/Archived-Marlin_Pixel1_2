/*
 * Copyright (C) 2018, Jordan Johnston <johnstonljordan@gmail.com>
 *
 * Boostbox is based on "cpu_input_boost" driver, authored by;
 *
 * Copyright (C) 2018, Sultan Alsawaf <sultanxda@gmail.com>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License version 2 and
 * only version 2 as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

#define pr_fmt(fmt) "boostbox: " fmt

#include <linux/cpu.h>
#include <linux/cpufreq.h>
#include <linux/fb.h>
#include <linux/input.h>
#include <linux/slab.h>
#include <linux/sched.h>
#include <linux/moduleparam.h>

/* Available bits for boost_drv state */
#define SCREEN_AWAKE		(1U << 0)
#define WAKE_BOOST		(1U << 1)
#define MAX_BOOST		(1U << 2)
#define GFX_BOOST		(1U << 3)
#define AUDIO_BOOST		(1U << 4)
#define TOP_APP_BOOST		(1U << 5)
#define RT_BOOST		(1U << 6)

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
unsigned int top;
unsigned int rt;
unsigned int gfx;
unsigned int audio;

/* Schedtune "floor" values */
static int dsb_top_app_floor = 0;
module_param(dsb_top_app_floor, uint, 0644);
static int dsb_gfx_floor = 15;
module_param(dsb_gfx_floor, uint, 0644);
static int dsb_rt_floor = 30;
module_param(dsb_rt_floor, uint, 0644);

/* Schedtune "boost" values  */
static int dsb_max_boost = 50;
module_param(dsb_max_boost, uint, 0644);
static int dsb_top_app_boost = 40;
module_param(dsb_top_app_boost, uint, 0644);
static int dsb_rt_boost = 35;
module_param(dsb_rt_boost, uint, 0644);
static int dsb_gfx_boost = 25;
module_param(dsb_gfx_boost, uint, 0644);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

// boost cpu frequencies
static int sx_lp_min_boost = 384000;
module_param(sx_lp_min_boost, uint, 0644);
static int sx_perf_min_boost = 384000;
module_param(sx_perf_min_boost, uint, 0644);
// Boosting Durations
static int sx_gfx_boost_ms = 64;
static int sx_wake_boost_ms = 1000;
module_param(sx_wake_boost_ms, uint, 0644);
static int sx_app_launch_boost_ms = 1500;
module_param(sx_app_launch_boost_ms, uint, 0644);
static int sx_rt_boost_ms = 1800;
module_param(sx_rt_boost_ms, uint, 0644);
static int sx_top_app_boost_ms = 120;
module_param(sx_top_app_boost_ms, uint, 0644);

struct boost_drv {
	struct workqueue_struct *wq;
	struct work_struct max_boost;
	struct delayed_work max_unboost;
	struct work_struct gfx_boost;
	struct delayed_work gfx_unboost;
	struct work_struct top_app_boost;
	struct delayed_work top_app_unboost;
	struct work_struct rt_boost;
	struct delayed_work rt_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block fb_notif;
	unsigned long max_boost_expires;
	unsigned long top_app_boost_expires;
	atomic_t max_boost_dur;
	atomic_t top_app_boost_dur;
	spinlock_t lock;
	u32 state;
};

static struct boost_drv *boost_drv_g;

static u32 get_boost_freq(struct boost_drv *b, u32 cpu)
{
	if (cpumask_test_cpu(cpu, cpu_lp_mask))
		return sx_lp_min_boost;

	return sx_perf_min_boost;
}

static u32 get_boost_state(struct boost_drv *b)
{
	u32 state;

	spin_lock(&b->lock);
	state = b->state;
	spin_unlock(&b->lock);

	return state;
}

static void set_boost_bit(struct boost_drv *b, u32 state)
{
	spin_lock(&b->lock);
	b->state |= state;
	spin_unlock(&b->lock);
}

static void clear_boost_bit(struct boost_drv *b, u32 state)
{
	spin_lock(&b->lock);
	b->state &= ~state;
	spin_unlock(&b->lock);
}

static void update_online_cpu_policy(void)
{
	u32 cpu;

	/* Trigger cpufreq notifier for online CPUs */
	get_online_cpus();
	for_each_online_cpu(cpu)
		cpufreq_update_policy(cpu);
	put_online_cpus();
}

static void unboost_all_cpus(struct boost_drv *b)
{
	if (!cancel_delayed_work_sync(&b->gfx_unboost) &&
		!cancel_delayed_work_sync(&b->max_unboost))
		return;
 
#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Reset dynamic stune boost value to the default value */
	do_stune_unboost("top-app", dsb_top_app_floor);
	do_stune_unboost("gfx", dsb_gfx_floor);
	do_stune_unboost("rt", dsb_rt_floor);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, WAKE_BOOST | MAX_BOOST | GFX_BOOST | TOP_APP_BOOST | RT_BOOST);
	update_online_cpu_policy();
}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
void boostbox_rt_kick(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	queue_work(b->wq, &b->rt_boost);
}

void boostbox_top_app_kick(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	queue_work(b->wq, &b->top_app_boost);
}
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

void boostbox_gfx_kick(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	queue_work(b->wq, &b->gfx_boost);
}

static void __boostbox_kick_max(struct boost_drv *b,
	unsigned int duration_ms)
{
	unsigned long new_expires;

	/* Skip this boost if there's already a longer boost in effect */
	spin_lock(&b->lock);
	new_expires = jiffies + msecs_to_jiffies(duration_ms);
	if (time_after(b->max_boost_expires, new_expires)) {
		spin_unlock(&b->lock);
		return;
	}
	b->max_boost_expires = new_expires;
	spin_unlock(&b->lock);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Set dynamic stune boost value */
	do_stune_boost("top-app", dsb_max_boost);
	do_stune_boost("rt", dsb_rt_boost);
	do_stune_boost("gfx", dsb_gfx_boost);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	atomic_set(&b->max_boost_dur, duration_ms);
	queue_work(b->wq, &b->max_boost);
}

void boostbox_kick_max(unsigned int duration_ms)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	__boostbox_kick_max(b, duration_ms);
}

static void max_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), max_boost);

	if (!cancel_delayed_work_sync(&b->max_unboost)) {
		set_boost_bit(b, MAX_BOOST);
		update_online_cpu_policy();
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Set dynamic stune boost value */
	do_stune_boost("top-app", dsb_max_boost);
	do_stune_boost("rt", dsb_rt_boost);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	queue_delayed_work(b->wq, &b->max_unboost,
		msecs_to_jiffies(atomic_read(&b->max_boost_dur)));
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), max_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Set dynamic stune boost value */
	do_stune_unboost("top-app", dsb_top_app_floor);
	do_stune_unboost("rt", dsb_rt_floor);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, WAKE_BOOST | MAX_BOOST | TOP_APP_BOOST | RT_BOOST);
	update_online_cpu_policy();
}

static void gfx_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), gfx_boost);

	if (!cancel_delayed_work_sync(&b->gfx_unboost)) {
		set_boost_bit(b, GFX_BOOST);
		update_online_cpu_policy();
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Set dynamic stune boost value */
	do_stune_boost("gfx", dsb_gfx_boost);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	queue_delayed_work(b->wq, &b->gfx_unboost,
		msecs_to_jiffies(sx_gfx_boost_ms));
}

static void gfx_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), gfx_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Reset dynamic stune boost value to the default value */
	do_stune_unboost("gfx", dsb_gfx_floor);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, GFX_BOOST);
	update_online_cpu_policy();
}

static void top_app_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), top_app_boost);

	if (!cancel_delayed_work_sync(&b->top_app_unboost)) {
		set_boost_bit(b, TOP_APP_BOOST);
		update_online_cpu_policy();
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Reset dynamic stune boost value to the default value */
	do_stune_boost("top-app", dsb_top_app_boost);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	queue_delayed_work(b->wq, &b->top_app_unboost,
		msecs_to_jiffies(sx_top_app_boost_ms));
}

static void top_app_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), top_app_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	do_stune_unboost("top-app", dsb_top_app_floor);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, TOP_APP_BOOST);
	update_online_cpu_policy();
}

static void rt_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), rt_boost);

	if (!cancel_delayed_work_sync(&b->rt_unboost)) {
		set_boost_bit(b, RT_BOOST);
		update_online_cpu_policy();
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Reset dynamic stune boost value to the default value */
	do_stune_boost("rt", dsb_rt_boost);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	queue_delayed_work(b->wq, &b->rt_unboost,
		msecs_to_jiffies(sx_rt_boost_ms));
}

static void rt_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), rt_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	do_stune_unboost("rt", dsb_rt_floor);
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, RT_BOOST);
	update_online_cpu_policy();
}

static int cpu_notifier_cb(struct notifier_block *nb,
	unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), cpu_notif);
	struct cpufreq_policy *policy = data;
	u32 boost_freq, state;

	if (action != CPUFREQ_ADJUST)
		return NOTIFY_OK;

	state = get_boost_state(b);

	/* Boost CPU to max frequency for max boost */
	if (state & MAX_BOOST) {
		policy->min = policy->max;
		return NOTIFY_OK;
	}

	/*
	 * Boost to policy->max if the boost frequency is higher. When
	 * unboosting, set policy->min to the absolute min freq for the CPU.
	 */
	if (state & GFX_BOOST) {
		boost_freq = get_boost_freq(b, policy->cpu);
		policy->min = min(policy->max, boost_freq);
	} else {
		policy->min = policy->cpuinfo.min_freq;
	}

	return NOTIFY_OK;
}

static int fb_notifier_cb(struct notifier_block *nb,
	unsigned long action, void *data)
{
	struct boost_drv *b = container_of(nb, typeof(*b), fb_notif);
	struct fb_event *evdata = data;
	int *blank = evdata->data;

	/* Parse framebuffer blank events as soon as they occur */
	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	/* Boost when the screen turns on and unboost when it turns off */
	if (*blank == FB_BLANK_UNBLANK) {
		set_boost_bit(b, SCREEN_AWAKE);
		__boostbox_kick_max(b, sx_wake_boost_ms);
	} else {
		clear_boost_bit(b, SCREEN_AWAKE);
		unboost_all_cpus(b);
	}

	return NOTIFY_OK;
}

static void boostbox_input_event(struct input_handle *handle,
	unsigned int type, unsigned int code, int value)
{
	struct boost_drv *b = handle->handler->private;
	u32 state;

	state = get_boost_state(b);

	if (!(state & SCREEN_AWAKE))
		return;

	queue_work(b->wq, &b->gfx_boost);
}

static int boostbox_input_connect(struct input_handler *handler,
	struct input_dev *dev, const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "boostbox_handle";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void boostbox_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id boostbox_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) },
	},
	/* Touchpad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_KEYBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.keybit = { [BIT_WORD(BTN_TOUCH)] = BIT_MASK(BTN_TOUCH) },
		.absbit = { [BIT_WORD(ABS_X)] =
			BIT_MASK(ABS_X) | BIT_MASK(ABS_Y) },
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) },
	},
	{ }
};

static struct input_handler boostbox_input_handler = {
	.event		= boostbox_input_event,
	.connect	= boostbox_input_connect,
	.disconnect	= boostbox_input_disconnect,
	.name		= "boostbox_handler",
	.id_table	= boostbox_ids
};

static struct boost_drv *alloc_boost_drv(void)
{
	struct boost_drv *b;

	b = kzalloc(sizeof(*b), GFP_KERNEL);
	if (!b)
		return NULL;

	b->wq = alloc_workqueue("boostbox_wq", WQ_HIGHPRI, 0);
	if (!b->wq) {
		pr_err("Failed to allocate workqueue\n");
		goto free_b;
	}

	return b;

free_b:
	kfree(b);
	return NULL;
}

static int __init boostbox_init(void)
{
	struct boost_drv *b;
	int ret;

	b = alloc_boost_drv();
	if (!b) {
		pr_err("Failed to allocate boost_drv struct\n");
		return -ENOMEM;
	}

	spin_lock_init(&b->lock);
	INIT_WORK(&b->max_boost, max_boost_worker);
	INIT_DELAYED_WORK(&b->max_unboost, max_unboost_worker);
	INIT_WORK(&b->gfx_boost, gfx_boost_worker);
	INIT_DELAYED_WORK(&b->gfx_unboost, gfx_unboost_worker);
	INIT_WORK(&b->top_app_boost, top_app_boost_worker);
	INIT_DELAYED_WORK(&b->top_app_unboost, top_app_unboost_worker);
	INIT_WORK(&b->rt_boost, rt_boost_worker);
	INIT_DELAYED_WORK(&b->rt_unboost, rt_unboost_worker);
	b->state = SCREEN_AWAKE;

	b->cpu_notif.notifier_call = cpu_notifier_cb;
	ret = cpufreq_register_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
	if (ret) {
		pr_err("Failed to register cpufreq notifier, err: %d\n", ret);
		goto free_b;
	}

	boostbox_input_handler.private = b;
	ret = input_register_handler(&boostbox_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		goto unregister_cpu_notif;
	}

	b->fb_notif.notifier_call = fb_notifier_cb;
	b->fb_notif.priority = INT_MAX;
	ret = fb_register_client(&b->fb_notif);
	if (ret) {
		pr_err("Failed to register fb notifier, err: %d\n", ret);
		goto unregister_handler;
	}

	/* Allow global boost config access for external boosts */
	boost_drv_g = b;

	return 0;

unregister_handler:
	input_unregister_handler(&boostbox_input_handler);
unregister_cpu_notif:
	cpufreq_unregister_notifier(&b->cpu_notif, CPUFREQ_POLICY_NOTIFIER);
free_b:
	kfree(b);
	return ret;
}
late_initcall(boostbox_init);
