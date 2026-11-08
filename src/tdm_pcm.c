// SPDX-License-Identifier: GPL-2.0-only
/*
 * Lantiq VR9 TDM/PCM block as a PCM slave transport.
 *
 * A peer device drives the bit clock (DCL) and frame sync (FSC); the SoC's
 * TDM block assigns time slots relative to FSC and moves one 16-bit word
 * per channel and frame through a small per-channel buffer. Up to 16
 * channels in 16-bit mode cover up to 32 eight-bit time slots per frame.
 *
 * Interface: /dev/tdm-pcm0 with kmod-lantiq-ssc-pcm's uapi version 3
 * (include/uapi/ssc_pcm.h, unchanged), so a consumer written for
 * /dev/ssc-pcm0 works here too. A "slot" in that uapi is 16 bits; slot k is
 * TDM channel k, carrying time slots 2k and 2k + 1. Frames come out aligned
 * to FSC, in wire order (slot 2k's byte first, MSB first as on the wire).
 *
 * The engine: the VR9 has no DMA path for this block (its central DMA map
 * names no PCM port), so the CPU moves every frame. By default one hrtimer
 * per CPU runs the engine every 100 us and the block buffers two frames
 * (250 us): while one CPU keeps interrupts off (xway_nand during
 * ubihealthd's reads, about 27 ms; a console line, about 10 ms) the other
 * CPU's timer alone still empties the buffer in time. Measured loss-free on
 * the 7362 SL during NAND reads on either CPU. The receive-ready interrupt
 * of the last channel can pace it instead (use_irq=1, with the one-frame
 * buffer; in two-frame mode it fires about nine times per frame). See
 * docs/hardware.md, "Engine".
 *
 * Register access: big-endian SoC, native-endian registers: only
 * __raw_readl()/__raw_writel() on ioremap()ed bases (readl()/writel()
 * byte-swap here). The block is only read while its clock gate is on.
 */

#include <linux/clk.h>
#include <linux/cpumask.h>
#include <linux/debugfs.h>
#include <linux/delay.h>
#include <linux/err.h>
#include <linux/fs.h>
#include <linux/hrtimer.h>
#include <linux/interrupt.h>
#include <linux/io.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/kfifo.h>
#include <linux/ktime.h>
#include <linux/math64.h>
#include <linux/miscdevice.h>
#include <linux/module.h>
#include <linux/mutex.h>
#include <linux/of.h>
#include <linux/platform_device.h>
#include <linux/poll.h>
#include <linux/seq_file.h>
#include <linux/slab.h>
#include <linux/smp.h>
#include <linux/spinlock.h>
#include <linux/uaccess.h>
#include <linux/wait.h>

#include <lantiq_soc.h>

#include "ssc_pcm.h"
#include "tdm_pcm.h"
#include "tdm_pcm_regs.h"

#define TDM_DEFAULT_SLOTS		8	/* 128 bits per frame */
#define TDM_MAX_SLOTS			TDM_CHANNELS
#define TDM_MAX_FRAME_BYTES		(2 * TDM_MAX_SLOTS)
#define TDM_DEFAULT_FPB			32
#define TDM_DEFAULT_N_DESC		16	/* 64 ms: outlasts a 30 ms blackout */
#define TDM_MAX_DESC			16
#define TDM_MAX_BLOCK_BYTES		4096
#define TDM_FRAME_HZ			8000
#define TDM_FRAME_NS			(NSEC_PER_SEC / TDM_FRAME_HZ)
#define TDM_TX_QUEUE_FRAMES		256	/* 32 ms */
#define TDM_DRAIN_MAX			6	/* frames per engine run */
#define TDM_TIMERS			2	/* the VR9 has two VPEs */

static bool use_irq;
module_param(use_irq, bool, 0444);
MODULE_PARM_DESC(use_irq, "pace the engine with the receive interrupt (needs pcm_cfg with the one-frame buffer); 0: timers only (default)");

static uint poll_us = 100;
module_param(poll_us, uint, 0644);
MODULE_PARM_DESC(poll_us, "period of the per-CPU engine timers in us (taken at start)");

static bool poll_all_cpus = true;
module_param(poll_all_cpus, bool, 0644);
MODULE_PARM_DESC(poll_all_cpus, "one engine timer per CPU (default) or one only");

/*
 * With use_irq, the backstop that matters: while the interrupt's CPU keeps
 * interrupts off (a NAND read, a console line), another CPU's timer must run
 * the engine before the receive buffer is overwritten. With the one-frame
 * buffer even 75 us lost a frame now and then (runs up to ~160 us apart).
 */
static uint poll_fast_us;
module_param(poll_fast_us, uint, 0644);
MODULE_PARM_DESC(poll_fast_us, "period of the timers on CPUs other than the receive interrupt's in us (0: poll_us; taken at start)");

/*
 * Two-frame buffers (TDM_CFG_DATA_BUF clear; set it for one frame, which the
 * timers alone cannot serve in time); receive and transmit time slots start
 * one bit clock after the FSC pulse (measured on the 7362 SL: the DECT
 * chip's bit 0 is the bit period after its one-clock FSC pulse).
 */
static uint pcm_cfg = (1 << TDM_CFG_TX_OFS_S) | 1;
module_param(pcm_cfg, uint, 0644);
MODULE_PARM_DESC(pcm_cfg, "configuration bits (offsets, edges, clocking, buffer mode) set at step 3");

static uint dcl_code;
module_param(dcl_code, uint, 0644);
MODULE_PARM_DESC(dcl_code, "0: detect the bit clock rate; else force rate code (dcl_code - 1)");

static uint rx_depth;
module_param(rx_depth, uint, 0644);
MODULE_PARM_DESC(rx_depth, "receive buffer depth in frames for the loss estimate (0: 1 with the frame-synchronous buffer, else 2)");

static uint tx_lead = 1;
module_param(tx_lead, uint, 0644);
MODULE_PARM_DESC(tx_lead, "frames written ahead of the bus at start (default 1)");

static bool tx_open_drain;
module_param(tx_open_drain, bool, 0444);
MODULE_PARM_DESC(tx_open_drain, "drive the data output open drain instead of push-pull (too slow for 1 Mbit/s on the 7362 SL)");

struct tdm_pcm;

struct tdm_timer {
	struct hrtimer t;
	ktime_t first;
	ktime_t period;
	struct tdm_pcm *tp;
	int cpu;
	u64 last_ns;
	u64 max_gap_ns;			/* longest time between two runs */
	u32 runs;
};

struct tdm_rxdesc {
	u32 seq;
	u32 flags;
	u64 ts_ns;
};

struct tdm_pcm {
	struct device *dev;
	void __iomem *base;
	void __iomem *pmu;		/* status read only */
	void __iomem *gpio;		/* pin mux, single bits */
	const struct tdm_pcm_pdata *pdata;
	struct clk *clk;
	int irq_rx, irq_tx;
	struct miscdevice misc;
	struct mutex lock;		/* ioctl, open */
	bool opened;
	struct dentry *dbg;
	u32 peek_off;

	unsigned int step;
	bool clk_on;			/* we enabled clk */
	bool pmu_on;			/* we opened the PMU gate */
	bool pins_set;
	u32 pin_mask[3];		/* per port: bits we changed */
	u32 pin_save[3][4];		/* DIR, ALT0, ALT1, OD before */

	/* geometry */
	unsigned int slots;
	unsigned int frame_bytes;
	unsigned int fpb;
	unsigned int n_desc;
	unsigned int block_bytes;
	u32 ch_mask;
	u32 cfg;			/* TDM_CFG without ENABLE */

	/* receive ring of blocks; ring_lock also covers the counters */
	spinlock_t ring_lock;
	wait_queue_head_t wq;
	u8 *ring;
	struct tdm_rxdesc *desc;
	unsigned int head, tail;
	u32 seq;
	u64 frames;
	u64 dropped;
	u8 *block_buf;
	unsigned int block_fill;
	struct mutex read_lock;
	u8 *read_buf;

