// SPDX-License-Identifier: GPL-2.0-only
/*
 * Copyright (c) Qualcomm Technologies, Inc. and/or its subsidiaries.
 *
 * QCOM TSC PTP : Linux driver for Time Stamp Counter Hardware.
 *
 */

#define pr_fmt(fmt) "qcom_tsc: %s: " fmt, __func__

#include <linux/delay.h>
#include <linux/clk.h>
#include <linux/device.h>
#include <linux/err.h>
#include <linux/gpio.h>
#include <linux/init.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/pinctrl/consumer.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/platform_device.h>
#include <linux/of_irq.h>

/* Register offset definitions */
#define TSCSS_TSC_CONTROL_CNTCR			0x0
#define TSCSS_TSC_PIPELINE_STAGES               0x4
#define TSCSS_TSC_CONTROL_CNTCV_LO		0x8
#define TSCSS_TSC_CONTROL_CNTCV_HI		0xC
#define TSCSS_TSC_CONTROL_CNTCV_FRAC		0x10
#define TSCSS_TSC_SLEEP_INCR_VAL_LO		0x24
#define TSCSS_TSC_SLEEP_INCR_VAL_HI		0x28
#define TSCSS_TSC_DRIFT_CORRECT_INCR_VAL	0x2C
#define TSCSS_TSC_DRIFT_CORRECT_DURATION	0x30
#define TSCSS_TSC_DRIFT_CORRECT_DURATION2	0x34
#define TSCSS_TSC_DRIFT_CORRECT_CMD		0x38
#define TSCSS_TSC_DRIFT_CORRECT_CMD2		0x3C
#define TSCSS_TSC_ROLLOVER_VAL			0x3C
#define TSCSS_TSC_SPARE				0x40
#define TSCSS_TSC_OFFSET_LO			0x50
#define TSCSS_TSC_OFFSET_HI			0x54
#define TSCSS_TSC_FUSA_CFG_STAT			0xF54
#define TSCSS_TSC_READ_CNTCV_LO			0x1000
#define TSCSS_TSC_READ_CNTCV_HI			0x1004
#define TSCSS_TSC_HW_PRELOAD_VAL_LO		0x0060
#define TSCSS_TSC_HW_PRELOAD_VAL_HI		0x0064

#define TSCSS_TSC_SLICE_ETU_CFG			0x0
#define TSCSS_TSC_SLICE_ETU_STATUS		0x4
#define TSCSS_ETU_SLICE_TSC_TS_LO		0x8
#define TSCSS_ETU_SLICE_TSC_TS_HI		0xC
#define TSCSS_ETU_SLICE_TS_EVENT_TYPE		0x10
#define TSCSS_ETU_SLICE_GCTR_TS_LO		0x20
#define TSCSS_ETU_SLICE_GCTR_TS_HI		0x24
#define TSCSS_ETU_SLICE_FIFO_CLR		0x30
#define TSCSS_ETU_SLICE_SW_TRIG_CFG		0x34
#define TSCSS_ETU_SLICE_TIMER_TRIG_PERIOD	0x38
#define MAX_ETU_SLICE				32

#define TSCSS_TSC_TIMER_TSC_SOF_PERIOD		0x0
#define TSCSS_TSC_TIMER_PULSE_WIDTH_REG		0x4
#define TSCSS_TSC_TIMER_PPS			0x8
#define TSCSS_TSC_TIMER_CFG			0xC
#define TSCSS_TSC_TIMER_PHASE_OFFSET		0x10
#define TSCSS_TSC_TIMER_FUSA_STATUS		0xF4
#define TSCSS_TIMER_COUNT			16
#define TSCSS_RESOLUTION			4
#define TSCSS_TSC_TIMER_BASE(base, idx, offset) \
			(base + idx * 0x100 + offset)

#define TSCSS_DRIFT_CORR_PPM			1000000L
#define TSCSS_MIN_SUBPERIOD			6
#define INCR_VAL				0x8
#define TSC_PRELOAD_POLLING_DELAY_MS		100
#define NSEC_SHFT				32
#define NSEC_MASK				GENMASK_ULL(31, 0)
#define NSEC					1000000000ULL
#define XO_MHZ					19200000
#define TSCSS_TSC_ETU_SLICE_BASE(reg_base, num, offset)	\
			(reg_base + num * 0x1000 + offset)
#define TSC_WRITE_READBACK_RETRIES		2
#define TSC_RESOLUTION				0x4

struct qcom_etu_slice {
	char name[10];
	struct ptp_clock *ptp_clock;
	void __iomem *etu_baseaddr;
	u64 etu_tsc_timestamp;
	u64 last_sec;
	u64 global_qtimer;
	int extts_enable;
	int extts_irq;
	int extts_index;
	int extts_event_sel;
	int extts_slice_num;
	int extts_event_type;
	u32 etu_tsc_sec;
	u32 etu_tsc_nsec;
	u32 etu_gctr_sec;
	u32 etu_gctr_nsec;
	bool extts_present;
};

struct qcom_ptp_tsc {
	struct	device *dev;
	void __iomem *baseaddr;
	void __iomem *etu_baseaddr;
	void __iomem *timer_baseaddr;
	void __iomem *mux_addr;
	struct clk *tsc_cfg_ahb_clk;
	struct clk *tsc_cntr_clk;
	struct clk *tsc_etu_clk;
	struct ptp_clock *ptp_clock;
	struct ptp_clock_info  ptp_clock_info;
	struct qcom_etu_slice etu_slice[MAX_ETU_SLICE];
	int pps_enable;
	int total_etu_cnt;
	u32 pulse_gen_ref_cnt;
	bool is_jump;
	bool tsc_nsec_update;
	bool tsc_hw_preload;
	bool frame_pulse_gen;
	spinlock_t reg_lock;
	struct delayed_work tsc_preload_poll_work;
	bool tsc_safety;
	u32 configured_slice_mask;
};

