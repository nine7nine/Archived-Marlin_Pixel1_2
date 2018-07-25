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
#define IDLE_BOOST		(1U << 5)

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
unsigned int top;
unsigned int fg;
unsigned int rt;
unsigned int gfx;
unsigned int audio;
static int boost_slot;
static bool stune_boost_active;

static int dsb_idle_boost = 0;
module_param(dsb_idle_boost, uint, 0644);
static int dsb_idle_boost_ms = 16000;
module_param(dsb_idle_boost_ms, uint, 0644);

/* Schedtune "floor" values */
static int dsb_top_app_floor = 0;
module_param(dsb_top_app_floor, uint, 0644);
static int dsb_gfx_floor = 0;
module_param(dsb_gfx_floor, uint, 0644);
static int dsb_rt_floor = 0;
module_param(dsb_rt_floor, uint, 0644);
static int dsb_fg_floor = 0;
module_param(dsb_fg_floor, uint, 0644);
static int dsb_audio_floor = 0;
module_param(dsb_audio_floor, uint, 0644);

/* Schedtune "boost" values  */
static int dsb_kick_max_boost = 30;
module_param(dsb_kick_max_boost, uint, 0644);
static int dsb_top_app_boost = 24;
module_param(dsb_top_app_boost, uint, 0644);
static int dsb_gfx_kick_boost = 20;
module_param(dsb_gfx_kick_boost, uint, 0644);
static int dsb_rt_boost = 20;
module_param(dsb_rt_boost, uint, 0644);
static int dsb_fg_boost = 6;
module_param(dsb_fg_boost, uint, 0644);
static int dsb_audio_boost = 6;
module_param(dsb_audio_boost, uint, 0644);
static int dsb_audio_boost_ms = 64;
module_param(dsb_audio_boost_ms, uint, 0644);
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
static int sx_app_launch_boost_ms = 1000;
module_param(sx_app_launch_boost_ms, uint, 0644);

struct boost_drv {
	struct workqueue_struct *wq;
	struct work_struct max_boost;
	struct delayed_work max_unboost;
	struct work_struct gfx_boost;
	struct delayed_work gfx_unboost;
	struct work_struct audio_boost;
	struct delayed_work audio_unboost;
	struct work_struct idle_boost;
	struct delayed_work idle_unboost;
	struct notifier_block cpu_notif;
	struct notifier_block fb_notif;
	unsigned long max_boost_expires;
	atomic_t max_boost_dur;
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
	/* Do the unboost explicitly */
	top = do_stune_unboost("top-app", dsb_top_app_floor, &boost_slot);
	if (!top)
		stune_boost_active = false;
	fg = do_stune_unboost("foreground", dsb_fg_floor, &boost_slot);
	if (!fg)
		stune_boost_active = false;
	gfx = do_stune_unboost("gfx", dsb_gfx_floor, &boost_slot);
	if (!gfx)
		stune_boost_active = false;
	rt = do_stune_unboost("rt", dsb_rt_floor, &boost_slot);
	if (!rt)
		stune_boost_active = false;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, WAKE_BOOST | MAX_BOOST | GFX_BOOST);
	update_online_cpu_policy();
}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
void dsb_audio_boost_kick(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	queue_work(b->wq, &b->audio_boost);
}

void boostbox_idle_boost(void)
{
	struct boost_drv *b = boost_drv_g;

	if (!b)
		return;

	queue_work(b->wq, &b->idle_boost);
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
	/* Check if boosted, if so; unboost && mark slots inactive */
	if (stune_boost_active) {
		top = do_stune_unboost("top-app", dsb_top_app_floor, &boost_slot);
		if (!top)
			stune_boost_active = false;
		fg = do_stune_unboost("foreground", dsb_fg_floor, &boost_slot);
		if (!fg)
			stune_boost_active = false;
		rt = do_stune_unboost("rt", dsb_rt_floor, &boost_slot);
		if (!rt)
			stune_boost_active = false;
	}

	/* Set dynamic stune boost values */
	top = do_stune_boost("top-app", dsb_kick_max_boost, &boost_slot);
	if (!top)
		stune_boost_active = true;
	fg = do_stune_boost("foreground", dsb_fg_boost, &boost_slot);
	if (!fg)
		stune_boost_active = true;
	rt = do_stune_boost("rt", dsb_rt_boost, &boost_slot);
	if (!rt)
		stune_boost_active = true;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	queue_delayed_work(b->wq, &b->max_unboost,
		msecs_to_jiffies(atomic_read(&b->max_boost_dur)));
}

