// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE PTP Core Implementation
 *
 * Implements PTP (IEEE 1588) clock registration, PHC operations, TX/RX
 * timestamp processing, and periodic PHC time caching for the E810 NIC.
 *
 * Design choices informed by studying the ice and mlx5 PTP implementations:
 *
 *   - **Separate HW layer**: All register access is delegated to
 *     fdice_ptp_hw.c, keeping this file hardware-agnostic.
 *
 *   - **Explicit state machine**: State transitions are validated to
 *     prevent double-init or use-after-cleanup.
 *
 *   - **Configurable TX timeout**: The 2-second TX timestamp timeout is
 *     stored in a per-device field rather than hard-coded, enabling
 *     future ethtool/debugfs tuning.
 *
 *   - **Structured statistics**: All PTP counters live in fdice_ptp_stats
 *     for clean exposure through devlink health or ethtool.
 *
 *   - **RX timestamp extension**: The 40-bit to 64-bit extension uses a
 *     periodically cached PHC time, with wrap-around handling.
 *
 * Architecture reference: docs/exploration/13-ptp-clock-synchronization.md
 */

#include <linux/module.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/kthread.h>
#include <linux/math64.h>
#include <linux/net_tstamp.h>
#include <linux/delay.h>
#include <linux/bitops.h>

#include "fdice_dev.h"
#include "fdice_ptp.h"
#include "fdice_ptp_hw.h"

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

/** Interval between periodic PHC cache updates (500 ms) */
#define FDICE_PTP_PHC_WORK_INTERVAL_MS	500

/**
 * Mask for the 40-bit timestamp value extracted from RX descriptors.
 * Used when extending partial RX timestamps to full 64-bit values.
 */
#define FDICE_PTP_TS_40BIT_MASK		GENMASK_ULL(39, 0)

/**
 * Half of the 40-bit timestamp range.  When the difference between
 * the cached PHC time and a raw 40-bit RX timestamp exceeds this
 * threshold, the extension algorithm adds or subtracts a full 40-bit
 * wrap-around to handle the ambiguity correctly.
 */
#define FDICE_PTP_TS_40BIT_HALF		BIT_ULL(39)

/**
 * Scaled PPM divisor.  The PTP subsystem passes frequency adjustments
 * as "scaled ppm" with 16 fractional bits:  1 ppm = 1 << 16.
 * The new increment value is: base_incval * (1 + scaled_ppm / 2^48).
 * We compute: new_incval = base_incval + (base_incval * scaled_ppm) >> 48.
 */
#define FDICE_PTP_SCALED_PPM_SHIFT	48

/* -----------------------------------------------------------------------
 * TX Timestamp Bitmap Management
 *
 * The bitmap tracks which TX timestamp indexes are in use.  The TX path
 * allocates an index, the completion path (interrupt or periodic poll)
 * reads the timestamp and frees the index.
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_tx_alloc_idx - Allocate a TX timestamp index
 * @port: per-port PTP state
 *
 * Finds the first free index in the TX timestamp bitmap and marks it
 * as in-use.  Must be called with port->tx_lock held.
 *
 * Return: The allocated index (0..num_entries-1), or -1 if all slots
 *         are occupied.
 */
static s8 fdice_ptp_tx_alloc_idx(struct fdice_ptp_port *port)
{
	unsigned long idx;

	idx = find_first_zero_bit(port->in_use, port->num_entries);
	if (idx >= port->num_entries)
		return -1;

	set_bit(idx, port->in_use);
	return (s8)idx;
}

/**
 * fdice_ptp_tx_free_idx - Release a TX timestamp index
 * @port: per-port PTP state
 * @idx: index to release
 *
 * Clears the in-use bit and NULLs the SKB pointer.  Must be called
 * with port->tx_lock held.
 */
static void fdice_ptp_tx_free_idx(struct fdice_ptp_port *port, u8 idx)
{
	if (idx < port->num_entries) {
		port->entries[idx].skb = NULL;
		clear_bit(idx, port->in_use);
	}
}