/* Write and readback safety critical registers */
static inline void tsc_write_readback(void __iomem *address, u32 regval, u32 rb_mask)
{
	int retries = TSC_WRITE_READBACK_RETRIES;
	u32 readback;

	while (retries-- > 0) {
		writel_relaxed(regval, address);
		readback = readl_relaxed(address);

		if ((regval & rb_mask) == (readback & rb_mask))
			return;

		udelay(1);

	}

	pr_err("TSC Readback failed : expected = 0x%x, actual = 0x%x\n",
		regval, readl_relaxed(address));
}

static void tsc_preload_poll(struct work_struct *work)
{
	struct qcom_ptp_tsc *timer = container_of(work, struct qcom_ptp_tsc,
						tsc_preload_poll_work.work);
	u32 regval;

	regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	/* Check for the HW_PRELOAD_STATUS and disable HW_PRELOAD */
	if (!(regval & BIT(14))) {
		regval &= ~BIT(2);
		tsc_write_readback(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR, regval, BIT(2));
		pr_info("TSC CNTCR: 0x%x HW_PRELOAD is disabled\n",
			readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR));
		return;
	}

	pr_debug("TSC CNTCR: 0x%x HW_PRELOAD_STATUS is not cleared\n", regval);
	mod_delayed_work(system_highpri_wq, &timer->tsc_preload_poll_work,
			msecs_to_jiffies(TSC_PRELOAD_POLLING_DELAY_MS));
}

static int qcom_ptp_tsc_is_enabled(void __iomem *addr)
{
	return readl_relaxed(addr + TSCSS_TSC_CONTROL_CNTCR) & BIT(0);
}

static void qcom_tod_read(struct qcom_ptp_tsc *timer, struct timespec64 *ts)
{
	u64 temp, final;
	u32 sec, nsec;

	if (!qcom_ptp_tsc_is_enabled(timer->baseaddr)) {
		pr_debug("TSC is not enabled\n");
		return;
	}

	sec = readl_relaxed(timer->baseaddr + TSCSS_TSC_READ_CNTCV_HI);
	nsec = readl_relaxed(timer->baseaddr + TSCSS_TSC_READ_CNTCV_LO);

	pr_debug("CNTR_HI: 0x%x, sec %lld\n",
			readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_HI), sec);
	pr_debug("CNTR_LO: 0x%x nsec %ld\n",
			readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO), nsec);

	if (timer->tsc_nsec_update) {
		temp = sec;
		final = (temp << NSEC_SHFT) | nsec;
		sec = div_u64_rem(final, NSEC, &nsec);
		pr_debug("tsc_nsec_update: %d, sec %lld, nsec %ld\n",
						timer->tsc_nsec_update, sec, nsec);
	}

	ts->tv_sec = sec;
	ts->tv_nsec = nsec;
}

static void qcom_ptp_enable_tsc_hw_preload(struct qcom_ptp_tsc *timer, struct timespec64 ts)
{
	u32 regval;
	int timeout = 500;

	/* Enable HW_PRELOAD */
	regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	regval |= BIT(2);
	tsc_write_readback(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR, regval, BIT(2));

	/* Program PRELOAD registers */
	writel_relaxed(ts.tv_sec, timer->baseaddr + TSCSS_TSC_HW_PRELOAD_VAL_HI);
	writel_relaxed(ts.tv_nsec, timer->baseaddr + TSCSS_TSC_HW_PRELOAD_VAL_LO);

	pr_debug("HW_PRELOAD_VAL_HI: 0x%x\n",
		readl_relaxed(timer->baseaddr + TSCSS_TSC_HW_PRELOAD_VAL_HI));
	pr_debug("HW_PRELOAD_VAL_LO: 0x%x\n",
		readl_relaxed(timer->baseaddr + TSCSS_TSC_HW_PRELOAD_VAL_LO));

	/* Check for the HW_PRELOAD_STATUS and start poll thread */
	while (timeout-- > 0) {
		regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
		if (regval & BIT(14)) {
			pr_debug("TSC CNTR: 0x%x HW_PRELOAD is enabled\n", regval);
			mod_delayed_work(system_highpri_wq, &timer->tsc_preload_poll_work,
					msecs_to_jiffies(TSC_PRELOAD_POLLING_DELAY_MS));
			return;
		}
		udelay(1);
	}

	pr_warn("TSC CNTR: 0x%x HW_PRELOAD enable failed\n",
		readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR));
}

static int qcom_ptp_update_tsc_cntr(struct qcom_ptp_tsc *timer,
				struct timespec64 offset)
{
	u64 timestamp = 0;
	u32 regval;

	/* Update to 1ns resolution */
	if (timer->tsc_nsec_update) {
		timestamp =  offset.tv_sec * NSEC + offset.tv_nsec;
		writel_relaxed((timestamp >> NSEC_SHFT),
				timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_HI);
		writel_relaxed(timestamp & NSEC_MASK, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO);
	} else {
		writel_relaxed(offset.tv_sec, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_HI);
		writel_relaxed(offset.tv_nsec, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO);
	}

	pr_debug("Timestamp %llu: sec: %lld, nsec: %ld\n", timestamp,
							offset.tv_sec, offset.tv_nsec);

	pr_debug("CNTR_HI: 0x%x\n", readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_HI));
	pr_debug("CNTR_LO: 0x%x\n", readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO));

	/* Enable the counter */
	regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	regval |= BIT(0);

	tsc_write_readback(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR, regval, BIT(0));

	return 0;
}

static void qcom_ptp_update_tsc_offset(struct qcom_ptp_tsc *timer,
				struct timespec64 offset)
{
	u64 timestamp = 0;

	if (timer->tsc_nsec_update) {
		timestamp =  offset.tv_sec * NSEC + offset.tv_nsec;
		writel_relaxed(timestamp & NSEC_MASK, timer->baseaddr + TSCSS_TSC_OFFSET_LO);
		writel_relaxed((timestamp >> NSEC_SHFT), timer->baseaddr + TSCSS_TSC_OFFSET_HI);
	} else {
		writel_relaxed(offset.tv_nsec, timer->baseaddr + TSCSS_TSC_OFFSET_LO);
		writel_relaxed(offset.tv_sec, timer->baseaddr + TSCSS_TSC_OFFSET_HI);
	}

