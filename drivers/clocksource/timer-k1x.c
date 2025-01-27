// SPDX-License-Identifier: GPL-2.0
/*
 * spacemit-k1x timer driver
 *
 * Copyright (C) 2023 Spacemit
 */

#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/interrupt.h>
#include <linux/clockchips.h>

#include <linux/io.h>
#include <linux/irq.h>
#include <linux/of.h>
#include <linux/of_address.h>
#include <linux/of_irq.h>
#include <linux/delay.h>
#include <linux/slab.h>
#include <linux/cpu.h>
#include <linux/clockchips.h>
#include <linux/sched_clock.h>
#include <linux/stat.h>
#include <linux/clk.h>
#include <linux/reset.h>

#define TMR_CCR		(0x000c)
#define TMR_TN_MM(n, m)	(0x0010 + ((n) << 4) + ((m) << 2))
#define TMR_CR(n)	(0x0090 + ((n) << 2))
#define TMR_SR(n)	(0x0080 + ((n) << 2))
#define TMR_IER(n)	(0x0060 + ((n) << 2))
#define TMR_PLVR(n)	(0x0040 + ((n) << 2))
#define TMR_PLCR(n)	(0x0050 + ((n) << 2))
#define TMR_WMER	(0x0068)
#define TMR_WMR		(0x006c)
#define TMR_WVR		(0x00cc)
#define TMR_WSR		(0x00c0)
#define TMR_ICR(n)	(0x0070 + ((n) << 2))
#define TMR_WICR	(0x00c4)
#define TMR_CER		(0x0000)
#define TMR_CMR		(0x0004)
#define TMR_WCR		(0x00c8)
#define TMR_WFAR	(0x00b0)
#define TMR_WSAR	(0x00b4)
#define TMR_CRSR        (0x0008)

#define TMR_CCR_CS_0(x)	(((x) & 0x3) << 0)
#define TMR_CCR_CS_1(x)	(((x) & 0x3) << 2)
#define TMR_CCR_CS_2(x)	(((x) & 0x3) << 5)

#define MAX_EVT_NUM		5

#define MAX_DELTA		(0xfffffffe)
#define MIN_DELTA		(5)

#define SPACEMIT_MAX_COUNTER		3
#define SPACEMIT_MAX_TIMER		3

#define TMR_CER_COUNTER(cid)	(1 << (cid))
#define SPACEMIT_ALL_COUNTERS	((1 << SPACEMIT_MAX_COUNTER) - 1)

#define	SPACEMIT_TIMER_CLOCK_32KHZ	32768

#define SPACEMIT_TIMER_COUNTER_CLKSRC	(1 << 0)
#define SPACEMIT_TIMER_COUNTER_CLKEVT	(1 << 1)
#define SPACEMIT_TIMER_COUNTER_DELAY		(1 << 2)

#define SPACEMIT_TIMER_ALL_CPU	(0xFFFFFFFF)

struct spacemit_timer;

struct spacemit_timer_evt {
	struct clock_event_device ced;
	struct irqaction irqa;
	unsigned int freq;
	unsigned int irq;
	unsigned int cid;
	unsigned int tid;
	int cpu;
	bool timer_enabled;
	/* 0: timer set; 1: timer timeout(irq comes) */
	int timer_status;
	unsigned int timeout;
	struct spacemit_timer *timer;
};

struct spacemit_timer {
	unsigned int id;
	void __iomem *base;
	struct spacemit_timer_evt evt[SPACEMIT_MAX_COUNTER];
	unsigned int flag;
	int loop_delay_fastclk;
	unsigned int fc_freq;
	unsigned int freq;
	struct clk *clk;
	/* lock to protect hw operation. */
	spinlock_t tm_lock;
};

struct timer_werror_info {
	u32 reg;
	u32 target;
	u32 val;
	u32 mask;
};

/* record the last x write failures */
#define TIMER_ERR_NUM			10
static struct timer_werror_info werr_info[TIMER_ERR_NUM];
static int werr_index;

