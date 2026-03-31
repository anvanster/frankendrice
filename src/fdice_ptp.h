// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_PTP_H_
#define _FDICE_PTP_H_

/**
 * DOC: FrankenDrICE PTP Subsystem Header
 *
 * Defines the PTP state machine, per-port TX timestamp tracking, and the
 * top-level PTP subsystem structure that is embedded in fdice_dev.  The
 * design separates concerns: this header defines the software-level PTP
 * abstractions, while fdice_ptp_hw.h handles register-level constants.
 *
 * Key improvements over the reference design:
 *   - Explicit state transition validation
 *   - Configurable TX timestamp timeout (not hard-coded 2 seconds)
 *   - Structured statistics for observability
 *   - Cleaner lock hierarchy documentation
 *
 * Architecture reference: docs/exploration/13-ptp-clock-synchronization.md
 */

#include <linux/bitops.h>
#include <linux/kthread.h>
#include <linux/net_tstamp.h>
#include <linux/ptp_clock_kernel.h>
#include <linux/spinlock.h>
#include <linux/types.h>

#include "fdice_ptp_hw.h"

/* Forward declarations */
struct fdice_dev;
struct fdice_hw;
struct sk_buff;

/* -----------------------------------------------------------------------
 * PTP State Machine
 *
 * Transitions:
 *   UNINIT -> INITIALIZING -> READY     (normal init)
 *   READY  -> RESETTING    -> READY     (reset recovery)
 *   Any    -> ERROR                     (unrecoverable failure)
 *   Any    -> UNINIT                    (cleanup / init failure)
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_ptp_state - PTP subsystem lifecycle states
 * @FDICE_PTP_UNINIT: Not initialized or cleaned up after failure
 * @FDICE_PTP_INITIALIZING: Initialization in progress
 * @FDICE_PTP_READY: Fully operational, timestamps available
 * @FDICE_PTP_RESETTING: Reset recovery in progress
 * @FDICE_PTP_ERROR: Unrecoverable error, requires full re-init
 */
enum fdice_ptp_state {
	FDICE_PTP_UNINIT = 0,
	FDICE_PTP_INITIALIZING,
	FDICE_PTP_READY,
	FDICE_PTP_RESETTING,
	FDICE_PTP_ERROR,
};

/* -----------------------------------------------------------------------
 * TX Timestamp Tracking
 *
 * Each port maintains a bitmap of outstanding TX timestamp requests.
 * When the TX path needs a timestamp, it allocates an index from the
 * bitmap, stores the SKB pointer, and programs the descriptor with the
 * index.  On completion (interrupt or polling), the hardware timestamp
 * is read from the PHY register at that index and delivered via
 * skb_tstamp_tx().
 *
 * Lock hierarchy: ptp_port.tx_lock protects the bitmap and skb array.
 * ----------------------------------------------------------------------- */

/** Maximum TX timestamp entries per port (E810 has 64 per port) */
#define FDICE_PTP_TX_TSTAMP_MAX		64

/** Default TX timestamp timeout in jiffies (2 seconds) */
#define FDICE_PTP_TX_TIMEOUT		(2 * HZ)

/**
 * struct fdice_ptp_tx_entry - Single TX timestamp request tracking
 * @skb: pointer to the SKB awaiting its timestamp
 * @start: jiffies value when the request was submitted
 * @cached_tstamp: last read raw timestamp (for change detection on
 *                 hardware without a ready bitmap)
 */
struct fdice_ptp_tx_entry {
	struct sk_buff	*skb;
	unsigned long	start;
	u64		cached_tstamp;
};

/**
 * struct fdice_ptp_port - Per-port PTP timestamp state
 * @tx_lock: spinlock protecting @in_use, @skbs, and @entries
 * @entries: array of per-index TX timestamp tracking entries
 * @in_use: bitmap indicating which indexes have pending requests
 * @port_num: physical port number (for PHY register addressing)
 * @num_entries: number of available TX timestamp entries
 * @link_up: current link state for this port
 *
 * Allocated once during PTP initialization; the entry array is sized
 * to match the hardware's per-port timestamp block.
 */
struct fdice_ptp_port {
	spinlock_t			tx_lock;
	struct fdice_ptp_tx_entry	*entries;
	unsigned long			in_use[BITS_TO_LONGS(FDICE_PTP_TX_TSTAMP_MAX)];
	u8				port_num;
	u8				num_entries;
	bool				link_up;
};