	pr_debug("OFFSET_HI: 0x%x\n", readl_relaxed(timer->baseaddr + TSCSS_TSC_OFFSET_HI));
	pr_debug("OFFSET_LO: 0x%x\n", readl_relaxed(timer->baseaddr + TSCSS_TSC_OFFSET_LO));
	pr_debug("CNTCV_HI: 0x%x\n", readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_HI));
	pr_debug("CNTCV_LO: 0x%x\n", readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO));
}

/*
 * PTP clock operations
 */
static int qcom_ptp_enable_drift_corr(struct qcom_ptp_tsc *timer, bool neg_adj, u32 subperiod)
{
	u32 cfg, cmd;

	cfg = readl_relaxed(timer->baseaddr + TSCSS_TSC_DRIFT_CORRECT_DURATION);
	if (!neg_adj) {
		cfg |= BIT(31);
		timer->is_jump = true;
	} else {
		cfg &= ~BIT(31);
		timer->is_jump = false;
	}

	tsc_write_readback(timer->baseaddr + TSCSS_TSC_DRIFT_CORRECT_DURATION, cfg, BIT(31));

	writel_relaxed(INCR_VAL, timer->baseaddr + TSCSS_TSC_DRIFT_CORRECT_INCR_VAL);

	tsc_write_readback(timer->baseaddr + TSCSS_TSC_DRIFT_CORRECT_DURATION2, subperiod,
			GENMASK(27, 0));

	cmd = readl_relaxed(timer->baseaddr + TSCSS_TSC_DRIFT_CORRECT_CMD);
	cmd |= BIT(2);
	tsc_write_readback(timer->baseaddr + TSCSS_TSC_DRIFT_CORRECT_CMD, cmd, BIT(2));

	pr_debug("is_jump:%d cfg: %u subperiod:%u cmd:%u\n", timer->is_jump, cfg, subperiod, cmd);

	return 0;
}

static int qcom_ptp_adjfine(struct ptp_clock_info *ptp, long scaled_ppm)
{
	struct qcom_ptp_tsc *timer = container_of(ptp, struct qcom_ptp_tsc,
			ptp_clock_info);
	long ppm = scaled_ppm_to_ppb(scaled_ppm)/1000;
	bool neg_adj = false;
	u32 subperiod;

	if (ppm == 0)
		return 0;

	if (ppm < 0) {
		neg_adj = true;
		ppm = -ppm;
	}

	/*
	 * ppm is the no. of cycles drifting per 1000000 cycles
	 * One cycle drift happens every N cycles
	 * N is calculated as N = 1 cycle * 1000000 / ppm
	 * So for every N cycles, 1 cycle correction should be applied,
	 * which is the subperiod.
	 */
	subperiod = TSCSS_DRIFT_CORR_PPM / ppm;
	subperiod = (subperiod < TSCSS_MIN_SUBPERIOD) ? TSCSS_MIN_SUBPERIOD : subperiod;

	qcom_ptp_enable_drift_corr(timer, neg_adj, subperiod);

	return 0;
}

static int qcom_ptp_adjtime(struct ptp_clock_info *ptp, s64 delta)
{
	unsigned long flags;
	struct qcom_ptp_tsc *timer = container_of(ptp, struct qcom_ptp_tsc,
			ptp_clock_info);
	struct timespec64 offset;

	spin_lock_irqsave(&timer->reg_lock, flags);

	if (timer->pulse_gen_ref_cnt) {
		pr_err("Frame pulse generation must be disabled for offset correction\n");
		spin_unlock_irqrestore(&timer->reg_lock, flags);
		return -EOPNOTSUPP;
	}

	offset = ns_to_timespec64(delta);
	pr_debug("sec: %lld, nsec: %ld\n", offset.tv_sec, offset.tv_nsec);

	qcom_ptp_update_tsc_offset(timer, offset);

	spin_unlock_irqrestore(&timer->reg_lock, flags);

	return 0;
}

static int qcom_ptp_gettime(struct ptp_clock_info *ptp, struct timespec64 *ts)
{
	unsigned long flags;
	struct qcom_ptp_tsc *timer = container_of(ptp, struct qcom_ptp_tsc,
								ptp_clock_info);

	spin_lock_irqsave(&timer->reg_lock, flags);
	qcom_tod_read(timer, ts);
	spin_unlock_irqrestore(&timer->reg_lock, flags);
	return 0;
}

/**
 * qcom_ptp_settime - Set the current time on the hardware clock
 * @ptp: ptp clock structure
 * @ts: timespec64 containing the new time for the cycle counter
 */
static int qcom_ptp_settime(struct ptp_clock_info *ptp, const struct timespec64 *ts)
{
	struct qcom_ptp_tsc *timer = container_of(ptp, struct qcom_ptp_tsc, ptp_clock_info);
	struct timespec64 delta, tod;
	struct timespec64 offset;
	unsigned long flags;
	int ret;

	spin_lock_irqsave(&timer->reg_lock, flags);

	pr_debug("TS.sec %lld TS.tv_nsec %ld\n", ts->tv_sec,  ts->tv_nsec);

	if (!qcom_ptp_tsc_is_enabled(timer->baseaddr)) {
		/* Update the Counter */
		offset.tv_sec = ts->tv_sec;
		offset.tv_nsec = ts->tv_nsec;

		ret = qcom_ptp_update_tsc_cntr(timer, offset);

		spin_unlock_irqrestore(&timer->reg_lock, flags);
		return ret;
	}

	/* Get the current timer value */
	qcom_tod_read(timer, &tod);

	/* Subtract the current reported time from our desired time */
	delta = timespec64_sub((struct timespec64)*ts, tod);

	pr_debug("Delta.sec %lld delta.tv_nsec %ld\n", delta.tv_sec,  delta.tv_nsec);

	/* Update the Counter */
	qcom_ptp_update_tsc_offset(timer, delta);

	spin_unlock_irqrestore(&timer->reg_lock, flags);
	return 0;
}