	/* engine */
	spinlock_t engine_lock;
	bool running;
	struct tdm_timer timer[TDM_TIMERS];
	unsigned int n_timers;
	int irq_cpu;			/* the receive interrupt's, or -1 */
	unsigned long storm_jiffy;
	unsigned int storm_count;
	bool storm;
	u32 irqs[2];			/* rx, tx */
	u32 polls;
	unsigned int depth;		/* receive buffer, frames */
	u32 lost;			/* frames lost (estimated) */
	u32 loss_events;
	u32 last_loss;			/* the latest event: frames, gap, CPU */
	u64 last_loss_gap_ns;
	int last_loss_cpu;
	u64 last_loss_ns;
	s64 win_min, ref_min;		/* loss estimator, see there */
	u32 win_frames;
	s64 max_lag_ns;
	u32 partial;			/* some channels ready, not all */
	u32 idle_runs;			/* runs that found no frame */
	u32 hist[TDM_DRAIN_MAX + 1];	/* frames per run */
	u32 tx_prime;			/* lead frames at start */
	u32 tx_full;			/* owed a frame, but no room */
	u32 gaps_250, gaps_1ms;
	u64 max_gap_ns;
	u64 last_rx_ns;
	u64 first_rx_ns;
	u64 max_run_ns;
	u32 crash;

	/* transmit queue of whole frames (writer: write(), reader: engine) */
	struct mutex write_lock;
	wait_queue_head_t tx_wq;
	struct kfifo txq;
	u8 tx_idle[SSC_PCM_MAX_FRAME_BYTES];
	u8 tx_cur[TDM_MAX_FRAME_BYTES];
	u64 tx_frames;
	u64 tx_idle_frames;
};

/* single instance: the VR9 has one TDM block */
static struct tdm_pcm *tdm_cur;

static inline u32 tdm_rd(struct tdm_pcm *tp, u32 off)
{
	u32 v = __raw_readl(tp->base + off);

	rmb();
	return v;
}

static inline void tdm_wr(struct tdm_pcm *tp, u32 off, u32 val)
{
	wmb();
	__raw_writel(val, tp->base + off);
}

/* ------------------------------------------------------------------ */
/* Clock gate                                                         */
/* ------------------------------------------------------------------ */
static bool tdm_gate_closed(struct tdm_pcm *tp)
{
	return tp->pmu && (__raw_readl(tp->pmu + LTQ_PMU_PWDSR) &
			   tp->pdata->pmu_gate);
}

static int tdm_clock_on(struct tdm_pcm *tp)
{
	int ret;

	if (tp->clk) {
		ret = clk_prepare_enable(tp->clk);
		if (ret)
			return ret;
		tp->clk_on = true;
		return 0;
	}
	if (!tp->pdata || !tp->pdata->pmu_gate)
		return 0;	/* nothing described: assume it runs */
	if (tdm_gate_closed(tp)) {
		/* the SoC code's own helper, under its lock */
		ltq_pmu_enable(tp->pdata->pmu_gate);
		tp->pmu_on = true;
	}
	if (tdm_gate_closed(tp)) {
		dev_err(tp->dev, "PMU gate 0x%08x did not open\n",
			tp->pdata->pmu_gate);
		return -EIO;
	}
	dev_info(tp->dev, "clock gate %s\n",
		 tp->pmu_on ? "opened" : "was already open");
	return 0;
}

static void tdm_clock_off(struct tdm_pcm *tp)
{
	if (tp->clk_on) {
		clk_disable_unprepare(tp->clk);
		tp->clk_on = false;
	}
	if (tp->pmu_on) {
		ltq_pmu_disable(tp->pdata->pmu_gate);
		tp->pmu_on = false;
	}
}

/* ------------------------------------------------------------------ */
/* Pins: only the bits of the pins the board lists, restored after    */
/* ------------------------------------------------------------------ */
static const u32 tdm_pin_regs[4] = {
	LTQ_GPIO_DIR, LTQ_GPIO_ALT0, LTQ_GPIO_ALT1, LTQ_GPIO_OD
};

static DEFINE_SPINLOCK(tdm_pin_lock);

static void tdm_pin_bits(void __iomem *reg, u32 mask, u32 set)
{
	u32 v = __raw_readl(reg);

	__raw_writel((v & ~mask) | (set & mask), reg);
}

static void tdm_pins_on(struct tdm_pcm *tp)
{
	const struct tdm_pcm_pdata *pd = tp->pdata;
	u32 want[3][4] = { };
	unsigned long flags;
	unsigned int i, p, r;

	if (!tp->gpio || !pd || !pd->n_pins)
		return;
	memset(tp->pin_mask, 0, sizeof(tp->pin_mask));
	for (i = 0; i < pd->n_pins; i++) {
		const struct tdm_pcm_pin *pin = &pd->pins[i];
		u32 bit = BIT(pin->gpio % 16);

		p = pin->gpio / 16;
		if (p > 2)
			continue;
		tp->pin_mask[p] |= bit;
		if (pin->out)
			want[p][0] |= bit;
		if (pin->alt0)
			want[p][1] |= bit;
		if (pin->alt1)
			want[p][2] |= bit;
		if (pin->out && pin->push_pull && !tx_open_drain)
			want[p][3] |= bit;
	}

	spin_lock_irqsave(&tdm_pin_lock, flags);
	for (p = 0; p < 3; p++) {
		void __iomem *port = tp->gpio + LTQ_GPIO_PORT(p);

		if (!tp->pin_mask[p])
			continue;
		for (r = 0; r < 4; r++)
			tp->pin_save[p][r] = __raw_readl(port + tdm_pin_regs[r]);
		/* driver type and function first, the direction last */
		tdm_pin_bits(port + LTQ_GPIO_OD, tp->pin_mask[p], want[p][3]);
		tdm_pin_bits(port + LTQ_GPIO_ALT0, tp->pin_mask[p], want[p][1]);
		tdm_pin_bits(port + LTQ_GPIO_ALT1, tp->pin_mask[p], want[p][2]);
		tdm_pin_bits(port + LTQ_GPIO_DIR, tp->pin_mask[p], want[p][0]);
	}
	spin_unlock_irqrestore(&tdm_pin_lock, flags);
	tp->pins_set = true;
}

static void tdm_pins_off(struct tdm_pcm *tp)
{
	unsigned long flags;
	unsigned int p;

	if (!tp->pins_set)
		return;
	spin_lock_irqsave(&tdm_pin_lock, flags);
	for (p = 0; p < 3; p++) {
		void __iomem *port = tp->gpio + LTQ_GPIO_PORT(p);
		u32 m = tp->pin_mask[p];

		if (!m)
			continue;
		/* the direction first: an output goes back to what it was */
		tdm_pin_bits(port + LTQ_GPIO_DIR, m, tp->pin_save[p][0]);
		tdm_pin_bits(port + LTQ_GPIO_ALT0, m, tp->pin_save[p][1]);
		tdm_pin_bits(port + LTQ_GPIO_ALT1, m, tp->pin_save[p][2]);
		tdm_pin_bits(port + LTQ_GPIO_OD, m, tp->pin_save[p][3]);
	}
	spin_unlock_irqrestore(&tdm_pin_lock, flags);
	tp->pins_set = false;
}

/* ------------------------------------------------------------------ */
/* Receive                                                            */
/* ------------------------------------------------------------------ */
/* Queue block_buf as the newest block. Called with ring_lock held. */
static void tdm_enqueue_block(struct tdm_pcm *tp)
{
	struct tdm_rxdesc *d = &tp->desc[tp->head];
	unsigned int next = (tp->head + 1) % tp->n_desc;

	memcpy(tp->ring + (size_t)tp->head * tp->block_bytes, tp->block_buf,
	       tp->block_bytes);
	d->seq = tp->seq++;
	d->flags = 0;
	d->ts_ns = ktime_get_ns();
	if (next == tp->tail) {
		/* full: drop the oldest block, flag the gap on the next */
		tp->tail = (tp->tail + 1) % tp->n_desc;
		tp->desc[tp->tail].flags |= SSC_PCM_BF_DROPPED;
		tp->dropped++;
	}
	tp->head = next;
}

/* One received frame into the block in progress; true if a block is done. */
static bool tdm_rx_frame(struct tdm_pcm *tp)
{
	tp->frames++;
	if (++tp->block_fill < tp->fpb)
		return false;
	tp->block_fill = 0;
	tdm_enqueue_block(tp);
	return true;
}