/* -----------------------------------------------------------------------
 * PTP Statistics
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_ptp_stats - PTP subsystem operational statistics
 * @tx_tstamp_good: successfully delivered TX timestamps
 * @tx_tstamp_skipped: TX timestamp requests skipped (no slot available)
 * @tx_tstamp_timeouts: TX timestamps that timed out
 * @tx_tstamp_flushed: TX timestamps flushed due to interface down
 * @rx_tstamp_extended: RX timestamps successfully extended to 64-bit
 * @late_phc_updates: periodic PHC cache updates that arrived late
 *
 * All counters are monotonically increasing and never reset.
 */
struct fdice_ptp_stats {
	u64	tx_tstamp_good;
	u32	tx_tstamp_skipped;
	u32	tx_tstamp_timeouts;
	u32	tx_tstamp_flushed;
	u64	rx_tstamp_extended;
	u32	late_phc_updates;
};

/* -----------------------------------------------------------------------
 * Top-Level PTP Structure
 *
 * Embedded in struct fdice_dev.  Aggregates all PTP state: the state
 * machine, PHC clock device, hardware capabilities, per-port tracking,
 * cached PHC time for RX extension, and the periodic worker thread.
 *
 * Lock hierarchy (acquire in this order, never reverse):
 *   1. PTP hardware semaphore (fdice_ptp_hw_sem_acquire)
 *   2. ptp_port.tx_lock
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_ptp - PTP subsystem state
 * @state: current lifecycle state
 * @clock: registered ptp_clock device (NULL until registration)
 * @info: ptp_clock_info ops structure filled during init
 * @hw_caps: detected hardware capabilities
 * @port: per-port TX timestamp tracking
 * @cached_phc_time: periodically refreshed PHC time for RX extension
 * @cached_phc_jiffies: jiffies when @cached_phc_time was last updated
 * @phc_work: delayed kthread work for periodic PHC time caching
 * @kworker: dedicated kernel worker thread for PTP tasks
 * @src_tmr_owner: true if this PF owns the source timer
 * @stats: operational statistics
 * @tstamp_config: current hardware timestamping configuration
 * @tx_timeout_jiffies: configurable TX timestamp timeout
 */
struct fdice_ptp {
	enum fdice_ptp_state		state;
	struct ptp_clock		*clock;
	struct ptp_clock_info		info;
	struct fdice_ptp_hw_caps	hw_caps;

	struct fdice_ptp_port		port;

	u64				cached_phc_time;
	unsigned long			cached_phc_jiffies;

	struct kthread_delayed_work	phc_work;
	struct kthread_worker		*kworker;

	bool				src_tmr_owner;

	struct fdice_ptp_stats		stats;
	struct kernel_hwtstamp_config	tstamp_config;
	unsigned long			tx_timeout_jiffies;
};

/* -----------------------------------------------------------------------
 * Container-of Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_from_info - Get fdice_ptp from ptp_clock_info
 * @info: ptp_clock_info pointer passed into PHC callbacks
 *
 * Return: pointer to the containing fdice_ptp structure.
 */
static inline struct fdice_ptp *fdice_ptp_from_info(struct ptp_clock_info *info)
{
	return container_of(info, struct fdice_ptp, info);
}

/* -----------------------------------------------------------------------
 * Public API -- PTP Lifecycle
 * ----------------------------------------------------------------------- */

int fdice_ptp_init(struct fdice_dev *fdev);
void fdice_ptp_cleanup(struct fdice_dev *fdev);

/* -----------------------------------------------------------------------
 * Public API -- TX / RX Timestamp Processing
 * ----------------------------------------------------------------------- */

s8 fdice_ptp_tx_request(struct fdice_ptp_port *port, struct sk_buff *skb);
void fdice_ptp_tx_tstamp(struct fdice_dev *fdev);
u64 fdice_ptp_rx_tstamp(struct fdice_ptp *ptp, u64 raw_tstamp_40bit);

/* -----------------------------------------------------------------------
 * Public API -- PHC Operations (registered via ptp_clock_info)
 * ----------------------------------------------------------------------- */

int fdice_ptp_adjfine(struct ptp_clock_info *info, long scaled_ppm);
int fdice_ptp_adjtime(struct ptp_clock_info *info, s64 delta);
int fdice_ptp_gettimex64(struct ptp_clock_info *info,
			 struct timespec64 *ts,
			 struct ptp_system_timestamp *sts);
int fdice_ptp_settime64(struct ptp_clock_info *info,
			const struct timespec64 *ts);
int fdice_ptp_enable(struct ptp_clock_info *info,
		     struct ptp_clock_request *rq, int on);

#endif /* _FDICE_PTP_H_ */