static void qcom_etu_event_handler(struct qcom_etu_slice *etu, struct qcom_ptp_tsc *timer)
{
	struct ptp_clock_event extts_event;
	u64 ts, temp;
	u32 regval;

	etu->etu_tsc_sec = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
				etu->extts_slice_num, TSCSS_ETU_SLICE_TSC_TS_HI));

	etu->etu_tsc_nsec = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
				etu->extts_slice_num, TSCSS_ETU_SLICE_TSC_TS_LO));

	if (!timer->tsc_nsec_update) {
		ts = etu->etu_tsc_sec * NSEC + etu->etu_tsc_nsec;
	} else {
		temp = etu->etu_tsc_sec;
		ts = (temp << NSEC_SHFT) | etu->etu_tsc_nsec;
	}

	etu->etu_gctr_sec = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
				etu->extts_slice_num, TSCSS_ETU_SLICE_GCTR_TS_HI));

	etu->etu_gctr_nsec = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
				etu->extts_slice_num, TSCSS_ETU_SLICE_GCTR_TS_LO));

	etu->extts_event_type = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
				etu->extts_slice_num, TSCSS_ETU_SLICE_TS_EVENT_TYPE));

	/* Concatenate GCTR_TS_HI(31:0) & GCTR_TS_LO(31:8) and divide with 19.2MHz */
	etu->global_qtimer = (((u64)etu->etu_gctr_sec << 24) |
			(etu->etu_gctr_nsec >> 8));

	if (ts != etu->etu_tsc_timestamp) {
		extts_event.type = PTP_CLOCK_EXTTS;
		extts_event.index = etu->extts_index;
		extts_event.timestamp = ts;

		pr_debug("type:%d index:%d TSC-timestamp:%llu\n", extts_event.type,
				extts_event.index, extts_event.timestamp);

		ptp_clock_event(etu->ptp_clock, &extts_event);

		if (etu->extts_event_sel == 0) {
			extts_event.timestamp = etu->global_qtimer;

			pr_debug("type:%d index:%d GCTR-timestamp:%llu\n", extts_event.type,
					extts_event.index, extts_event.timestamp);

			ptp_clock_event(etu->ptp_clock, &extts_event);
		}
	}
	etu->etu_tsc_timestamp = ts;

	pr_debug("etu_tsc_sec:%u etu_tsc_nsec:%u etu_tsc_timestamp:%llu\n",
			etu->etu_tsc_sec, etu->etu_tsc_nsec, etu->etu_tsc_timestamp);

	pr_debug("etu_gctr_sec:%u etu_gctr_nsec:%u global_qtimer:%x\n",
			etu->etu_gctr_sec, etu->etu_gctr_nsec, etu->global_qtimer);

	regval = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
				etu->extts_slice_num, TSCSS_TSC_SLICE_ETU_CFG));
	regval &= ~BIT(17);
	regval &= ~BIT(16);

	tsc_write_readback(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr, etu->extts_slice_num,
				TSCSS_TSC_SLICE_ETU_CFG), regval, GENMASK(17, 16));

	/* FIFO CLR */
	writel_relaxed(0x7, TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
				etu->extts_slice_num, TSCSS_ETU_SLICE_FIFO_CLR));

	/* Enable GCTR_TS_EN & TSCTR_TS_EN*/
	regval |= BIT(16) | BIT(17);

	tsc_write_readback(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr, etu->extts_slice_num,
				 TSCSS_TSC_SLICE_ETU_CFG), regval, GENMASK(17, 16));
}

static irqreturn_t qcom_etu_irq_handler(int irq, void *data)
{
	struct qcom_etu_slice *etu = (struct qcom_etu_slice *)data;
	struct ptp_clock_event extts_event;
	u32 regval, status;

	status = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
					etu->extts_slice_num, TSCSS_TSC_SLICE_ETU_STATUS));

	pr_debug("Slice %d extts_enable %d status 0%x\n",
					etu->extts_slice_num, etu->extts_enable, status);

	if (etu->extts_enable && (status & GENMASK(5, 0)))
		qcom_etu_event_handler(etu, data);

	return IRQ_HANDLED;
}

static irqreturn_t qcom_etu_summary_irq_handler(int irq, void *data)
{
	struct qcom_etu_slice *etu;
	struct qcom_ptp_tsc *timer;
	unsigned long mask;
	u32 status;
	int slice;

	timer = data;
	mask = timer->configured_slice_mask;

	for_each_set_bit(slice, &mask, MAX_ETU_SLICE) {
		etu = &timer->etu_slice[slice];
		status = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(etu->etu_baseaddr,
						slice, TSCSS_TSC_SLICE_ETU_STATUS));

		if (etu->extts_enable && (status & GENMASK(5, 0)))
			qcom_etu_event_handler(etu, timer);
	}

	return IRQ_HANDLED;
}

static irqreturn_t qcom_fusa_irq_handler(int irq, void *data)
{
	struct qcom_ptp_tsc *timer = data;
	u32 regval, cfg;
	int i;

	regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_FUSA_CFG_STAT);

	/* Check if counter error or valid error */
	if (regval & BIT(1) || regval & BIT(2)) {
		regval |= BIT(0);
		tsc_write_readback(timer->baseaddr + TSCSS_TSC_FUSA_CFG_STAT, regval, BIT(0));

		regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_FUSA_CFG_STAT);
		regval &= ~BIT(0);
		tsc_write_readback(timer->baseaddr + TSCSS_TSC_FUSA_CFG_STAT, regval, BIT(0));
	}

	/* Check all the timers and clear the error */
	for (i = 0; i < TSCSS_TIMER_COUNT; i++) {
		regval = readl_relaxed(TSCSS_TSC_TIMER_BASE(timer->timer_baseaddr,
					i, TSCSS_TSC_TIMER_FUSA_STATUS));
		/* Check if pulse width, pps or approx error occurred */
		if (regval & BIT(0) || regval & BIT(1) || regval & BIT(2)) {
			cfg = readl_relaxed(TSCSS_TSC_TIMER_BASE(timer->timer_baseaddr,
					    i, TSCSS_TSC_TIMER_CFG));
			cfg |= BIT(31);
			tsc_write_readback(TSCSS_TSC_TIMER_BASE(timer->timer_baseaddr, i,
				TSCSS_TSC_TIMER_CFG), cfg, BIT(31));

			cfg &= ~BIT(31);
			tsc_write_readback(TSCSS_TSC_TIMER_BASE(timer->timer_baseaddr, i,
				TSCSS_TSC_TIMER_CFG), cfg, BIT(31));
		}
	}

	return IRQ_HANDLED;
}