/*
 * Lost frames, estimated from time. The block raises no overflow flag; a
 * frame that is not read before the next one arrives is gone. Frames come
 * every 125 us exactly (the peer's crystal), so "receive time - frames *
 * 125 us" (the phase) stays within the engine's latency while nothing is
 * lost and jumps by 125 us per lost frame. The reference is the smallest
 * phase of the current and the previous 100 ms window, which follows the
 * slow drift between the peer's clock and ours. A lost frame is counted
 * once the phase exceeds the reference by more than the buffer holds
 * (depth frames) minus 1/8 frame; it is then folded into the phase.
 *
 * Nothing is logged here: a console line on the 7362 SL's 115200 Bd serial
 * console keeps the printing CPU's interrupts off for about 9.6 ms (measured),
 * which lost the next 76 frames, whose message lost the next ones, and so on.
 * The events are counted and the latest is kept for debugfs.
 */
#define TDM_LOSS_WIN		800	/* frames, 100 ms */

static void tdm_loss_check(struct tdm_pcm *tp, u64 now)
{
	s64 phase = (s64)now - (s64)(tp->frames + tp->lost) * TDM_FRAME_NS;
	s64 ref, excess;
	u64 gap;

	if (!tp->last_rx_ns) {
		tp->first_rx_ns = now;
		tp->win_min = phase;
		tp->ref_min = phase;
		tp->win_frames = 0;
		tp->last_rx_ns = now;
		return;
	}
	gap = now - tp->last_rx_ns;
	tp->last_rx_ns = now;
	if (gap > tp->max_gap_ns)
		tp->max_gap_ns = gap;
	if (gap > 250 * NSEC_PER_USEC)
		tp->gaps_250++;
	if (gap > NSEC_PER_MSEC)
		tp->gaps_1ms++;

	ref = min(tp->ref_min, tp->win_min);
	excess = phase - ref - (s64)(tp->depth - 1) * TDM_FRAME_NS -
		 TDM_FRAME_NS * 7 / 8;
	if (excess > 0) {
		u32 lost = div_u64(excess, TDM_FRAME_NS) + 1;

		tp->lost += lost;
		tp->loss_events++;
		tp->last_loss = lost;
		tp->last_loss_gap_ns = gap;
		tp->last_loss_cpu = smp_processor_id();
		tp->last_loss_ns = now;
		phase -= (s64)lost * TDM_FRAME_NS;
	}
	if (phase - ref > tp->max_lag_ns)
		tp->max_lag_ns = phase - ref;
	if (phase < tp->win_min)
		tp->win_min = phase;
	if (++tp->win_frames >= TDM_LOSS_WIN) {
		tp->ref_min = tp->win_min;
		tp->win_min = phase;
		tp->win_frames = 0;
	}
}

/*
 * Take every complete frame the channels hold: a frame is complete when
 * every enabled channel has a word. All channels fill and drain in step,
 * so reading channel by channel keeps the frame together.
 */
static bool tdm_rx_drain(struct tdm_pcm *tp, u64 now, unsigned int *got)
{
	unsigned int n, ch;
	bool queued = false;

	for (n = 0; n < TDM_DRAIN_MAX; n++) {
		u32 rs = tdm_rd(tp, TDM_RX_STAT) & tp->ch_mask;
		u8 *f;

		if (rs != tp->ch_mask) {
			if (rs)
				tp->partial++;
			break;
		}
		spin_lock(&tp->ring_lock);
		f = tp->block_buf + (size_t)tp->block_fill * tp->frame_bytes;
		for (ch = 0; ch < tp->slots; ch++) {
			u32 v = tdm_rd(tp, TDM_CH_DATA(ch));

			f[2 * ch] = v >> 8;
			f[2 * ch + 1] = v;
		}
		queued |= tdm_rx_frame(tp);
		spin_unlock(&tp->ring_lock);
	}

	tp->hist[n]++;
	*got = n;
	if (!n) {
		tp->idle_runs++;
		return false;
	}
	tdm_loss_check(tp, now);
	return queued;
}

/* ------------------------------------------------------------------ */
/* Transmit                                                           */
/* ------------------------------------------------------------------ */
/* Write the next queued frame, or the idle frame, to every channel. */
static bool tdm_tx_frame(struct tdm_pcm *tp)
{
	unsigned int ch;
	bool took;

	took = kfifo_out(&tp->txq, tp->tx_cur, tp->frame_bytes) ==
	       tp->frame_bytes;
	if (!took)
		memcpy(tp->tx_cur, tp->tx_idle, tp->frame_bytes);
	for (ch = 0; ch < tp->slots; ch++)
		tdm_wr(tp, TDM_CH_DATA(ch),
		       (tp->tx_cur[2 * ch] << 8) | tp->tx_cur[2 * ch + 1]);
	spin_lock(&tp->ring_lock);
	if (took)
		tp->tx_frames++;
	else
		tp->tx_idle_frames++;
	spin_unlock(&tp->ring_lock);
	return took;
}

/*
 * Transmit paced by receive: one frame out for every frame in, after a
 * lead written at the start. The transmit status cannot pace it alone:
 * with no bus clock it reports room forever. A frame is written only when
 * every channel has room, so the channels stay in step. A frame window the
 * engine misses (a lost receive frame) makes the block repeat its last
 * word on every channel; those are counted as underruns.
 */
static bool tdm_tx_fill(struct tdm_pcm *tp, unsigned int frames)
{
	u32 ready = tp->ch_mask << 16;
	bool took = false;

	while (frames--) {
		if ((tdm_rd(tp, TDM_TX_STAT) & ready) != ready) {
			tp->tx_full++;
			break;
		}
		took |= tdm_tx_frame(tp);
	}
	return took;
}

/* ------------------------------------------------------------------ */
/* Engine                                                             */
/* ------------------------------------------------------------------ */
static void tdm_service(struct tdm_pcm *tp, bool timer)
{
	unsigned long flags;
	bool rx = false, tx = false;
	unsigned int got = 0;
	u64 now, dt;

	spin_lock_irqsave(&tp->engine_lock, flags);
	if (!tp->running) {
		spin_unlock_irqrestore(&tp->engine_lock, flags);
		return;
	}
	if (timer)
		tp->polls++;
	now = ktime_get_ns();
	rx = tdm_rx_drain(tp, now, &got);
	tx = tdm_tx_fill(tp, got);
	dt = ktime_get_ns() - now;
	if (dt > tp->max_run_ns)
		tp->max_run_ns = dt;
	spin_unlock_irqrestore(&tp->engine_lock, flags);

	if (tx)
		wake_up_interruptible(&tp->tx_wq);
	if (rx)
		wake_up_interruptible(&tp->wq);
}

/*
 * The receive request is level: if it cannot be cleared (it should clear
 * with the read of the frame) it would hold the CPU. More than 100 kHz of
 * runs within one jiffy is far beyond the 8 kHz frame rate: switch the
 * interrupt off and leave the engine to the timers.
 */
static bool tdm_irq_storm(struct tdm_pcm *tp)
{
	if (time_after(jiffies, tp->storm_jiffy)) {
		tp->storm_jiffy = jiffies;
		tp->storm_count = 0;
	}
	if (++tp->storm_count < 100000 / HZ)
		return false;
	tdm_wr(tp, TDM_RX_IEN, 0);
	tdm_wr(tp, TDM_TX_IEN, 0);
	if (tp->irq_rx > 0)
		disable_irq_nosync(tp->irq_rx);
	if (tp->irq_tx > 0)
		disable_irq_nosync(tp->irq_tx);
	tp->storm = true;
	dev_err_ratelimited(tp->dev, "interrupt storm, timers only\n");
	return true;
}

static irqreturn_t tdm_irq(int irq, void *data)
{
	struct tdm_pcm *tp = data;

	tp->irqs[irq == tp->irq_tx]++;
	if (tp->storm || tdm_irq_storm(tp))
		return IRQ_HANDLED;
	tdm_service(tp, false);
	return IRQ_HANDLED;
}

static enum hrtimer_restart tdm_timer_fn(struct hrtimer *t)
{
	struct tdm_timer *pt = container_of(t, struct tdm_timer, t);
	u64 now = ktime_get_ns();