static void max_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), max_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Reset dynamic stune boost value to the default value */
	top = do_stune_unboost("top-app", dsb_top_app_floor, &boost_slot);
	if (!top)
		stune_boost_active = false;
	fg = do_stune_unboost("foreground", dsb_fg_floor, &boost_slot);
	if (!fg)
		stune_boost_active = false;
	rt = do_stune_unboost("rt", dsb_rt_floor, &boost_slot);
	if (!rt)
		stune_boost_active = false;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, WAKE_BOOST | MAX_BOOST);
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
	/* Check if boosted, if so; unboost && mark slots inactive */
	if (stune_boost_active) {
		gfx = do_stune_unboost("gfx", dsb_gfx_floor, &boost_slot);
		if (!gfx)
			stune_boost_active = false;
		rt = do_stune_unboost("rt", dsb_rt_floor, &boost_slot);
		if (!rt)
			stune_boost_active = false;
	}
	/* Set dynamic stune boost values, mark slots active */
	gfx = do_stune_boost("gfx", dsb_gfx_kick_boost, &boost_slot);
	if (!gfx)
		stune_boost_active = true;
	rt = do_stune_boost("rt", dsb_rt_boost, &boost_slot);
	if (!rt)
		stune_boost_active = true;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	queue_delayed_work(b->wq, &b->gfx_unboost,
		msecs_to_jiffies(sx_gfx_boost_ms));
}

static void gfx_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), gfx_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Check if boosted, if so; unboost && mark slots inactive */
	if (stune_boost_active) {
		/* Reset dynamic stune boost value to the default value */
		gfx = do_stune_unboost("gfx", dsb_gfx_floor, &boost_slot);
		if (!gfx)
			stune_boost_active = false;
		rt = do_stune_unboost("rt", dsb_rt_floor, &boost_slot);
		if (!rt)
			stune_boost_active = false;
	}
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, GFX_BOOST);
	update_online_cpu_policy();
}

static void audio_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), audio_boost);

	if (!cancel_delayed_work_sync(&b->audio_unboost)) {
		set_boost_bit(b, AUDIO_BOOST);
		update_online_cpu_policy();
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Check if boosted, if so; unboost && mark slots inactive */
	if (stune_boost_active) {
		audio = do_stune_unboost("audio", dsb_audio_floor, &boost_slot);
		if (!audio)
			stune_boost_active = false;
	}
	/* Set dynamic stune boost value */
	audio = do_stune_boost("audio", dsb_audio_boost, &boost_slot);
	if (!audio)
		stune_boost_active = true;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	queue_delayed_work(b->wq, &b->audio_unboost,
		msecs_to_jiffies(dsb_audio_boost_ms));
}

static void audio_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), audio_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	audio = do_stune_unboost("audio", dsb_audio_floor, &boost_slot);
	if (!audio)
		stune_boost_active = false;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, AUDIO_BOOST);
	update_online_cpu_policy();
}

static void idle_boost_worker(struct work_struct *work)
{
	struct boost_drv *b = container_of(work, typeof(*b), idle_boost);

	if (!cancel_delayed_work_sync(&b->idle_unboost)) {
		set_boost_bit(b, IDLE_BOOST);
		update_online_cpu_policy();
	}

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	/* Check if boosted, if so; unboost && mark slots inactive */
	if (stune_boost_active) {
		top = do_stune_unboost("top-app", dsb_idle_boost, &boost_slot);
		if (!top)
			stune_boost_active = false;
	}
	/* Reset dynamic stune boost value to the default value */
	top = do_stune_boost("top-app", dsb_top_app_boost, &boost_slot);
	if (!top)
		stune_boost_active = true;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	queue_delayed_work(b->wq, &b->idle_unboost,
		msecs_to_jiffies(dsb_idle_boost_ms));
}

static void idle_unboost_worker(struct work_struct *work)
{
	struct boost_drv *b =
		container_of(to_delayed_work(work), typeof(*b), idle_unboost);

#ifdef CONFIG_DYNAMIC_STUNE_BOOST
	top = do_stune_unboost("top-app", dsb_idle_boost, &boost_slot);
	if (!top)
		stune_boost_active = false;
#endif /* CONFIG_DYNAMIC_STUNE_BOOST */

	clear_boost_bit(b, IDLE_BOOST);
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
	INIT_WORK(&b->audio_boost, audio_boost_worker);
	INIT_DELAYED_WORK(&b->audio_unboost, audio_unboost_worker);
	INIT_WORK(&b->idle_boost, idle_boost_worker);
	INIT_DELAYED_WORK(&b->idle_unboost, idle_unboost_worker);
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