static void qcom_tsc_configure_etu(struct qcom_ptp_tsc *timer, int slice)
{
	void __iomem *base = timer->etu_baseaddr;
	u32 regval;

	/* Register for the IRQ */
	regval = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(base, slice, TSCSS_TSC_SLICE_ETU_CFG));
	regval |= (timer->etu_slice[slice].extts_event_sel << 4) & GENMASK(9, 4);

	tsc_write_readback(TSCSS_TSC_ETU_SLICE_BASE(base, slice, TSCSS_TSC_SLICE_ETU_CFG), regval,
			GENMASK(9, 4));

	regval = readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(base, slice, TSCSS_TSC_SLICE_ETU_CFG));

	/* Enable GCTR_TS_EN & TSCTR_TS_EN*/
	regval |= BIT(16) | BIT(17);
	/* Interrupt MASK enable */
	regval |= BIT(31);

	/* Enable rising edge config */
	regval |= BIT(0);
	tsc_write_readback(TSCSS_TSC_ETU_SLICE_BASE(base, slice, TSCSS_TSC_SLICE_ETU_CFG), regval,
			BIT(0));

	/* Enable SW trigger for slice with event_sel as 0.*/
	if (timer->etu_slice[slice].extts_event_sel == 0)
		writel_relaxed(BIT(1), TSCSS_TSC_ETU_SLICE_BASE(base, slice,
			       TSCSS_ETU_SLICE_SW_TRIG_CFG));

	/* Bitmask of configured slices.*/
	timer->configured_slice_mask |= BIT(slice);

	pr_debug("ETU_SLICE#%d: 0x%x\n", slice,
		readl_relaxed(TSCSS_TSC_ETU_SLICE_BASE(base, slice, TSCSS_TSC_SLICE_ETU_CFG)));
}

static void qcom_tsc_set_timer_phase(struct qcom_ptp_tsc *timer,
				     struct ptp_clock_request *rq)
{
	void __iomem *base = timer->timer_baseaddr;
	u32 regval;

	regval = rq->perout.phase.sec * NSEC + rq->perout.phase.nsec;
	do_div(regval, TSCSS_RESOLUTION);
	tsc_write_readback(TSCSS_TSC_TIMER_BASE(base, rq->perout.index,
				TSCSS_TSC_TIMER_PHASE_OFFSET), regval, GENMASK(31, 0));
}

static void qcom_tsc_disable_timer(struct qcom_ptp_tsc *timer,
				   unsigned int index)
{
	void __iomem *base = timer->timer_baseaddr;
	u32 regval;

	regval = readl_relaxed(TSCSS_TSC_TIMER_BASE(base, index,
				TSCSS_TSC_TIMER_CFG));
	regval &= ~BIT(0);
	tsc_write_readback(TSCSS_TSC_TIMER_BASE(base, index, TSCSS_TSC_TIMER_CFG), regval, BIT(0));

	pr_debug("Frame pulse generation for timer%d disabled\n", index);
}

static bool qcom_tsc_timer_is_enabled(struct qcom_ptp_tsc *timer, unsigned int index)
{
	return readl_relaxed(TSCSS_TSC_TIMER_BASE(timer->timer_baseaddr, index,
					TSCSS_TSC_TIMER_CFG)) & BIT(0);
}

static void qcom_tsc_configure_timer(struct qcom_ptp_tsc *timer,
				     struct ptp_clock_request *rq)
{
	void __iomem *base = timer->timer_baseaddr;
	unsigned long flags;
	u32 regval;

	/* If the timer is already enabled, update only phase offset */
	if (!!qcom_tsc_timer_is_enabled(timer, rq->perout.index)) {
		if (rq->perout.flags & PTP_PEROUT_PHASE)
			qcom_tsc_set_timer_phase(timer, rq);
		return;
	}

	/* Configure the delay between 2 frame pulses */
	regval = rq->perout.period.sec * NSEC + rq->perout.period.nsec;
	tsc_write_readback(TSCSS_TSC_TIMER_BASE(base, rq->perout.index,
				TSCSS_TSC_TIMER_TSC_SOF_PERIOD), regval, GENMASK(31, 0));

	/* Fractional frame rate */
	if (rq->perout.period.reserved != 0) {
		regval = rq->perout.period.reserved;
		writel_relaxed(regval, TSCSS_TSC_TIMER_BASE(base,
				rq->perout.index, TSCSS_TSC_TIMER_PPS));
	}

	/* To stretch the o/p pulse by n clock cycles */
	if (rq->perout.flags & PTP_PEROUT_DUTY_CYCLE) {
		regval = rq->perout.on.sec * NSEC + rq->perout.on.nsec;
		do_div(regval, TSCSS_RESOLUTION);
		tsc_write_readback(TSCSS_TSC_TIMER_BASE(base, rq->perout.index,
				TSCSS_TSC_TIMER_PULSE_WIDTH_REG), regval, GENMASK(31, 0));
	}

	/* Propagate the pulses to Camera Subsystem and ETU */
	regval = readl_relaxed(TSCSS_TSC_TIMER_BASE(base,
				rq->perout.index, TSCSS_TSC_TIMER_CFG));
	regval |= BIT(2);
	tsc_write_readback(TSCSS_TSC_TIMER_BASE(base, rq->perout.index,
			TSCSS_TSC_TIMER_CFG), regval, BIT(2));

	/* Enable the timer */
	regval = readl_relaxed(TSCSS_TSC_TIMER_BASE(base,
				rq->perout.index, TSCSS_TSC_TIMER_CFG));
	regval |= BIT(0);
	tsc_write_readback(TSCSS_TSC_TIMER_BASE(base, rq->perout.index,
			TSCSS_TSC_TIMER_CFG), regval, BIT(0));