	if (pt->last_ns && now - pt->last_ns > pt->max_gap_ns)
		pt->max_gap_ns = now - pt->last_ns;
	pt->last_ns = now;
	pt->runs++;
	tdm_service(pt->tp, true);
	hrtimer_forward_now(t, pt->period);
	return HRTIMER_RESTART;
}

static void tdm_start_timer_here(void *data)
{
	struct tdm_timer *pt = data;

	hrtimer_start(&pt->t, pt->first, HRTIMER_MODE_REL_PINNED);
}

/* The CPU the receive interrupt is delivered to, or -1 if none. */
static int tdm_irq_cpu(struct tdm_pcm *tp)
{
	const struct cpumask *m;

	if (!use_irq || tp->irq_rx <= 0)
		return -1;
	m = irq_get_effective_affinity_mask(tp->irq_rx);
	if (!m || cpumask_empty(m))
		return -1;
	return cpumask_first(m);
}

/*
 * One timer per online CPU (up to two), spread over the period; the timers
 * on CPUs other than the interrupt's run every poll_fast_us. Whichever CPU
 * is free runs the engine; engine_lock keeps the runs apart.
 */
static void tdm_start_timers(struct tdm_pcm *tp)
{
	u64 slow = (u64)max(poll_us, 20U) * NSEC_PER_USEC;
	u64 fast = poll_fast_us ? (u64)max(poll_fast_us, 20U) * NSEC_PER_USEC :
		   slow;
	unsigned int cpu, n = 0;

	tp->irq_cpu = tdm_irq_cpu(tp);
	for_each_online_cpu(cpu) {
		struct tdm_timer *pt = &tp->timer[n];
		u64 period = tp->irq_cpu >= 0 && cpu != tp->irq_cpu ? fast :
			     slow;

		if (n == TDM_TIMERS || (n && !poll_all_cpus))
			break;
		hrtimer_init(&pt->t, CLOCK_MONOTONIC, HRTIMER_MODE_REL_PINNED);
		pt->t.function = tdm_timer_fn;
		pt->tp = tp;
		pt->cpu = cpu;
		pt->last_ns = 0;
		pt->max_gap_ns = 0;
		pt->runs = 0;
		pt->period = ns_to_ktime(period);
		pt->first = ns_to_ktime(period + n * period / TDM_TIMERS);
		if (!smp_call_function_single(cpu, tdm_start_timer_here, pt, 1))
			n++;
	}
	tp->n_timers = n;
}

static void tdm_reset_counters(struct tdm_pcm *tp)
{
	tp->irqs[0] = tp->irqs[1] = 0;
	tp->polls = 0;
	tp->lost = 0;
	tp->loss_events = 0;
	tp->last_loss = 0;
	tp->last_loss_gap_ns = 0;
	tp->last_loss_cpu = -1;
	tp->last_loss_ns = 0;
	tp->win_min = tp->ref_min = 0;
	tp->win_frames = 0;
	tp->max_lag_ns = 0;
	tp->partial = 0;
	tp->idle_runs = 0;
	memset(tp->hist, 0, sizeof(tp->hist));
	tp->tx_prime = 0;
	tp->tx_full = 0;
	tp->gaps_250 = tp->gaps_1ms = 0;
	tp->max_gap_ns = 0;
	tp->last_rx_ns = 0;
	tp->first_rx_ns = 0;
	tp->max_run_ns = 0;
	tp->crash = 0;
	tp->storm = false;
	tp->storm_count = 0;
	tp->block_fill = 0;
}

static int tdm_engine_start(struct tdm_pcm *tp)
{
	unsigned long flags;

	if (!tp->block_buf)
		return -ENOMEM;
	tdm_reset_counters(tp);
	tp->depth = rx_depth ?: (tp->cfg & TDM_CFG_DATA_BUF ? 1 : 2);

	tdm_wr(tp, TDM_CFG, tp->cfg | TDM_CFG_ENABLE);
	tdm_wr(tp, TDM_CH_EN, tp->ch_mask);
	dev_info(tp->dev, "enabled: CFG=0x%08x EN=0x%08x STAT=0x%08x\n",
		 tdm_rd(tp, TDM_CFG), tdm_rd(tp, TDM_CH_EN),
		 tdm_rd(tp, TDM_STAT));

	spin_lock_irqsave(&tp->engine_lock, flags);
	tp->running = true;
	/* a lead of whole frames in the transmit buffers before the first FSC */
	tp->tx_prime = min(tx_lead, (uint)TDM_DRAIN_MAX);
	tdm_tx_fill(tp, tp->tx_prime);
	spin_unlock_irqrestore(&tp->engine_lock, flags);

	if (use_irq && tp->irq_rx > 0) {
		enable_irq(tp->irq_rx);
		/* one request per frame: the last channel's word is in */
		tdm_wr(tp, TDM_RX_IEN, BIT(tp->slots - 1));
	}
	tdm_start_timers(tp);
	dev_info(tp->dev,
		 "running: %u slots, %s on cpu %d, %u timer(s) of %u/%u us, %u frames primed\n",
		 tp->slots, use_irq && tp->irq_rx > 0 ? "rx interrupt" :
		 "timers only", tp->irq_cpu, tp->n_timers,
		 (u32)ktime_to_us(tp->timer[0].period),
		 tp->n_timers > 1 ? (u32)ktime_to_us(tp->timer[1].period) : 0,
		 tp->tx_prime);
	return 0;
}

static void tdm_engine_stop(struct tdm_pcm *tp)
{
	unsigned long flags;
	unsigned int i;

	if (!tp->running)
		return;
	tdm_wr(tp, TDM_RX_IEN, 0);
	tdm_wr(tp, TDM_TX_IEN, 0);
	if (use_irq && tp->irq_rx > 0) {
		if (tp->storm)
			synchronize_irq(tp->irq_rx);
		else
			disable_irq(tp->irq_rx);
	}
	for (i = 0; i < tp->n_timers; i++)
		hrtimer_cancel(&tp->timer[i].t);

	spin_lock_irqsave(&tp->engine_lock, flags);
	tp->running = false;
	spin_unlock_irqrestore(&tp->engine_lock, flags);

	tdm_wr(tp, TDM_CFG, tp->cfg);
	dev_info(tp->dev,
		 "stopped: %llu frames, about %u lost in %u events, longest gap %llu us, largest lag %lld us\n",
		 tp->frames, tp->lost, tp->loss_events,
		 div_u64(tp->max_gap_ns, NSEC_PER_USEC),
		 div_s64(tp->max_lag_ns, NSEC_PER_USEC));
	wake_up_interruptible(&tp->wq);
	wake_up_interruptible(&tp->tx_wq);
}

/* ------------------------------------------------------------------ */
/* Steps                                                              */
/* ------------------------------------------------------------------ */
static int tdm_step_tdm(struct tdm_pcm *tp)
{
	unsigned int ch;

	tp->cfg = pcm_cfg & TDM_CFG_USER_M;
	tdm_wr(tp, TDM_CFG, tp->cfg);		/* interface off */
	tdm_wr(tp, TDM_RX_IEN, 0);
	tdm_wr(tp, TDM_TX_IEN, 0);
	tdm_wr(tp, TDM_LOOP, 0);
	tdm_wr(tp, TDM_FREQ, dcl_code ?
	       TDM_FREQ_FORCE | ((dcl_code - 1) & TDM_FREQ_CODE_M) : 0);
	tdm_wr(tp, TDM_CH_EN, 0);
	tdm_wr(tp, TDM_CH_MODE, tp->ch_mask);	/* 16-bit channels */
	for (ch = 0; ch < TDM_CHANNELS; ch++) {
		u32 ts = ch < tp->slots ? 2 * ch : 0;

		tdm_wr(tp, TDM_CH_SLOT(ch), (ts << TDM_CH_SLOT_RX_S) | ts);
	}
	tdm_wr(tp, TDM_CH_EN, tp->ch_mask);
	tdm_wr(tp, TDM_STAT, TDM_STAT_CRASH_A | TDM_STAT_CRASH_B);
	dev_info(tp->dev,
		 "configured: CFG=0x%08x MODE=0x%08x EN=0x%08x STAT=0x%08x\n",
		 tdm_rd(tp, TDM_CFG), tdm_rd(tp, TDM_CH_MODE),
		 tdm_rd(tp, TDM_CH_EN), tdm_rd(tp, TDM_STAT));
	return 0;
}