static struct spacemit_timer *spacemit_timers[SPACEMIT_MAX_TIMER];
static int timer_counter_switch_clock(struct spacemit_timer *tm, unsigned int freq);

void timer_dump_hwinfo(int tid)
{
	struct spacemit_timer_evt *t_evt = &spacemit_timers[tid]->evt[0];
	void __iomem *base = spacemit_timers[tid]->base;
	unsigned int sr, cid, cer, cmr, ccr, mr, ier, cr;

	cid = t_evt->cid;

	cer = __raw_readl(base + TMR_CER);
	cmr = __raw_readl(base + TMR_CMR);
	ccr = __raw_readl(base + TMR_CCR);
	mr = __raw_readl(base + TMR_TN_MM(cid, 0));
	ier = __raw_readl(base + TMR_IER(cid));
	sr = __raw_readl(base + TMR_SR(cid));
	cr = __raw_readl(base + TMR_CR(cid));

	pr_err("timer enable: %d. timeout: %d cycles. next event: %lld\n", !t_evt->timer_status, t_evt->timeout, t_evt->ced.next_event);

	pr_err("cer/cmr/ccr/mr/ier/sr/cr: (0x%x 0x%x 0x%x 0x%x 0x%x 0x%x 0x%x)\n", cer, cmr, ccr, mr, ier, sr, cr);

	return;
}

static void timer_write_error(u32 reg, u32 target, u32 val, u32 mask)
{
	werr_info[werr_index].reg = reg;
	werr_info[werr_index].target = target;
	werr_info[werr_index].val = val;
	werr_info[werr_index].mask = mask;
	werr_index = (werr_index+1) % TIMER_ERR_NUM;

	pr_err("timer write fail: register = 0x%x: (0x%x, 0x%x, 0x%x)\n", reg, target, val, mask);
}

static void timer_write_check(struct spacemit_timer *tm, u32 reg, u32 val, u32 mask, bool clr, bool clk_switch)
{
	int loop = 3, retry = 100;
	u32 t_read, t_check = clr ? !val : val;

reg_re_write:
	__raw_writel(val, tm->base + reg);

	if (clk_switch)
		timer_counter_switch_clock(tm, tm->fc_freq);

	t_read = __raw_readl(tm->base + reg);

	while (((t_read & mask) != (t_check & mask)) && loop) {
		/* avoid trying frequently to worsen bus contention */
		udelay(30);
		t_read = __raw_readl(tm->base + reg);
		loop--;

		if (!loop) {
			timer_write_error(reg, t_check, t_read, mask);
			loop = 3;
			if (--retry)
				goto reg_re_write;
			else
				return;
		}
	}
}

static int timer_counter_switch_clock(struct spacemit_timer *tm, unsigned int freq)
{
	u32 ccr, val, mask, tid;

	tid = tm->id;

	ccr = __raw_readl(tm->base + TMR_CCR);

	switch (tid) {
	case 0:
		mask = TMR_CCR_CS_0(3);
		break;
	case 1:
		mask = TMR_CCR_CS_1(3);
		break;
	case 2:
		mask = TMR_CCR_CS_2(3);
		break;
	default:
		pr_err("wrong timer id: 0x%x\n", tid);
		return -EINVAL;
	}

	ccr &= ~mask;

	if (freq == tm->fc_freq)
		val = 0;
	else if (freq == SPACEMIT_TIMER_CLOCK_32KHZ)
		val = 1;
	else {
		pr_err("Timer %d: invalid clock rate %d\n", tid, freq);
		return -EINVAL;
	}

	switch (tid) {
	case 0:
		ccr |= TMR_CCR_CS_0(val);
		break;
	case 1:
		ccr |= TMR_CCR_CS_1(val);
		break;
	case 2:
		ccr |= TMR_CCR_CS_2(val);
		break;
	}

	timer_write_check(tm, TMR_CCR, ccr, mask, false, false);

	return 0;
}