/* -----------------------------------------------------------------------
 * TX Timestamp Request
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_tx_request - Request a TX timestamp for a packet
 * @port: per-port PTP state
 * @skb: socket buffer that needs timestamping
 *
 * Allocates a TX timestamp index, stores the SKB reference, and records
 * the submission time for timeout detection.  The caller writes the
 * returned index into the TX descriptor so the hardware knows where to
 * store the captured timestamp.
 *
 * Return: The allocated index (0..63), or -1 if no slots are available.
 */
s8 fdice_ptp_tx_request(struct fdice_ptp_port *port, struct sk_buff *skb)
{
	unsigned long flags;
	s8 idx;

	spin_lock_irqsave(&port->tx_lock, flags);
	idx = fdice_ptp_tx_alloc_idx(port);
	if (idx >= 0) {
		port->entries[idx].skb = skb;
		port->entries[idx].start = jiffies;
		port->entries[idx].cached_tstamp = 0;
	}
	spin_unlock_irqrestore(&port->tx_lock, flags);

	return idx;
}

/* -----------------------------------------------------------------------
 * TX Timestamp Completion Processing
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_tx_complete_one - Complete a single TX timestamp
 * @fdev: device instance
 * @port: per-port PTP state
 * @idx: timestamp index to process
 *
 * Reads the raw timestamp from the PHY, converts it to a
 * skb_shared_hwtstamps, and delivers it to the networking stack via
 * skb_tstamp_tx().  Frees the bitmap slot on success or timeout.
 *
 * Must be called with port->tx_lock held.
 */
static void fdice_ptp_tx_complete_one(struct fdice_dev *fdev,
				      struct fdice_ptp_port *port, u8 idx)
{
	struct fdice_ptp *ptp = &fdev->ptp;
	struct skb_shared_hwtstamps shhwtstamps;
	struct sk_buff *skb;
	u64 raw_tstamp;
	int err;

	skb = port->entries[idx].skb;
	if (!skb)
		return;

	/* Check for timeout */
	if (time_is_before_jiffies(port->entries[idx].start +
				   ptp->tx_timeout_jiffies)) {
		dev_dbg(&fdev->pdev->dev,
			"PTP TX timestamp timeout on port %u idx %u\n",
			port->port_num, idx);
		ptp->stats.tx_tstamp_timeouts++;
		dev_kfree_skb_any(skb);
		fdice_ptp_tx_free_idx(port, idx);
		return;
	}

	err = fdice_ptp_hw_read_tx_tstamp(&fdev->hw, port->port_num,
					  idx, &raw_tstamp);
	if (err)
		return;

	/*
	 * If the timestamp hasn't changed from the cached value, the
	 * hardware hasn't captured a new timestamp yet -- skip this index
	 * and try again on the next poll.
	 */
	if (raw_tstamp == port->entries[idx].cached_tstamp)
		return;

	port->entries[idx].cached_tstamp = raw_tstamp;

	memset(&shhwtstamps, 0, sizeof(shhwtstamps));
	shhwtstamps.hwtstamp = ns_to_ktime(raw_tstamp);

	skb_tstamp_tx(skb, &shhwtstamps);
	dev_kfree_skb_any(skb);
	fdice_ptp_tx_free_idx(port, idx);
	ptp->stats.tx_tstamp_good++;
}

/**
 * fdice_ptp_tx_tstamp - Process all pending TX timestamps
 * @fdev: device instance
 *
 * Iterates the in-use bitmap for the port, attempting to complete each
 * outstanding TX timestamp request.  Called from the PTP timestamp
 * interrupt handler or the periodic worker.
 */
void fdice_ptp_tx_tstamp(struct fdice_dev *fdev)
{
	struct fdice_ptp_port *port = &fdev->ptp.port;
	unsigned long flags;
	unsigned long idx;

	spin_lock_irqsave(&port->tx_lock, flags);
	for_each_set_bit(idx, port->in_use, port->num_entries)
		fdice_ptp_tx_complete_one(fdev, port, (u8)idx);
	spin_unlock_irqrestore(&port->tx_lock, flags);
}