static void tdm_free_buffers(struct tdm_pcm *tp)
{
	kfree(tp->ring);
	kfree(tp->desc);
	kfree(tp->block_buf);
	kfree(tp->read_buf);
	kfifo_free(&tp->txq);
	tp->ring = NULL;
	tp->desc = NULL;
	tp->block_buf = NULL;
	tp->read_buf = NULL;
}

static void tdm_teardown(struct tdm_pcm *tp)
{
	tdm_engine_stop(tp);
	if (tp->step >= SSC_PCM_STEP_CLOCK) {
		tdm_wr(tp, TDM_CFG, 0);
		tdm_wr(tp, TDM_CH_EN, 0);
	}
	tdm_pins_off(tp);
	tdm_clock_off(tp);

	mutex_lock(&tp->read_lock);
	mutex_lock(&tp->write_lock);
	tp->head = tp->tail = 0;
	tp->seq = 0;
	tp->frames = 0;
	tp->dropped = 0;
	tp->tx_frames = 0;
	tp->tx_idle_frames = 0;
	if (kfifo_initialized(&tp->txq))
		kfifo_reset(&tp->txq);
	tp->step = SSC_PCM_STEP_OFF;
	mutex_unlock(&tp->write_lock);
	mutex_unlock(&tp->read_lock);
}

static int tdm_step(struct tdm_pcm *tp, unsigned int n)
{
	int ret;

	if (n == SSC_PCM_STEP_OFF) {
		tdm_teardown(tp);
		return 0;
	}
	if (n != tp->step + 1)
		return -EINVAL;

	switch (n) {
	case SSC_PCM_STEP_CLOCK:
		ret = tdm_clock_on(tp);
		break;
	case SSC_PCM_STEP_IFACE:
		tdm_pins_on(tp);
		ret = 0;
		break;
	case SSC_PCM_STEP_TDM:
		ret = tdm_step_tdm(tp);
		break;
	case SSC_PCM_STEP_DMA:
		ret = tdm_engine_start(tp);
		break;
	default:
		ret = -EINVAL;
	}
	if (!ret)
		tp->step = n;
	return ret;
}

static int tdm_config(struct tdm_pcm *tp, struct ssc_pcm_config *c)
{
	unsigned int slots = c->slots ?: TDM_DEFAULT_SLOTS;
	unsigned int fpb = c->frames_per_block ?: TDM_DEFAULT_FPB;
	unsigned int ndesc = c->n_desc ?: TDM_DEFAULT_N_DESC;

	if (tp->step != SSC_PCM_STEP_OFF)
		return -EBUSY;
	if (slots < 1 || slots > TDM_MAX_SLOTS)
		return -EINVAL;
	if (fpb < 1 || fpb * slots * 2 > TDM_MAX_BLOCK_BYTES)
		return -EINVAL;
	if (ndesc < 2 || ndesc > TDM_MAX_DESC)
		return -EINVAL;

	mutex_lock(&tp->read_lock);
	mutex_lock(&tp->write_lock);
	tdm_free_buffers(tp);
	tp->slots = slots;
	tp->frame_bytes = slots * 2;
	tp->ch_mask = GENMASK(slots - 1, 0);
	tp->fpb = fpb;
	tp->n_desc = ndesc;
	tp->block_bytes = fpb * tp->frame_bytes;
	tp->ring = kzalloc((size_t)ndesc * tp->block_bytes, GFP_KERNEL);
	tp->desc = kcalloc(ndesc, sizeof(*tp->desc), GFP_KERNEL);
	tp->block_buf = kzalloc(tp->block_bytes, GFP_KERNEL);
	tp->read_buf = kzalloc(tp->block_bytes, GFP_KERNEL);
	if (!tp->ring || !tp->desc || !tp->block_buf || !tp->read_buf ||
	    kfifo_alloc(&tp->txq, TDM_TX_QUEUE_FRAMES * tp->frame_bytes,
			GFP_KERNEL)) {
		tdm_free_buffers(tp);
		mutex_unlock(&tp->write_lock);
		mutex_unlock(&tp->read_lock);
		return -ENOMEM;
	}
	mutex_unlock(&tp->write_lock);
	mutex_unlock(&tp->read_lock);

	c->slots = slots;
	c->frames_per_block = fpb;
	c->n_desc = ndesc;
	return 0;
}

/* Registers worth a look; only read while the clock gate is open. */
static const u32 tdm_regs_dump[8] = {
	TDM_CFG, TDM_STAT, TDM_RX_STAT, TDM_TX_STAT,
	TDM_CH_EN, TDM_CH_MODE, TDM_FREQ, TDM_RX_IEN,
};

static void tdm_regs(struct tdm_pcm *tp, struct ssc_pcm_regs *r)
{
	unsigned int i;

	memset(r, 0, sizeof(*r));
	r->step = tp->step;
	if (tp->step < SSC_PCM_STEP_CLOCK)
		return;
	r->valid = 1;
	for (i = 0; i < ARRAY_SIZE(tdm_regs_dump); i++)
		r->reg[i] = tdm_rd(tp, tdm_regs_dump[i]);
}

static long tdm_ioctl_locked(struct tdm_pcm *tp, unsigned int cmd,
			     void __user *argp)
{
	struct ssc_pcm_config cfg;
	struct ssc_pcm_regs regs;
	struct ssc_pcm_stats st;
	struct ssc_pcm_tx_idle idle;
	unsigned long flags;
	__u32 n;
	int ret;

	switch (cmd) {
	case SSC_PCM_IOC_CONFIG:
		if (copy_from_user(&cfg, argp, sizeof(cfg)))
			return -EFAULT;
		ret = tdm_config(tp, &cfg);
		if (ret)
			return ret;
		return copy_to_user(argp, &cfg, sizeof(cfg)) ? -EFAULT : 0;
	case SSC_PCM_IOC_STEP:
		if (copy_from_user(&n, argp, sizeof(n)))
			return -EFAULT;
		if (n != SSC_PCM_STEP_OFF && !tp->block_buf)
			return -EINVAL;	/* CONFIG first */
		return tdm_step(tp, n);
	case SSC_PCM_IOC_REGS:
		tdm_regs(tp, &regs);
		return copy_to_user(argp, &regs, sizeof(regs)) ? -EFAULT : 0;
	case SSC_PCM_IOC_STATS:
		memset(&st, 0, sizeof(st));
		spin_lock_irqsave(&tp->engine_lock, flags);
		spin_lock(&tp->ring_lock);
		st.frames = tp->frames;
		st.blocks = tp->seq;
		st.dropped = tp->dropped;
		st.tx_frames = tp->tx_frames;
		st.tx_idle = tp->tx_idle_frames;
		/* a frame not read in time is lost, and its transmit window
		 * repeated the previous word: one count each
		 */
		st.overruns = tp->lost;
		st.tx_underruns = tp->lost;
		st.rx_irqs = tp->irqs[0];
		st.tx_irqs = tp->irqs[1];
		st.polls = tp->polls;
		spin_unlock(&tp->ring_lock);
		spin_unlock_irqrestore(&tp->engine_lock, flags);
		if (tp->frame_bytes && kfifo_initialized(&tp->txq))
			st.tx_queued = kfifo_len(&tp->txq) / tp->frame_bytes;
		st.step = tp->step;
		st.block_bytes = tp->block_bytes;
		return copy_to_user(argp, &st, sizeof(st)) ? -EFAULT : 0;
	case SSC_PCM_IOC_TX_IDLE:
		if (copy_from_user(&idle, argp, sizeof(idle)))
			return -EFAULT;
		/* taken at the next frame; a torn idle frame is harmless */
		memcpy(tp->tx_idle, idle.frame, sizeof(tp->tx_idle));
		return 0;
	default:
		return -ENOTTY;
	}
}