	spin_lock_irqsave(&timer->reg_lock, flags);
	timer->pulse_gen_ref_cnt++;
	spin_unlock_irqrestore(&timer->reg_lock, flags);

	/* Add phase offset to the o/p pulse */
	if (rq->perout.flags & PTP_PEROUT_PHASE) {
		qcom_tsc_set_timer_phase(timer, rq);
	}
}

static int qcom_ptp_enable(struct ptp_clock_info *ptp,
			  struct ptp_clock_request *rq, int on)
{
	struct qcom_ptp_tsc *timer = container_of(ptp, struct qcom_ptp_tsc,
							ptp_clock_info);
	struct timespec64 ts;
	unsigned long flags;
	u32 regval;
	int slice;

	pr_debug("Request Type %d\n", rq->type);

	switch (rq->type) {
	case PTP_CLK_REQ_PPS:
		timer->pps_enable = 1;
		return 0;
	case PTP_CLK_REQ_EXTTS:
		pr_debug("PTP_CLK_REQ_EXTTS: Request external Index %d\n", rq->extts.index);

		if (rq->extts.index > timer->total_etu_cnt)
			return -EINVAL;
		else if (!on)
			return 0;

		for (slice = 0; slice < MAX_ETU_SLICE; slice++) {
			if (!timer->etu_slice[slice].extts_present)
				continue;

			if (rq->extts.index == timer->etu_slice[slice].extts_index) {
				pr_debug("slice %d, index %d, etu_index %d\n", slice,
					rq->extts.index, timer->etu_slice[slice].extts_index);
				qcom_tsc_configure_etu(timer,
					timer->etu_slice[slice].extts_slice_num);
				timer->etu_slice[slice].extts_enable = true;
			}

		}
		return 0;
	case PTP_CLK_REQ_PEROUT:
		if (timer->tsc_hw_preload) {
			if (timer->pulse_gen_ref_cnt) {
				pr_err("Frame pulse generation must be disabled for hw_preload\n");
				return -EOPNOTSUPP;
			}

			if (!rq->perout.period.sec) {
				/* Get the current timer value */
				qcom_tod_read(timer, &ts);
			} else {
				pr_debug("PTP_CLK_REQ_PEROUT: sec:%u nsec:%u\n",
					rq->perout.period.sec, rq->perout.period.nsec);
				ts.tv_sec = rq->perout.period.sec;
				ts.tv_nsec = rq->perout.period.nsec;
			}

			/* Preload TSC with tv_sec += 1 and tv_nsec = 0 values */
			ts.tv_sec += 1;
			ts.tv_nsec = 0;
			qcom_ptp_enable_tsc_hw_preload(timer, ts);
		}

		if (timer->frame_pulse_gen) {
			if (!!on) {
				qcom_tsc_configure_timer(timer, rq);
			} else if (!!qcom_tsc_timer_is_enabled(timer, rq->perout.index)) {
				if (rq->perout.flags & PTP_PEROUT_PHASE) {
					qcom_tsc_set_timer_phase(timer, rq);
				} else {
					qcom_tsc_disable_timer(timer, rq->perout.index);
					spin_lock_irqsave(&timer->reg_lock, flags);
					timer->pulse_gen_ref_cnt--;
					spin_unlock_irqrestore(&timer->reg_lock, flags);
				}
			}
			pr_debug("PTP_CLK_REQ_PEROUT: Configured timer%d on=%d\n",
				 rq->perout.index, on);
		}

		return 0;
	default:
		break;
	}

	return -EOPNOTSUPP;
}

static struct ptp_clock_info qcom_ptp_clock_info = {
	.owner    = THIS_MODULE,
	.name     = "QCOM TSC",
	.max_adj  = 999999999,
	/* The number of external time stamp channels. */
	.n_ext_ts = 1,
	.n_per_out = 16,
	.pps = 1,
	.adjfine  = qcom_ptp_adjfine,
	.adjtime  = qcom_ptp_adjtime,
	.gettime64  = qcom_ptp_gettime,
	.settime64 = qcom_ptp_settime,
	.enable   = qcom_ptp_enable,
};

/* --- Sysfs Interface --- */

static ssize_t tsc_src_ctrl_show(struct device *dev, struct device_attribute *attr, char *buf)
{
	struct qcom_ptp_tsc *timer = dev_get_drvdata(dev);
	u32 val;

	val = readl_relaxed(timer->mux_addr);

	return sysfs_emit(buf, "%u\n", val);
}

static ssize_t tsc_src_ctrl_store(struct device *dev, struct device_attribute *attr,
			 const char *buf, size_t count)
{
	struct qcom_ptp_tsc *timer = dev_get_drvdata(dev);
	bool val;
	u32 regval;
	int ret;

	ret = kstrtobool(buf, &val);
	if (ret)
		return ret;

	regval = readl_relaxed(timer->mux_addr);

	/* BIT(0) selects timestamp source:
	 * 0 = default source (MD)
	 * 1 = alternate source (SD)
	 */
	if (!val)
		regval &= ~BIT(0);
	else
		regval |= BIT(0);

	writel_relaxed(regval, timer->mux_addr);

	return count;
}

static DEVICE_ATTR_RW(tsc_src_ctrl);

/* module operations */

static int qcom_ptp_tsc_remove(struct platform_device *pdev)
{
	struct qcom_ptp_tsc *timer = platform_get_drvdata(pdev);

	if (timer->ptp_clock) {
		if (timer->mux_addr)
			device_remove_file(&pdev->dev, &dev_attr_tsc_src_ctrl);
		ptp_clock_unregister(timer->ptp_clock);
		timer->ptp_clock = NULL;
	}

	return 0;
}

static int qcom_tsc_etu_get_data(struct platform_device *pdev,
		 struct qcom_ptp_tsc *timer)
{
	struct device *dev = &pdev->dev;
	struct resource *r_mem;
	struct device_node *child;
	struct pinctrl *pinctrl;
	int ret, summary_irq;