/* -----------------------------------------------------------------------
 * RX Timestamp Extension
 *
 * RX descriptors carry only the lower 40 bits of the PHC timestamp.
 * To reconstruct the full 64-bit value, we combine the 40-bit partial
 * stamp with the upper bits of a periodically cached PHC time.  The
 * wrap-around logic ensures correctness when the 40-bit counter rolls
 * over between the cache update and the RX timestamp capture.
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_rx_tstamp - Extend a 40-bit RX timestamp to 64-bit
 * @ptp: PTP subsystem state
 * @raw_tstamp_40bit: raw 40-bit timestamp from the RX descriptor
 *
 * Uses the cached PHC time to reconstruct the full 64-bit timestamp.
 * Handles the case where the 40-bit value has wrapped relative to the
 * cached snapshot.
 *
 * Return: Full 64-bit nanosecond timestamp.
 */
u64 fdice_ptp_rx_tstamp(struct fdice_ptp *ptp, u64 raw_tstamp_40bit)
{
	u64 cached = ptp->cached_phc_time;
	u64 result;

	raw_tstamp_40bit &= FDICE_PTP_TS_40BIT_MASK;

	/*
	 * Replace the lower 40 bits of the cached PHC time with the raw
	 * RX timestamp, then adjust for wrap-around.
	 */
	result = (cached & ~FDICE_PTP_TS_40BIT_MASK) | raw_tstamp_40bit;

	/*
	 * If the raw timestamp is significantly larger than the lower 40
	 * bits of the cached time, the raw value was captured before the
	 * cached PHC's upper bits incremented -- subtract one wrap.
	 * Conversely, if the raw value is significantly smaller, the
	 * upper bits need to be incremented by one wrap.
	 */
	if (raw_tstamp_40bit > (cached & FDICE_PTP_TS_40BIT_MASK)) {
		if ((raw_tstamp_40bit - (cached & FDICE_PTP_TS_40BIT_MASK)) >
		    FDICE_PTP_TS_40BIT_HALF)
			result -= BIT_ULL(40);
	} else {
		if (((cached & FDICE_PTP_TS_40BIT_MASK) - raw_tstamp_40bit) >
		    FDICE_PTP_TS_40BIT_HALF)
			result += BIT_ULL(40);
	}

	ptp->stats.rx_tstamp_extended++;

	return result;
}

/* -----------------------------------------------------------------------
 * Periodic PHC Time Caching
 *
 * A dedicated kthread worker periodically reads the PHC and stores it
 * in ptp->cached_phc_time.  This avoids an expensive hardware read in
 * the per-packet RX path while keeping the cached value fresh enough
 * for reliable 40-bit to 64-bit extension.
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_cache_phc_time - Periodic PHC cache update worker
 * @work: kthread_work embedded in fdice_ptp.phc_work
 *
 * Reads the current PHC time (under the hardware semaphore) and stores
 * it along with the current jiffies value.  Reschedules itself for
 * the next update interval.
 */
static void fdice_ptp_cache_phc_time(struct kthread_work *work)
{
	struct kthread_delayed_work *dwork =
		container_of(work, struct kthread_delayed_work, work);
	struct fdice_ptp *ptp =
		container_of(dwork, struct fdice_ptp, phc_work);
	struct fdice_dev *fdev =
		container_of(ptp, struct fdice_dev, ptp);
	struct fdice_hw *hw = &fdev->hw;
	unsigned long prev_jiffies;
	u64 phc_time;
	int err;

	if (ptp->state != FDICE_PTP_READY)
		return;

	prev_jiffies = ptp->cached_phc_jiffies;

	if (!fdice_ptp_hw_sem_acquire(hw)) {
		dev_dbg(&fdev->pdev->dev,
			"PTP failed to acquire semaphore for PHC cache\n");
		goto resched;
	}

	err = fdice_ptp_hw_read_time(hw, &phc_time, NULL);
	fdice_ptp_hw_sem_release(hw);

	if (!err) {
		WRITE_ONCE(ptp->cached_phc_time, phc_time);
		WRITE_ONCE(ptp->cached_phc_jiffies, jiffies);
	}

	/*
	 * Track late updates: if more than 2x the expected interval has
	 * elapsed since the last successful cache, RX timestamp accuracy
	 * may be degraded.
	 */
	if (prev_jiffies &&
	    time_is_before_jiffies(prev_jiffies +
				   msecs_to_jiffies(FDICE_PTP_PHC_WORK_INTERVAL_MS * 2)))
		ptp->stats.late_phc_updates++;

resched:
	kthread_queue_delayed_work(ptp->kworker, &ptp->phc_work,
				   msecs_to_jiffies(FDICE_PTP_PHC_WORK_INTERVAL_MS));
}