/* ------------------------------------------------------------------ */
/* read / write / poll                                                */
/* ------------------------------------------------------------------ */
static ssize_t tdm_read(struct file *file, char __user *buf, size_t count,
			loff_t *ppos)
{
	struct tdm_pcm *tp = file->private_data;
	bool nonblock = file->f_flags & O_NONBLOCK;
	struct ssc_pcm_block hdr;
	struct tdm_rxdesc *d;
	unsigned long flags;
	size_t need;
	int ret;

	if (tp->step != SSC_PCM_STEP_DMA)
		return -ENODATA;
	if (mutex_lock_interruptible(&tp->read_lock))
		return -ERESTARTSYS;
	for (;;) {
		if (!tp->running || !tp->ring) {
			ret = -ENODATA;
			goto out;
		}
		need = sizeof(hdr) + tp->block_bytes;
		if (count < need) {
			ret = -EINVAL;
			goto out;
		}
		spin_lock_irqsave(&tp->ring_lock, flags);
		if (tp->tail != tp->head)
			break;
		spin_unlock_irqrestore(&tp->ring_lock, flags);
		if (nonblock) {
			ret = -EAGAIN;
			goto out;
		}
		mutex_unlock(&tp->read_lock);
		ret = wait_event_interruptible(tp->wq, tp->tail != tp->head ||
					       !tp->running);
		if (ret)
			return ret;
		if (mutex_lock_interruptible(&tp->read_lock))
			return -ERESTARTSYS;
	}

	d = &tp->desc[tp->tail];
	memset(&hdr, 0, sizeof(hdr));
	hdr.magic = SSC_PCM_BLOCK_MAGIC;
	hdr.seq = d->seq;
	hdr.flags = d->flags;
	hdr.len = tp->block_bytes;
	hdr.ts_ns = d->ts_ns;
	memcpy(tp->read_buf, tp->ring + (size_t)tp->tail * tp->block_bytes,
	       tp->block_bytes);
	tp->tail = (tp->tail + 1) % tp->n_desc;
	spin_unlock_irqrestore(&tp->ring_lock, flags);

	if (copy_to_user(buf, &hdr, sizeof(hdr)) ||
	    copy_to_user(buf + sizeof(hdr), tp->read_buf, tp->block_bytes))
		ret = -EFAULT;
	else
		ret = need;
out:
	mutex_unlock(&tp->read_lock);
	return ret;
}

static unsigned int tdm_tx_room(struct tdm_pcm *tp)
{
	return kfifo_avail(&tp->txq) / tp->frame_bytes;
}

static ssize_t tdm_write(struct file *file, const char __user *buf,
			 size_t count, loff_t *ppos)
{
	struct tdm_pcm *tp = file->private_data;
	bool nonblock = file->f_flags & O_NONBLOCK;
	u8 frame[TDM_MAX_FRAME_BYTES];
	unsigned int frames, i;
	ssize_t ret;

	if (tp->step != SSC_PCM_STEP_DMA)
		return -ENODATA;
	if (mutex_lock_interruptible(&tp->write_lock))
		return -ERESTARTSYS;
	for (;;) {
		if (!tp->running) {
			ret = -ENODATA;
			goto out;
		}
		if (!count || count % tp->frame_bytes) {
			ret = -EINVAL;
			goto out;
		}
		if (tdm_tx_room(tp))
			break;
		if (nonblock) {
			ret = -EAGAIN;
			goto out;
		}
		mutex_unlock(&tp->write_lock);
		ret = wait_event_interruptible(tp->tx_wq, !tp->running ||
					       tdm_tx_room(tp));
		if (ret)
			return ret;
		if (mutex_lock_interruptible(&tp->write_lock))
			return -ERESTARTSYS;
	}

	/* whole frames only: a partial frame would shift every later one */
	frames = min_t(size_t, tdm_tx_room(tp), count / tp->frame_bytes);
	for (i = 0; i < frames; i++) {
		if (copy_from_user(frame, buf + (size_t)i * tp->frame_bytes,
				   tp->frame_bytes))
			break;
		kfifo_in(&tp->txq, frame, tp->frame_bytes);
	}
	ret = i ? (ssize_t)i * tp->frame_bytes : -EFAULT;
out:
	mutex_unlock(&tp->write_lock);
	return ret;
}

static __poll_t tdm_poll(struct file *file, poll_table *wait)
{
	struct tdm_pcm *tp = file->private_data;
	__poll_t mask = 0;

	poll_wait(file, &tp->wq, wait);
	poll_wait(file, &tp->tx_wq, wait);
	if (tp->step != SSC_PCM_STEP_DMA || !tp->running)
		return 0;
	if (tp->tail != tp->head)
		mask |= EPOLLIN | EPOLLRDNORM;
	if (tdm_tx_room(tp))
		mask |= EPOLLOUT | EPOLLWRNORM;
	return mask;
}

static int tdm_open(struct inode *inode, struct file *file)
{
	struct tdm_pcm *tp = tdm_cur;
	int ret = 0;

	if (!tp)
		return -ENODEV;
	if (mutex_lock_interruptible(&tp->lock))
		return -ERESTARTSYS;
	if (tp->opened) {
		ret = -EBUSY;
	} else {
		tp->opened = true;
		file->private_data = tp;
	}
	mutex_unlock(&tp->lock);
	return ret;
}

static int tdm_release(struct inode *inode, struct file *file)
{
	struct tdm_pcm *tp = file->private_data;

	mutex_lock(&tp->lock);
	tdm_teardown(tp);
	tp->opened = false;
	mutex_unlock(&tp->lock);
	return 0;
}

static long tdm_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
	struct tdm_pcm *tp = file->private_data;
	struct ssc_pcm_info info;
	long ret;

	if (cmd == SSC_PCM_IOC_INFO) {
		memset(&info, 0, sizeof(info));
		info.version = SSC_PCM_IOC_VERSION;
		info.step = tp->step;
		info.slots = tp->slots;
		info.frame_bytes = tp->frame_bytes;
		strscpy(info.name, TDM_PCM_DRV_NAME, sizeof(info.name));
		return copy_to_user((void __user *)arg, &info, sizeof(info)) ?
			-EFAULT : 0;
	}
	if (mutex_lock_interruptible(&tp->lock))
		return -ERESTARTSYS;
	ret = tdm_ioctl_locked(tp, cmd, (void __user *)arg);
	mutex_unlock(&tp->lock);
	return ret;
}

static const struct file_operations tdm_fops = {
	.owner		= THIS_MODULE,
	.open		= tdm_open,
	.release	= tdm_release,
	.read		= tdm_read,
	.write		= tdm_write,
	.poll		= tdm_poll,
	.unlocked_ioctl	= tdm_ioctl,
	.llseek		= noop_llseek,
};

/* ------------------------------------------------------------------ */
/* debugfs: engine statistics and single register reads               */
/* ------------------------------------------------------------------ */
static int tdm_dbg_stats_show(struct seq_file *m, void *v)
{
	struct tdm_pcm *tp = m->private;
	unsigned long flags;
	u64 frames, first, last, span;
	u32 hist[TDM_DRAIN_MAX + 1];
	unsigned int i;

	spin_lock_irqsave(&tp->engine_lock, flags);
	frames = tp->frames;
	first = tp->first_rx_ns;
	last = tp->last_rx_ns;
	memcpy(hist, tp->hist, sizeof(hist));
	spin_unlock_irqrestore(&tp->engine_lock, flags);
	span = last > first ? last - first : 0;

	seq_printf(m, "step %u running %d slots %u frame_bytes %u\n",
		   tp->step, tp->running, tp->slots, tp->frame_bytes);
	seq_printf(m, "frames %llu blocks %u dropped %llu\n", frames, tp->seq,
		   tp->dropped);
	/* frames per second from the receive timestamps: the FSC rate */
	seq_printf(m, "span_us %llu frame_rate_mhz %llu\n",
		   div_u64(span, NSEC_PER_USEC),
		   span ? div64_u64(frames * 1000ULL * NSEC_PER_SEC, span) : 0);
	seq_printf(m, "irqs rx %u tx %u polls %u storm %d timers %u\n",
		   tp->irqs[0], tp->irqs[1], tp->polls, tp->storm,
		   tp->n_timers);
	seq_printf(m, "lost %u loss_events %u depth %u max_lag_us %lld partial %u idle_runs %u\n",
		   tp->lost, tp->loss_events, tp->depth,
		   div_s64(tp->max_lag_ns, NSEC_PER_USEC), tp->partial,
		   tp->idle_runs);
	seq_puts(m, "frames_per_run");
	for (i = 0; i <= TDM_DRAIN_MAX; i++)
		seq_printf(m, " %u", hist[i]);
	seq_putc(m, '\n');
	seq_printf(m, "max_gap_us %llu gaps_250us %u gaps_1ms %u max_run_us %llu\n",
		   div_u64(tp->max_gap_ns, NSEC_PER_USEC), tp->gaps_250,
		   tp->gaps_1ms,
		   div_u64(tp->max_run_ns, NSEC_PER_USEC));
	seq_printf(m, "tx_frames %llu tx_idle %llu tx_lead %u tx_full %u\n",
		   tp->tx_frames, tp->tx_idle_frames, tp->tx_prime,
		   tp->tx_full);
	if (tp->loss_events)
		seq_printf(m, "last_loss %u gap_us %llu cpu %d at_ms %llu\n",
			   tp->last_loss,
			   div_u64(tp->last_loss_gap_ns, NSEC_PER_USEC),
			   tp->last_loss_cpu,
			   div_u64(tp->last_loss_ns - first, NSEC_PER_MSEC));
	seq_printf(m, "irq_cpu %d\n", tp->irq_cpu);
	for (i = 0; i < tp->n_timers; i++)
		seq_printf(m, "timer cpu %d period_us %lld runs %u max_gap_us %llu\n",
			   tp->timer[i].cpu, ktime_to_us(tp->timer[i].period),
			   tp->timer[i].runs,
			   div_u64(tp->timer[i].max_gap_ns, NSEC_PER_USEC));
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tdm_dbg_stats);