static void timer_counter_disable(struct spacemit_timer_evt *evt)
{
	struct spacemit_timer *tm = evt->timer;
	u32 cer;
	bool clk_switch = false;

	if (evt->freq != tm->fc_freq)
		clk_switch = true;
	/*
	 * Stop the counter will need multiple timer clock to take effect.
	 * Some operations can only be done when counter is disabled. So
	 * add delay here.
	 */
	/* Step1: disable counter */
	cer = __raw_readl(tm->base + TMR_CER);
	timer_write_check(tm, TMR_CER, (cer & ~(1 << evt->cid)), (1 << evt->cid), false, clk_switch);

	/* remove unnecesary write, check explicitly: 2 cycles (32k) */

	evt->timer_status = 1;
}

static void timer_counter_enable(struct spacemit_timer_evt *evt)
{
	struct spacemit_timer *tm = evt->timer;
	u32 cer;

	/* Switch to original clock */
	if (evt->freq != tm->fc_freq)
		timer_counter_switch_clock(tm, evt->freq);

	/* Enable timer */
	cer = __raw_readl(tm->base + TMR_CER);

	timer_write_check(tm, TMR_CER, (cer | (1 << evt->cid)), (1 << evt->cid), false, false);

	evt->timer_status = 0;
}

static irqreturn_t timer_interrupt(int irq, void *dev_id)
{
	struct clock_event_device *c = dev_id;
	struct spacemit_timer_evt *evt;
	unsigned int cnt;
	unsigned long flags;
	void __iomem *base;

	evt = container_of(c, struct spacemit_timer_evt, ced);
	cnt = evt->cid;
	base = evt->timer->base;

	spin_lock_irqsave(&(evt->timer->tm_lock), flags);
	/* We only use match #0 for the counter. */
	if (__raw_readl(base + TMR_SR(cnt)) & 0x1) {
		timer_counter_disable(evt);

		/* Disable the interrupt. */
		timer_write_check(evt->timer, TMR_IER(cnt), 0, 0x7, false, false);
		/* Clear interrupt status */
		timer_write_check(evt->timer, TMR_ICR(cnt), 0x1, 0x7, true, false);

		spin_unlock_irqrestore(&(evt->timer->tm_lock), flags);

		c->event_handler(c);

		return IRQ_HANDLED;
	}

	spin_unlock_irqrestore(&(evt->timer->tm_lock), flags);
	return IRQ_NONE;
}

static int timer_shutdown(struct clock_event_device *dev)
{
	struct spacemit_timer_evt *evt;
	unsigned long flags;

	evt = container_of(dev, struct spacemit_timer_evt, ced);

	spin_lock_irqsave(&(evt->timer->tm_lock), flags);

	evt->timer_enabled = !evt->timer_status;

	/* disable counter */
	timer_counter_disable(evt);

	spin_unlock_irqrestore(&(evt->timer->tm_lock), flags);

	return 0;
}

static int timer_resume(struct clock_event_device *dev)
{
	struct spacemit_timer_evt *evt;
	unsigned long flags;

	evt = container_of(dev, struct spacemit_timer_evt, ced);

	spin_lock_irqsave(&(evt->timer->tm_lock), flags);

	/* check whether need to enable timer */
	if (evt->timer_enabled)
		timer_counter_enable(evt);

	spin_unlock_irqrestore(&(evt->timer->tm_lock), flags);

	return 0;
}

static int timer_set_next_event(unsigned long delta,
				struct clock_event_device *dev)
{
	struct spacemit_timer_evt *evt;
	unsigned int cid;
	unsigned long flags;
	u32 cer;
	void __iomem *base;

	evt = container_of(dev, struct spacemit_timer_evt, ced);
	cid = evt->cid;
	base = evt->timer->base;

	spin_lock_irqsave(&(evt->timer->tm_lock), flags);