/* -----------------------------------------------------------------------
 * PHC Clock Operations (ptp_clock_info callbacks)
 *
 * These functions are registered with the PTP subsystem and called by
 * userspace tools (phc2sys, ptp4l) via the PTP clock device.
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_adjfine - Adjust the PHC frequency
 * @info: PTP clock info (registered ops structure)
 * @scaled_ppm: frequency adjustment in scaled parts-per-million
 *              (1 ppm = 1 << 16 in this encoding)
 *
 * Computes a new timer increment value based on the requested frequency
 * offset and programs it into the hardware.  The formula is:
 *
 *   new_incval = base_incval + (base_incval * scaled_ppm) >> 48
 *
 * This provides sub-ppb granularity sufficient for PTP synchronization.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_adjfine(struct ptp_clock_info *info, long scaled_ppm)
{
	struct fdice_ptp *ptp = fdice_ptp_from_info(info);
	struct fdice_dev *fdev = container_of(ptp, struct fdice_dev, ptp);
	struct fdice_hw *hw = &fdev->hw;
	u64 base_incval = ptp->hw_caps.nominal_incval;
	u64 adj;
	int err;
	bool neg = false;

	if (scaled_ppm < 0) {
		neg = true;
		scaled_ppm = -scaled_ppm;
	}

	/* adj = base_incval * abs(scaled_ppm) >> 48 */
	adj = mul_u64_u64_shr(base_incval, (u64)scaled_ppm,
			      FDICE_PTP_SCALED_PPM_SHIFT);

	if (neg)
		base_incval -= adj;
	else
		base_incval += adj;

	if (!fdice_ptp_hw_sem_acquire(hw))
		return -EBUSY;

	err = fdice_ptp_hw_write_incval(hw, base_incval);
	fdice_ptp_hw_sem_release(hw);

	return err;
}

/**
 * fdice_ptp_adjtime - Adjust the PHC by a time offset
 * @info: PTP clock info
 * @delta: signed nanosecond offset to apply
 *
 * For small adjustments (within s32 range), uses the hardware ADJ_TIME
 * command for atomic adjustment.  For larger deltas, falls back to a
 * read-modify-write sequence (non-atomic but correct).
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_adjtime(struct ptp_clock_info *info, s64 delta)
{
	struct fdice_ptp *ptp = fdice_ptp_from_info(info);
	struct fdice_dev *fdev = container_of(ptp, struct fdice_dev, ptp);
	struct fdice_hw *hw = &fdev->hw;
	int err;

	/*
	 * For large adjustments that exceed the s32 range of the hardware
	 * ADJ_TIME command, fall back to read + add + write.
	 */
	if (delta > S32_MAX || delta < S32_MIN) {
		u64 phc_time;

		if (!fdice_ptp_hw_sem_acquire(hw))
			return -EBUSY;

		err = fdice_ptp_hw_read_time(hw, &phc_time, NULL);
		if (err) {
			fdice_ptp_hw_sem_release(hw);
			return err;
		}

		phc_time += delta;
		err = fdice_ptp_hw_write_time(hw, phc_time);
		fdice_ptp_hw_sem_release(hw);

		if (!err) {
			WRITE_ONCE(ptp->cached_phc_time, phc_time);
			WRITE_ONCE(ptp->cached_phc_jiffies, jiffies);
		}

		return err;
	}

	if (!fdice_ptp_hw_sem_acquire(hw))
		return -EBUSY;

	err = fdice_ptp_hw_adj_time(hw, (s32)delta);
	fdice_ptp_hw_sem_release(hw);

	if (!err) {
		/* Update the cached PHC time to reflect the adjustment */
		WRITE_ONCE(ptp->cached_phc_time,
			   ptp->cached_phc_time + delta);
		WRITE_ONCE(ptp->cached_phc_jiffies, jiffies);
	}

	return err;
}