/* Pin mux state of ports 0 to 2 (registers pinctrl-xway reads too). */
static int tdm_dbg_pins_show(struct seq_file *m, void *v)
{
	struct tdm_pcm *tp = m->private;
	static const u32 ports[] = { 0, 1, 2 };
	unsigned int i;

	if (!tp->gpio)
		return -ENODEV;
	for (i = 0; i < ARRAY_SIZE(ports); i++) {
		void __iomem *p = tp->gpio + LTQ_GPIO_PORT(ports[i]);

		seq_printf(m, "port %u: in 0x%04x dir 0x%04x alt0 0x%04x alt1 0x%04x od 0x%04x\n",
			   ports[i], __raw_readl(p + 0x04) & 0xffff,
			   __raw_readl(p + LTQ_GPIO_DIR) & 0xffff,
			   __raw_readl(p + LTQ_GPIO_ALT0) & 0xffff,
			   __raw_readl(p + LTQ_GPIO_ALT1) & 0xffff,
			   __raw_readl(p + LTQ_GPIO_OD) & 0xffff);
	}
	if (tp->pmu)
		seq_printf(m, "pmu pwdsr 0x%08x (tdm gate %s)\n",
			   __raw_readl(tp->pmu + LTQ_PMU_PWDSR),
			   tdm_gate_closed(tp) ? "closed" : "open");
	return 0;
}
DEFINE_SHOW_ATTRIBUTE(tdm_dbg_pins);

/*
 * peek: write a register offset, read its value. One register per read,
 * only with the clock gate open, only offsets of the documented map.
 */
static ssize_t tdm_dbg_peek_write(struct file *file, const char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct tdm_pcm *tp = file->private_data;
	u32 off;
	int ret;

	ret = kstrtou32_from_user(ubuf, len, 0, &off);
	if (ret)
		return ret;
	if (off & 3 || off > TDM_CH_DATA(TDM_CHANNELS - 1))
		return -EINVAL;
	tp->peek_off = off;
	return len;
}

static ssize_t tdm_dbg_peek_read(struct file *file, char __user *ubuf,
				 size_t len, loff_t *ppos)
{
	struct tdm_pcm *tp = file->private_data;
	char buf[32];
	int n;

	if (*ppos)
		return 0;
	if (tp->step < SSC_PCM_STEP_CLOCK)
		return -EIO;
	n = scnprintf(buf, sizeof(buf), "0x%02x 0x%08x\n", tp->peek_off,
		      tdm_rd(tp, tp->peek_off));
	return simple_read_from_buffer(ubuf, len, ppos, buf, n);
}

static const struct file_operations tdm_dbg_peek_fops = {
	.owner	= THIS_MODULE,
	.open	= simple_open,
	.read	= tdm_dbg_peek_read,
	.write	= tdm_dbg_peek_write,
	.llseek	= noop_llseek,
};

/*
 * scope: a logic-analyser view of the bus pads. Reading the file samples
 * the GPIO input registers (the ones gpio-xway reads) back to back with
 * interrupts off for a few milliseconds, one character per sample: bit 0
 * FSC (GPIO 0), bit 1 DCL (GPIO 40), bit 2 DI (GPIO 41), bit 3 DO
 * (GPIO 25), as a hex digit. Write 2 first to sample port 2 only (DCL and
 * DI, faster). The first line gives the sample count and the time taken.
 */
#define TDM_SCOPE_SAMPLES	8192

struct tdm_scope {
	char *text;
	size_t len;
};

static u32 tdm_scope_mode;

static int tdm_dbg_scope_open(struct inode *inode, struct file *file)
{
	struct tdm_pcm *tp = inode->i_private;
	void __iomem *in0, *in1, *in2;
	struct tdm_scope *sc;
	unsigned long flags;
	u64 t0, t1;
	u16 *smp;
	size_t i, n;

	if (!tp->gpio)
		return -ENODEV;
	sc = kzalloc(sizeof(*sc), GFP_KERNEL);
	if (!sc)
		return -ENOMEM;
	smp = kcalloc(TDM_SCOPE_SAMPLES, 3 * sizeof(*smp), GFP_KERNEL);
	sc->text = kmalloc(TDM_SCOPE_SAMPLES + 128, GFP_KERNEL);
	if (!smp || !sc->text) {
		kfree(smp);
		kfree(sc->text);
		kfree(sc);
		return -ENOMEM;
	}
	in0 = tp->gpio + LTQ_GPIO_PORT(0) + 0x04;
	in1 = tp->gpio + LTQ_GPIO_PORT(1) + 0x04;
	in2 = tp->gpio + LTQ_GPIO_PORT(2) + 0x04;

	local_irq_save(flags);
	t0 = ktime_get_ns();
	if (tdm_scope_mode == 2) {
		for (i = 0; i < TDM_SCOPE_SAMPLES; i++)
			smp[3 * i + 2] = __raw_readl(in2);
	} else {
		for (i = 0; i < TDM_SCOPE_SAMPLES; i++) {
			smp[3 * i] = __raw_readl(in0);
			smp[3 * i + 1] = __raw_readl(in1);
			smp[3 * i + 2] = __raw_readl(in2);
		}
	}
	t1 = ktime_get_ns();
	local_irq_restore(flags);

	n = scnprintf(sc->text, 128, "samples %u ns %llu mode %u\n",
		      TDM_SCOPE_SAMPLES, t1 - t0, tdm_scope_mode);
	for (i = 0; i < TDM_SCOPE_SAMPLES; i++) {
		unsigned int v = (smp[3 * i] & BIT(0) ? 1 : 0) |
				 (smp[3 * i + 2] & BIT(8) ? 2 : 0) |
				 (smp[3 * i + 2] & BIT(9) ? 4 : 0) |
				 (smp[3 * i + 1] & BIT(9) ? 8 : 0);

		sc->text[n++] = hex_asc[v];
	}
	sc->text[n++] = '\n';
	sc->len = n;
	kfree(smp);
	file->private_data = sc;
	return 0;
}

static ssize_t tdm_dbg_scope_read(struct file *file, char __user *ubuf,
				  size_t len, loff_t *ppos)
{
	struct tdm_scope *sc = file->private_data;

	if (!sc)
		return 0;
	return simple_read_from_buffer(ubuf, len, ppos, sc->text, sc->len);
}

static ssize_t tdm_dbg_scope_write(struct file *file, const char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	int ret = kstrtou32_from_user(ubuf, len, 0, &tdm_scope_mode);

	return ret ? ret : len;
}