	r_mem = platform_get_resource_byname(pdev, IORESOURCE_MEM, "etu");
	if (!r_mem) {
		dev_err(&pdev->dev, "No ETU resource defined\n");
		return 0;
	}

	timer->etu_baseaddr = devm_ioremap_resource(&pdev->dev, r_mem);
	if (IS_ERR(timer->etu_baseaddr))
		return PTR_ERR(timer->etu_baseaddr);

	summary_irq = platform_get_irq_byname(pdev, "etu_summary_irq");

	if (summary_irq > 0) {
		ret = devm_request_irq(dev, summary_irq,
				qcom_etu_summary_irq_handler, IRQF_TRIGGER_HIGH,
				"etu_summary_irq", timer);
		if (ret) {
			dev_err(&pdev->dev, "Request_summary_irq failed:%d: err:%d\n",
				summary_irq, ret);
			return ret;
		}
	}

	for_each_available_child_of_node(dev->of_node, child) {
		const char *name;
		u32 sel, slice;

		ret = of_property_read_u32(child, "qcom,etu-event-sel", &sel);
		if (ret) {
			pr_debug("missing qcom,etu-event-sel\n");
			continue;
		}

		ret = of_property_read_u32(child, "qcom,etu-slice", &slice);
		if (ret) {
			pr_debug("missing qcom,etu-event-slice\n");
			continue;
		}

		ret = of_property_read_string(child, "qcom,etu-event-name", &name);
		if (ret) {
			pr_debug("missing qcom,etu-event-name\n");
			continue;
		}

		timer->etu_slice[slice].etu_baseaddr = timer->etu_baseaddr;
		timer->etu_slice[slice].extts_index = slice;
		timer->etu_slice[slice].extts_event_sel = sel;
		timer->etu_slice[slice].extts_slice_num = slice;
		timer->etu_slice[slice].extts_present = true;
		timer->etu_slice[slice].ptp_clock = timer->ptp_clock;

		strscpy(timer->etu_slice[slice].name, name, sizeof(timer->etu_slice[slice].name));

		if (summary_irq < 0) {
			timer->etu_slice[slice].extts_irq = of_irq_get_byname(child, name);
			ret = devm_request_irq(dev, timer->etu_slice[slice].extts_irq,
					qcom_etu_irq_handler, IRQF_TRIGGER_RISING,
					timer->etu_slice[slice].name,
					(void *)&timer->etu_slice[slice]);
			if (ret) {
				dev_err(&pdev->dev, "Request_irq failed:%d: err:%d\n",
					timer->etu_slice[slice].extts_irq, ret);
				return ret;
			}
		} else
			timer->etu_slice[slice].extts_irq = summary_irq;

		pinctrl = devm_pinctrl_get_select_default(&pdev->dev);
		if (IS_ERR(pinctrl))
			dev_info(&pdev->dev, "No default pinctrl found\n");

		pr_debug("sel: %d, index: %d, slice-num:%d slice-name: %s, IRQ: %d\n", sel,
				timer->etu_slice[slice].extts_index, slice,
				timer->etu_slice[slice].name, timer->etu_slice[slice].extts_irq);
	}

	timer->total_etu_cnt = MAX_ETU_SLICE;
	timer->ptp_clock_info.n_ext_ts = MAX_ETU_SLICE;

	return 0;
}

static int qcom_tsc_self_test(struct qcom_ptp_tsc *timer)
{
	u32 regval;

	/* Disable the TSC counter. */
	regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	regval &= ~BIT(0);
	tsc_write_readback(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR, regval, BIT(0));

	/* Initiate the counter with known value.(0x11111111) */
	writel_relaxed(0x11111111, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO);
	writel_relaxed(0x11111111, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_HI);

	/* Update the INCR value as 0x1. */
	regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	regval &= ~(GENMASK(11, 8));
	regval |= (0x1 << 8);
	tsc_write_readback(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR, regval, GENMASK(11, 8));

	/* Write the STEP_INCR once. */
	regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	regval |= BIT(12);
	writel_relaxed(regval, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);

	/* Read counter and compare the counter value. */
	if (readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO) -
			0x11111111 != TSC_RESOLUTION) {
		pr_err("TSC self test failed counter diff = 0x%x\n",
			readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO) -
			0x11111111);
		return -EINVAL;
	}

	/* Clear the STEP_INCR bit. */
	regval = readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	regval &= ~BIT(12);
	writel_relaxed(regval, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);
	readl_relaxed(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR);

	/* Reset the counter to 0. */
	writel_relaxed(0x0, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_LO);
	writel_relaxed(0x0, timer->baseaddr + TSCSS_TSC_CONTROL_CNTCV_HI);

	return 0;
}

static int tsc_mux_src_ctrl(struct platform_device *pdev, struct qcom_ptp_tsc *timer)
{
	struct resource *r_mem;
	int ret = 0;

	r_mem = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tsc_src_sel");
	if (!r_mem) {
		dev_warn(&pdev->dev, "no tsc mux ctrl resource defined\n");
		return -ENXIO;
	}

	timer->mux_addr = devm_ioremap_resource(&pdev->dev, r_mem);
	if (IS_ERR(timer->mux_addr))
		return PTR_ERR(timer->mux_addr);

	ret = device_create_file(&pdev->dev, &dev_attr_tsc_src_ctrl);
	if (ret) {
		dev_err(&pdev->dev, "Failed to create mux_ctrl sysfs attribute: %d\n", ret);
		return ret;
	}

	return 0;
}