/**
 * fdice_ptp_gettimex64 - Read the PHC time with system timestamp
 * @info: PTP clock info
 * @ts: output timespec64 for the PHC time
 * @sts: optional system timestamp pair for PTP_SYS_OFFSET_EXTENDED
 *
 * Reads the current PHC time under the hardware semaphore and converts
 * it to timespec64 format.  If @sts is provided, captures system
 * timestamps around the hardware read for cross-timestamp support.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_gettimex64(struct ptp_clock_info *info,
			 struct timespec64 *ts,
			 struct ptp_system_timestamp *sts)
{
	struct fdice_ptp *ptp = fdice_ptp_from_info(info);
	struct fdice_dev *fdev = container_of(ptp, struct fdice_dev, ptp);
	struct fdice_hw *hw = &fdev->hw;
	u64 phc_time;
	int err;

	if (!fdice_ptp_hw_sem_acquire(hw))
		return -EBUSY;

	err = fdice_ptp_hw_read_time(hw, &phc_time, sts);
	fdice_ptp_hw_sem_release(hw);

	if (!err)
		*ts = ns_to_timespec64(phc_time);

	return err;
}

/**
 * fdice_ptp_settime64 - Set the PHC to an absolute time
 * @info: PTP clock info
 * @ts: timespec64 with the desired time
 *
 * Converts the timespec64 to nanoseconds and writes it to the hardware
 * timer under the PTP semaphore.  Also resets the cached PHC time.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_settime64(struct ptp_clock_info *info,
			const struct timespec64 *ts)
{
	struct fdice_ptp *ptp = fdice_ptp_from_info(info);
	struct fdice_dev *fdev = container_of(ptp, struct fdice_dev, ptp);
	struct fdice_hw *hw = &fdev->hw;
	u64 time_ns;
	int err;

	time_ns = timespec64_to_ns(ts);

	if (!fdice_ptp_hw_sem_acquire(hw))
		return -EBUSY;

	err = fdice_ptp_hw_write_time(hw, time_ns);
	fdice_ptp_hw_sem_release(hw);

	if (!err) {
		WRITE_ONCE(ptp->cached_phc_time, time_ns);
		WRITE_ONCE(ptp->cached_phc_jiffies, jiffies);
	}

	return err;
}

/**
 * fdice_ptp_enable - Enable/disable PTP clock features (GPIO/PPS)
 * @info: PTP clock info
 * @rq: the requested clock feature configuration
 * @on: true to enable, false to disable
 *
 * Handles PTP_CLK_REQ_EXTTS and PTP_CLK_REQ_PEROUT requests for
 * external timestamp capture and periodic output signal generation.
 * PTP_CLK_REQ_PPS is not directly supported.
 *
 * Return: 0 on success, -EOPNOTSUPP for unsupported request types.
 */
int fdice_ptp_enable(struct ptp_clock_info *info,
		     struct ptp_clock_request *rq, int on)
{
	/* GPIO/PPS support is deferred to a later phase */
	return -EOPNOTSUPP;
}

/* -----------------------------------------------------------------------
 * PTP Clock Registration
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_setup_clock_info - Populate the ptp_clock_info structure
 * @ptp: PTP subsystem state
 * @fdev: device instance (for naming)
 *
 * Fills in the ptp_clock_info ops and capability fields based on the
 * detected hardware capabilities.  Called before ptp_clock_register().
 */
static void fdice_ptp_setup_clock_info(struct fdice_ptp *ptp,
				       struct fdice_dev *fdev)
{
	struct ptp_clock_info *info = &ptp->info;

	snprintf(info->name, sizeof(info->name), "fdice-%s",
		 pci_name(fdev->pdev));
	info->owner = THIS_MODULE;
	info->max_adj = ptp->hw_caps.max_adj_ppb;