	cer = __raw_readl(base + TMR_CER);

	/* If the timer counter is enabled, first disable it. */
	if (cer & (1 << cid))
		timer_counter_disable(evt);

	/* Setup new counter value */
	timer_write_check(evt->timer, TMR_TN_MM(cid, 0), (delta - 1), (u32)(-1), false, false);

	/* enable the matching interrupt */
	timer_write_check(evt->timer, TMR_IER(cid), 0x1, 0x1, false, false);

	timer_counter_enable(evt);

	evt->timeout = delta - 1;

	spin_unlock_irqrestore(&(evt->timer->tm_lock), flags);
	return 0;
}

int __init spacemit_timer_init(struct device_node *np, int tid, void __iomem *base,
			  unsigned int flag, unsigned int fc_freq,
			  unsigned int apb_freq, unsigned int freq)
{
	struct spacemit_timer *tm = spacemit_timers[tid];
	struct clk *clk;
	struct reset_control *resets;
	u32 tmp, delay;

	if (tm)
		return -EINVAL;

	tm = kzalloc(sizeof(*tm), GFP_KERNEL);
	if (!tm)
		return -ENOMEM;

	clk = of_clk_get(np, 0);
	if (!clk) {
		pr_err("%s: get clk failed! %s\n", __func__, np->name);
		goto out;
	}

	if (IS_ERR(clk)) {
		pr_err("Timer %d: fail to get clock!\n", tid);
		goto out;
	}

	if (clk_prepare_enable(clk)) {
		pr_err("Timer %d: fail to enable clock!\n", tid);
		goto out;
	}

	if (clk_set_rate(clk, fc_freq)) {
		pr_err("Timer %d: fail to set clock rate to %uHz!\n", tid, fc_freq);
		goto out;
	}

	resets = of_reset_control_get(np, 0);
	if(IS_ERR(resets)) {
		clk_disable_unprepare(clk);
		return PTR_ERR(resets);
	}
	reset_control_deassert(resets);
	/*
	 * The calculation formula for the loop cycle is:
	 *
	 * (1) need wait for 2 timer's clock cycle:
	 *        1             2
	 *     ------- x 2 = -------
	 *     fc_freq       fc_freq
	 *
	 * (2) convert to apb clock cycle:
	 *        2          1        apb_freq * 2
	 *     ------- / -------- = ----------------
	 *     fc_freq   apb_freq       fc_freq
	 *
	 * (3) every apb register's accessing will take 8 apb clock cycle,
	 *     also consider add extral one more time for safe way;
	 *     so finally need loop times for the apb register accessing:
	 *
	 *       (apb_freq * 2)
	 *     ------------------ / 8 + 1
	 *          fc_freq
	 */
	delay = ((apb_freq * 2) / fc_freq / 8) + 1;
	pr_err("Timer %d: loop_delay_fastclk is %d\n", tid, delay);

	tm->id = tid;
	tm->base = base;
	tm->flag = flag;
	tm->loop_delay_fastclk = delay;
	tm->fc_freq = fc_freq;
	tm->freq = freq;
	spin_lock_init(&(tm->tm_lock));

	spacemit_timers[tid] = tm;

	/* We will disable all counters. Switch to fastclk first. */
	timer_counter_switch_clock(tm, fc_freq);

	/* disalbe all counters */
	tmp = __raw_readl(base + TMR_CER) & ~SPACEMIT_ALL_COUNTERS;
	__raw_writel(tmp, base + TMR_CER);

	/* disable matching interrupt */
	__raw_writel(0x00, base + TMR_IER(0));
	__raw_writel(0x00, base + TMR_IER(1));
	__raw_writel(0x00, base + TMR_IER(2));

	while (delay--) {
		/* Clear pending interrupt status */
		__raw_writel(0x1, base + TMR_ICR(0));
		__raw_writel(0x1, base + TMR_ICR(1));
		__raw_writel(0x1, base + TMR_ICR(2));
		__raw_writel(tmp, base + TMR_CER);
	}

	return 0;
out:
	kfree(tm);
	return -EINVAL;
}