static int qcom_ptp_tsc_probe(struct platform_device *pdev)
{
	struct qcom_ptp_tsc *timer;
	struct resource *r_mem;
	u32 cntr_val, pipeline_stages;
	int fusa_irq, ret;

	timer = devm_kzalloc(&pdev->dev, sizeof(*timer), GFP_KERNEL);
	if (!timer)
		return -ENOMEM;

	timer->dev = &pdev->dev;

	r_mem = platform_get_resource_byname(pdev, IORESOURCE_MEM, "tsc");
	if (!r_mem) {
		dev_err(&pdev->dev, "no IO resource defined\n");
		return -ENXIO;
	}

	timer->baseaddr = devm_ioremap_resource(&pdev->dev, r_mem);
	if (IS_ERR(timer->baseaddr))
		return PTR_ERR(timer->baseaddr);

	r_mem = platform_get_resource_byname(pdev, IORESOURCE_MEM, "timer");
	if (!r_mem) {
		dev_err(&pdev->dev, "no timer resource defined\n");
		return -ENXIO;
	}

	timer->timer_baseaddr = devm_ioremap_resource(&pdev->dev, r_mem);
	if (IS_ERR(timer->baseaddr))
		return PTR_ERR(timer->baseaddr);

	spin_lock_init(&timer->reg_lock);

	timer->tsc_cfg_ahb_clk = devm_clk_get(&pdev->dev, "cfg_ahb");
	if (IS_ERR(timer->tsc_cfg_ahb_clk)) {
		if (PTR_ERR(timer->tsc_cfg_ahb_clk) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Unable to get CFG AHB clock\n");
		return PTR_ERR(timer->tsc_cfg_ahb_clk);
	}

	timer->tsc_cntr_clk = devm_clk_get(&pdev->dev, "cntr");
	if (IS_ERR(timer->tsc_cntr_clk)) {
		if (PTR_ERR(timer->tsc_cntr_clk) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Unable to get Counter clock\n");
		return PTR_ERR(timer->tsc_cntr_clk);
	}

	timer->tsc_etu_clk = devm_clk_get(&pdev->dev, "etu");
	if (IS_ERR(timer->tsc_etu_clk)) {
		if (PTR_ERR(timer->tsc_etu_clk) != -EPROBE_DEFER)
			dev_err(&pdev->dev, "Unable to get ETU clock\n");
		return PTR_ERR(timer->tsc_etu_clk);
	}

	ret = clk_prepare_enable(timer->tsc_cfg_ahb_clk);
	if (ret) {
		pr_debug("Failed to enable AHB clock\n");
		return ret;
	}

	ret = clk_prepare_enable(timer->tsc_cntr_clk);
	if (ret) {
		pr_debug("Failed to enable counter clock\n");
		return ret;
	}

	ret = clk_prepare_enable(timer->tsc_etu_clk);
	if (ret) {
		pr_debug("Failed to enable etu clock\n");
		return ret;
	}

	timer->tsc_nsec_update = of_property_read_bool(pdev->dev.of_node,
							"qcom,tsc-nsec-update");

	timer->tsc_hw_preload = of_property_read_bool(pdev->dev.of_node,
							"qcom,tsc-hw-preload");

	timer->frame_pulse_gen = of_property_read_bool(pdev->dev.of_node,
							"qcom,tsc-frame-pulse-gen");

	timer->tsc_safety = of_property_read_bool(pdev->dev.of_node,
							"qcom,tsc-safety");

	if (timer->tsc_hw_preload)
		INIT_DEFERRABLE_WORK(&timer->tsc_preload_poll_work, tsc_preload_poll);

	timer->ptp_clock_info = qcom_ptp_clock_info;

	if (timer->tsc_safety) {
		ret = qcom_tsc_self_test(timer);
		if (ret) {
			pr_debug("Failed in self test\n");
			return ret;
		}
	}

	timer->ptp_clock = ptp_clock_register(&timer->ptp_clock_info, &pdev->dev);
	if (IS_ERR(timer->ptp_clock)) {
		ret = PTR_ERR(timer->ptp_clock);
		dev_err(&pdev->dev, "Failed to register ptp clock\n");
		goto out;
	}

	if (!of_property_read_u32(pdev->dev.of_node, "qcom,tsc-pipeline-stages", &pipeline_stages))
		writel_relaxed(pipeline_stages, timer->baseaddr + TSCSS_TSC_PIPELINE_STAGES);

	qcom_tsc_etu_get_data(pdev, timer);

	ret = tsc_mux_src_ctrl(pdev, timer);
	if (ret)
		dev_warn(&pdev->dev, "mux control not enabled (%d)\n", ret);


	if (!timer->tsc_nsec_update) {
		cntr_val = (timer->tsc_hw_preload ? 0x1D8 : 0x1CC);
		writel_relaxed(0x3B9AC9FF, timer->baseaddr + TSCSS_TSC_ROLLOVER_VAL);
	} else {
		cntr_val = 0x1CC;
		fusa_irq = platform_get_irq_byname(pdev, "tscss_fusa_irq");
		if (fusa_irq < 0) {
			dev_warn(&pdev->dev, "FUSA IRQ not available: %d\n", fusa_irq);
		} else {
			ret = devm_request_irq(timer->dev, fusa_irq, qcom_fusa_irq_handler,
						IRQF_TRIGGER_RISING, "tscss_fusa_irq",
						timer);
			if (ret)
				pr_debug("Failed to request fusa IRQ\n");
			else
				pr_debug("Fusa IRQ registered ret%d\n", ret);
		}
	}

	tsc_write_readback(timer->baseaddr + TSCSS_TSC_CONTROL_CNTCR, cntr_val, GENMASK(11, 0));

	pr_info("TSC CNTR 0x%x tsc-nsec-update %d tsc-hw-preload %d\n",
		readl_relaxed(timer->baseaddr), timer->tsc_nsec_update, timer->tsc_hw_preload);

	platform_set_drvdata(pdev, timer);

	return 0;
out:
	timer->ptp_clock = NULL;
	return ret;
}

static const struct of_device_id tsc_of_match[] = {
	{ .compatible = "qcom,tsc", },
	{ /* end of table */ }
};
MODULE_DEVICE_TABLE(of, timer_tsc_of_match);

static struct platform_driver qcom_ptp_tsc_driver = {
	.probe  = qcom_ptp_tsc_probe,
	.remove = qcom_ptp_tsc_remove,
	.driver = {
		.name = "qcom_ptp_tsc",
		.of_match_table = tsc_of_match,
	},
};

module_platform_driver(qcom_ptp_tsc_driver);

MODULE_DESCRIPTION("PTP QCOM TSC driver");
MODULE_LICENSE("GPL");