	/* PHC operations */
	info->adjfine = fdice_ptp_adjfine;
	info->adjtime = fdice_ptp_adjtime;
	info->gettimex64 = fdice_ptp_gettimex64;
	info->settime64 = fdice_ptp_settime64;
	info->enable = fdice_ptp_enable;

	/* GPIO capabilities (populated when GPIO support is added) */
	info->n_per_out = FDICE_PTP_TGT_MAX_CHANNELS;
	info->n_ext_ts = FDICE_PTP_EVNT_MAX_CHANNELS;
}

/* -----------------------------------------------------------------------
 * PTP Initialization and Cleanup
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_init_port - Initialize per-port TX timestamp tracking
 * @port: per-port PTP state to initialize
 * @hw_caps: hardware capability descriptor
 * @port_num: physical port number
 *
 * Allocates the TX timestamp entry array and initializes the bitmap
 * and spinlock.
 *
 * Return: 0 on success, -ENOMEM on allocation failure.
 */
static int fdice_ptp_init_port(struct fdice_ptp_port *port,
			       const struct fdice_ptp_hw_caps *hw_caps,
			       u8 port_num)
{
	port->num_entries = hw_caps->ts_block_per_port;
	port->port_num = port_num;
	port->link_up = false;

	spin_lock_init(&port->tx_lock);
	bitmap_zero(port->in_use, FDICE_PTP_TX_TSTAMP_MAX);

	port->entries = kcalloc(port->num_entries,
				sizeof(*port->entries), GFP_KERNEL);
	if (!port->entries)
		return -ENOMEM;

	return 0;
}

/**
 * fdice_ptp_cleanup_port - Free per-port TX timestamp resources
 * @port: per-port PTP state to clean up
 *
 * Frees any SKBs still held in the TX timestamp tracking array, then
 * frees the entry array itself.
 */
static void fdice_ptp_cleanup_port(struct fdice_ptp_port *port)
{
	unsigned long idx;

	if (!port->entries)
		return;

	for_each_set_bit(idx, port->in_use, port->num_entries) {
		if (port->entries[idx].skb) {
			dev_kfree_skb_any(port->entries[idx].skb);
			port->entries[idx].skb = NULL;
		}
	}
	bitmap_zero(port->in_use, FDICE_PTP_TX_TSTAMP_MAX);

	kfree(port->entries);
	port->entries = NULL;
}