static int __init spacemit_timer_hw_init(struct spacemit_timer_evt *evt)
{
	struct spacemit_timer *tm = evt->timer;
	unsigned int tmp, delay, freq, cid, ratio;
	int ret;

	cid = evt->cid;
	freq = evt->freq;

	ret = timer_counter_switch_clock(tm, freq);
	if (ret)
		return ret;

	ratio = tm->fc_freq / freq;
	delay = tm->loop_delay_fastclk * ratio;

	/* set timer to free-running mode */
	tmp = __raw_readl(tm->base + TMR_CMR) | TMR_CER_COUNTER(cid);
	__raw_writel(tmp, tm->base + TMR_CMR);

	/* free-running */
	__raw_writel(0x0, tm->base + TMR_PLCR(cid));
	/* clear status */
	__raw_writel(0x7, tm->base + TMR_ICR(cid));

	/* enable counter */
	tmp = __raw_readl(tm->base + TMR_CER) | TMR_CER_COUNTER(cid);
	__raw_writel(tmp, tm->base + TMR_CER);

	while (delay--)
		__raw_writel(tmp, tm->base + TMR_CER);

	return 0;
}


int __init spacemit_timer_setup(struct spacemit_timer_evt *evt)
{
	int broadcast = 0;
	int ret;

	if (evt->cpu == SPACEMIT_TIMER_ALL_CPU)
		broadcast = 1;
	else if (evt->cpu >= num_possible_cpus())
		return -EINVAL;

	evt->ced.name = "timer-spacemit";
	evt->ced.features = CLOCK_EVT_FEAT_ONESHOT;
	evt->ced.rating = 200;
	evt->ced.set_next_event = timer_set_next_event;
	evt->ced.set_state_shutdown = timer_shutdown;
	evt->ced.tick_resume = timer_resume;
	evt->ced.irq = evt->irq;

	evt->irqa.flags = IRQF_TIMER | IRQF_IRQPOLL;
	evt->irqa.handler = timer_interrupt;
	evt->irqa.dev_id = &(evt->ced);

	ret = spacemit_timer_hw_init(evt);
	if (ret)
		return ret;

	if (broadcast) {
		evt->irqa.name = "broadcast-timer";
		/* evt->ced.features |= CLOCK_EVT_FEAT_DYNIRQ; */
		evt->ced.cpumask = cpu_possible_mask;
		ret = request_irq(evt->ced.irq, timer_interrupt, IRQF_TIMER | IRQF_IRQPOLL | IRQF_ONESHOT, "broadcast-timer", evt->irqa.dev_id);
		if (ret < 0)
			return ret;
		clockevents_config_and_register(&evt->ced,
						evt->freq, MIN_DELTA, MAX_DELTA);
	} else {
		evt->irqa.name = "local-timer";
		evt->ced.cpumask = cpumask_of(evt->cpu);
		evt->irqa.flags |= IRQF_PERCPU;
		ret = request_irq(evt->ced.irq, timer_interrupt, IRQF_TIMER | IRQF_IRQPOLL, "local-timer", evt->irqa.dev_id);
		if (ret < 0)
			return ret;
		/* Enable clock event device for boot CPU. */
		if (evt->cpu == smp_processor_id()) {
			clockevents_config_and_register(&evt->ced,
							evt->freq, MIN_DELTA,
							MAX_DELTA);
			/* Only online CPU can be set affinity. */
			irq_set_affinity_hint(evt->ced.irq, cpumask_of(evt->cpu));
		} else {
			/* disable none boot CPU's irq at first */
			disable_irq(evt->ced.irq);
		}
	}

	return 0;
}

#ifdef CONFIG_OF

const struct of_device_id spacemit_counter_of_id[] = {
	{
		.compatible = "spacemit,timer-match",
	},
	{ },
};