static int tdm_dbg_scope_release(struct inode *inode, struct file *file)
{
	struct tdm_scope *sc = file->private_data;

	if (sc)
		kfree(sc->text);
	kfree(sc);
	return 0;
}

static const struct file_operations tdm_dbg_scope_fops = {
	.owner		= THIS_MODULE,
	.open		= tdm_dbg_scope_open,
	.read		= tdm_dbg_scope_read,
	.write		= tdm_dbg_scope_write,
	.release	= tdm_dbg_scope_release,
	.llseek		= noop_llseek,
};

/*
 * bench: how long a single bus access can take on this CPU. Write the
 * mode (0: a TDM register, 1: a GPIO input register, 2: no access, only
 * the clock, 3: a memory barrier only, 4: a TDM register read followed by
 * a barrier), then read: interrupts off for 60 ms, the longest iteration
 * and the iteration count come back. Used to show that the NAND driver's
 * reads stall register access from the other CPU as well.
 */
static u32 tdm_bench_mode;

static int tdm_dbg_bench_show(struct seq_file *m, void *v)
{
	struct tdm_pcm *tp = m->private;
	void __iomem *reg = NULL;
	u64 start, t, prev, worst = 0, n = 0;
	unsigned long flags;

	if (tdm_bench_mode == 0) {
		if (tp->step < SSC_PCM_STEP_CLOCK)
			return -EIO;
		reg = tp->base + TDM_STAT;
	} else if (tdm_bench_mode == 4) {
		if (tp->step < SSC_PCM_STEP_CLOCK)
			return -EIO;
		reg = tp->base + TDM_STAT;
	} else if (tdm_bench_mode == 1) {
		if (!tp->gpio)
			return -ENODEV;
		reg = tp->gpio + LTQ_GPIO_PORT(2) + 0x04;
	}
	local_irq_save(flags);
	start = prev = ktime_get_ns();
	do {
		if (reg)
			__raw_readl(reg);
		if (tdm_bench_mode >= 3)
			mb();
		t = ktime_get_ns();
		if (t - prev > worst)
			worst = t - prev;
		prev = t;
		n++;
	} while (t - start < 60 * NSEC_PER_MSEC);
	local_irq_restore(flags);
	seq_printf(m, "mode %u cpu %d iterations %llu longest_ns %llu\n",
		   tdm_bench_mode, smp_processor_id(), n, worst);
	return 0;
}

static int tdm_dbg_bench_open(struct inode *inode, struct file *file)
{
	return single_open(file, tdm_dbg_bench_show, inode->i_private);
}

static ssize_t tdm_dbg_bench_write(struct file *file, const char __user *ubuf,
				   size_t len, loff_t *ppos)
{
	int ret = kstrtou32_from_user(ubuf, len, 0, &tdm_bench_mode);

	return ret ? ret : len;
}

static const struct file_operations tdm_dbg_bench_fops = {
	.owner		= THIS_MODULE,
	.open		= tdm_dbg_bench_open,
	.read		= seq_read,
	.write		= tdm_dbg_bench_write,
	.llseek		= seq_lseek,
	.release	= single_release,
};

/* ------------------------------------------------------------------ */
/* probe / remove                                                     */
/* ------------------------------------------------------------------ */
static int tdm_probe(struct platform_device *pdev)
{
	struct device *dev = &pdev->dev;
	struct tdm_pcm *tp;
	int ret;

	if (tdm_cur)
		return -EBUSY;
	tp = devm_kzalloc(dev, sizeof(*tp), GFP_KERNEL);
	if (!tp)
		return -ENOMEM;
	tp->dev = dev;
	tp->pdata = dev_get_platdata(dev);
	mutex_init(&tp->lock);
	mutex_init(&tp->read_lock);
	mutex_init(&tp->write_lock);
	spin_lock_init(&tp->ring_lock);
	spin_lock_init(&tp->engine_lock);
	init_waitqueue_head(&tp->wq);
	init_waitqueue_head(&tp->tx_wq);
	tp->slots = TDM_DEFAULT_SLOTS;
	tp->frame_bytes = 2 * tp->slots;
	tp->irq_cpu = -1;

	tp->base = devm_platform_ioremap_resource(pdev, 0);
	if (IS_ERR(tp->base))
		return PTR_ERR(tp->base);

	tp->clk = devm_clk_get_optional(dev, NULL);
	if (IS_ERR(tp->clk))
		return dev_err_probe(dev, PTR_ERR(tp->clk), "module clock\n");

	/* Registers of the SoC code: mapped, never requested. */
	if (tp->pdata && tp->pdata->pmu_gate) {
		tp->pmu = devm_ioremap(dev, LTQ_PMU_PHYS, LTQ_PMU_MAP_SIZE);
		if (!tp->pmu)
			return -ENOMEM;
	}
	if (tp->pdata && tp->pdata->n_pins) {
		tp->gpio = devm_ioremap(dev, LTQ_GPIO_PHYS, LTQ_GPIO_MAP_SIZE);
		if (!tp->gpio)
			return -ENOMEM;
	}

	tp->irq_rx = platform_get_irq_optional(pdev, 0);
	tp->irq_tx = platform_get_irq_optional(pdev, 1);
	if (tp->irq_rx > 0) {
		ret = devm_request_irq(dev, tp->irq_rx, tdm_irq,
				       IRQF_NO_AUTOEN, "tdm-pcm rx", tp);
		if (ret) {
			dev_warn(dev, "rx irq %d: %d, timers only\n",
				 tp->irq_rx, ret);
			tp->irq_rx = 0;
		}
	}

	tp->misc.name = "tdm-pcm0";
	tp->misc.minor = MISC_DYNAMIC_MINOR;
	tp->misc.fops = &tdm_fops;
	tp->misc.mode = 0600;
	tp->misc.parent = dev;
	ret = misc_register(&tp->misc);
	if (ret)
		return ret;

	tp->dbg = debugfs_create_dir("tdm-pcm", NULL);
	debugfs_create_file("stats", 0400, tp->dbg, tp, &tdm_dbg_stats_fops);
	debugfs_create_file("pins", 0400, tp->dbg, tp, &tdm_dbg_pins_fops);
	debugfs_create_file("peek", 0600, tp->dbg, tp, &tdm_dbg_peek_fops);
	debugfs_create_file("scope", 0600, tp->dbg, tp, &tdm_dbg_scope_fops);
	debugfs_create_file("bench", 0600, tp->dbg, tp, &tdm_dbg_bench_fops);

	platform_set_drvdata(pdev, tp);
	tdm_cur = tp;
	dev_info(dev, "VR9 TDM/PCM transport ready (rx irq %d, tx irq %d)\n",
		 tp->irq_rx, tp->irq_tx);
	return 0;
}

static void tdm_remove(struct platform_device *pdev)
{
	struct tdm_pcm *tp = platform_get_drvdata(pdev);

	tdm_cur = NULL;
	debugfs_remove_recursive(tp->dbg);
	misc_deregister(&tp->misc);
	tdm_teardown(tp);
	tdm_free_buffers(tp);
}

static const struct of_device_id tdm_of_match[] = {
	{ .compatible = "lantiq,xrx200-tdm-pcm" },
	{ }
};
MODULE_DEVICE_TABLE(of, tdm_of_match);

static struct platform_driver tdm_driver = {
	.probe		= tdm_probe,
	.remove		= tdm_remove,
	.driver		= {
		.name		= TDM_PCM_DRV_NAME,
		.of_match_table	= tdm_of_match,
		/* an open fd would outlive a sysfs unbind */
		.suppress_bind_attrs = true,
	},
};

static int __init tdm_init(void)
{
	int ret = platform_driver_register(&tdm_driver);

	if (ret)
		return ret;
	ret = tdm_pcm_fallback_init();
	if (ret)
		pr_warn(TDM_PCM_DRV_NAME ": bring-up fallback failed: %d\n",
			ret);
	return 0;
}

static void __exit tdm_exit(void)
{
	tdm_pcm_fallback_exit();
	platform_driver_unregister(&tdm_driver);
}

module_init(tdm_init);
module_exit(tdm_exit);

MODULE_AUTHOR("openDECTcell project");
MODULE_DESCRIPTION("Lantiq VR9 TDM/PCM block as a PCM slave transport");
MODULE_LICENSE("GPL");