/**
 * fdice_ptp_init - Initialize the PTP subsystem
 * @fdev: device instance
 *
 * Full PTP initialization sequence:
 *   1. Detect hardware capabilities (E810-specific)
 *   2. Determine if this PF owns the source timer
 *   3. If owner: acquire semaphore, program increment value and
 *      initial time, release semaphore, register PTP clock
 *   4. Initialize per-port TX timestamp tracking
 *   5. Start the periodic PHC cache worker
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_init(struct fdice_dev *fdev)
{
	struct fdice_ptp *ptp = &fdev->ptp;
	struct fdice_hw *hw = &fdev->hw;
	struct kthread_worker *kworker;
	struct timespec64 ts;
	u64 init_time_ns;
	int err;

	if (ptp->state != FDICE_PTP_UNINIT)
		return -EBUSY;

	if (!fdev->hw.func_caps.ieee_1588)
		return -ENODEV;

	ptp->state = FDICE_PTP_INITIALIZING;
	ptp->tx_timeout_jiffies = FDICE_PTP_TX_TIMEOUT;
	memset(&ptp->stats, 0, sizeof(ptp->stats));

	/* Step 1: Detect E810 hardware capabilities */
	err = fdice_ptp_hw_init_e810(hw, &ptp->hw_caps);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"PTP hardware capability detection failed: %d\n", err);
		goto err_uninit;
	}

	/*
	 * Step 2: Determine source timer ownership.
	 *
	 * Only one PF per device owns the source timer and is responsible
	 * for programming it.  In E810, this is determined by the PF ID
	 * and primary function status.  For simplicity, PF 0 is the owner.
	 */
	ptp->src_tmr_owner = (hw->pf_id == 0);

	/* Step 3: If this PF owns the timer, initialize it */
	if (ptp->src_tmr_owner) {
		if (!fdice_ptp_hw_sem_acquire(hw)) {
			dev_err(&fdev->pdev->dev,
				"PTP failed to acquire semaphore for init\n");
			err = -EBUSY;
			goto err_uninit;
		}

		/* Program the nominal increment value */
		err = fdice_ptp_hw_write_incval(hw,
						ptp->hw_caps.nominal_incval);
		if (err) {
			fdice_ptp_hw_sem_release(hw);
			dev_err(&fdev->pdev->dev,
				"PTP failed to write increment value: %d\n",
				err);
			goto err_uninit;
		}

		/* Set initial time to current wall clock */
		ts = ktime_to_timespec64(ktime_get_real());
		init_time_ns = timespec64_to_ns(&ts);
		err = fdice_ptp_hw_write_time(hw, init_time_ns);
		fdice_ptp_hw_sem_release(hw);

		if (err) {
			dev_err(&fdev->pdev->dev,
				"PTP failed to write initial time: %d\n", err);
			goto err_uninit;
		}

		/* Register the PTP clock device */
		fdice_ptp_setup_clock_info(ptp, fdev);
		ptp->clock = ptp_clock_register(&ptp->info, &fdev->pdev->dev);
		if (IS_ERR(ptp->clock)) {
			err = PTR_ERR(ptp->clock);
			ptp->clock = NULL;
			dev_err(&fdev->pdev->dev,
				"PTP clock registration failed: %d\n", err);
			goto err_uninit;
		}
	}

	/* Step 4: Initialize per-port TX timestamp tracking */
	err = fdice_ptp_init_port(&ptp->port, &ptp->hw_caps,
				  hw->port_info ? hw->port_info->lport : 0);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"PTP port init failed: %d\n", err);
		goto err_clock;
	}

	/* Step 5: Seed the cached PHC time and start the periodic worker */
	ptp->cached_phc_time = 0;
	ptp->cached_phc_jiffies = 0;

	kthread_init_delayed_work(&ptp->phc_work, fdice_ptp_cache_phc_time);

	kworker = kthread_create_worker(0, "fdice_ptp/%s",
					pci_name(fdev->pdev));
	if (IS_ERR(kworker)) {
		err = PTR_ERR(kworker);
		dev_err(&fdev->pdev->dev,
			"PTP worker creation failed: %d\n", err);
		goto err_port;
	}
	ptp->kworker = kworker;

	/* Kick off the first PHC cache update */
	kthread_queue_delayed_work(ptp->kworker, &ptp->phc_work, 0);

	ptp->state = FDICE_PTP_READY;
	dev_info(&fdev->pdev->dev, "PTP initialized successfully%s\n",
		 ptp->src_tmr_owner ? " (source timer owner)" : "");

	return 0;

err_port:
	fdice_ptp_cleanup_port(&ptp->port);
err_clock:
	if (ptp->clock) {
		ptp_clock_unregister(ptp->clock);
		ptp->clock = NULL;
	}
err_uninit:
	ptp->state = FDICE_PTP_UNINIT;
	return err;
}

/**
 * fdice_ptp_cleanup - Tear down the PTP subsystem
 * @fdev: device instance
 *
 * Stops the periodic worker, unregisters the PTP clock, and frees all
 * per-port resources.  Safe to call even if init failed partially
 * (guards against UNINIT state).
 */
void fdice_ptp_cleanup(struct fdice_dev *fdev)
{
	struct fdice_ptp *ptp = &fdev->ptp;

	if (ptp->state == FDICE_PTP_UNINIT)
		return;

	ptp->state = FDICE_PTP_UNINIT;

	/* Stop the periodic PHC cache worker */
	if (ptp->kworker) {
		kthread_cancel_delayed_work_sync(&ptp->phc_work);
		kthread_destroy_worker(ptp->kworker);
		ptp->kworker = NULL;
	}

	/* Unregister the PTP clock device */
	if (ptp->clock) {
		ptp_clock_unregister(ptp->clock);
		ptp->clock = NULL;
	}

	/* Free per-port TX timestamp tracking */
	fdice_ptp_cleanup_port(&ptp->port);

	dev_info(&fdev->pdev->dev, "PTP cleaned up\n");
}