static int __init spacemit_of_counter_init(struct device_node *np, int tid)
{
	int irq, ret;
	unsigned int cid, cpu;
	struct spacemit_timer_evt *evt;

	if (!np)
		return -EINVAL;

	ret = of_property_read_u32(np, "spacemit,timer-counter-id", &cid);
	if (ret || cid >= SPACEMIT_MAX_TIMER) {
		pr_err("Timer %d: fail to get counter id 0x%x\n", tid, cid);
		return ret;
	}

	if (of_property_read_bool(np, "spacemit,timer-broadcast"))
		cpu = SPACEMIT_TIMER_ALL_CPU;
	else {
		ret = of_property_read_u32(np,
					   "spacemit,timer-counter-cpu",
					   &cpu);
		if (ret) {
			pr_err("Timer %d:%d: fail to get cpu\n",
			       tid, cid);
			return ret;
		}
	}
	irq = irq_of_parse_and_map(np, 0);
	evt = &spacemit_timers[tid]->evt[cid];
	evt->timer = spacemit_timers[tid];
	evt->freq = spacemit_timers[tid]->freq;
	evt->irq = irq;
	evt->cpu = cpu;
	evt->cid = cid;
	evt->tid = tid;
	ret = spacemit_timer_setup(evt);
	if (ret) {
		pr_err("Timer %d:%d: fail to create clkevt\n",
		       tid, cid);
		return ret;
	}

	return 0;
}

static int __init spacemit_of_timer_init(struct device_node *np)
{
	unsigned int flag, tid, fc_freq, apb_freq, freq;
	void __iomem *base;
	struct device_node *child_np;
	const struct of_device_id *match;
	int ret = 0;

	/* timer initialization */
	base = of_iomap(np, 0);
	if (!base) {
		pr_err("Timer: fail to map register space\n");
		ret = -EINVAL;
		goto out;
	}

	flag = 0;

	/* get timer id */
	ret = of_property_read_u32(np, "spacemit,timer-id", &tid);
	if (ret || tid >= SPACEMIT_MAX_TIMER) {
		pr_err("Timer %d: fail to get timer-id with err %d\n", tid, ret);
		goto out;
	}

	/* timer's fast clock and apb frequency */
	ret = of_property_read_u32(np, "spacemit,timer-fastclk-frequency", &fc_freq);
	if (ret) {
		pr_err("Timer %d: fail to get fastclk-frequency with err %d\n",
		       tid, ret);
		goto out;
	}

	ret = of_property_read_u32(np, "spacemit,timer-apb-frequency", &apb_freq);
	if (ret) {
		pr_err("Timer %d: fail to get apb-frequency with err %d\n",
		       tid, ret);
		goto out;
	}

	ret = of_property_read_u32(np, "spacemit,timer-frequency", &freq);
	if (ret) {
		pr_err("Timer %d: fail to get timer frequency with err %d\n",
		       tid, ret);
		goto out;
	}

	/*
	 * Need use loop for more safe register's accessing,
	 * so at here dynamically calculate the loop time.
	 */
	if (!fc_freq || !apb_freq) {
		pr_err("mmp timer's fast clock or apb freq are incorrect!\n");
		ret = -EINVAL;
		goto out;
	}

	ret = spacemit_timer_init(np, tid, base, flag, fc_freq, apb_freq, freq);
	if (ret)
		goto out;

	/* counter initialization */
	for_each_child_of_node(np, child_np) {
		match = of_match_node(spacemit_counter_of_id, child_np);
		if (!of_device_is_available(child_np))
			continue;
		ret = spacemit_of_counter_init(child_np, tid);
		if (ret)
			goto out;
	}
	return 0;
out:
	if (ret)
		pr_err("Failed to get timer from dtb with error:%d\n", ret);
	return ret;
}

TIMER_OF_DECLARE(spacemit_timer, "spacemit,soc-timer", spacemit_of_timer_init);
#endif
