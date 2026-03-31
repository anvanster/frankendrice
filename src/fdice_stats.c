// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_stats.c - Statistics collection and reporting
 *
 * Phase 3.11 implementation: per-port and per-queue statistics
 * aggregation for rtnl_link_stats64 and ethtool_stats.
 *
 * Design rationale:
 *   - Per-queue stats are maintained in the fdice_txq_stats and
 *     fdice_rxq_stats structures embedded in each queue, updated
 *     from NAPI poll context without locking (single-writer per queue).
 *   - Port-level stats are derived by aggregating per-queue counters
 *     on demand (in ndo_get_stats64 and ethtool get_ethtool_stats),
 *     avoiding the overhead of maintaining separate port-level counters.
 *   - Stat string arrays are defined here for use by both ethtool
 *     get_strings and any future debugfs/sysfs stat export.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.11)
 */

#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/string.h>

#include "fdice_dev.h"
#include "fdice_channel.h"
#include "fdice_queue.h"

/* -----------------------------------------------------------------------
 * Stat Name String Arrays
 *
 * These arrays are exported for use by fdice_ethtool.c get_strings.
 * Order must match the stat reading order in fdice_get_ethtool_stats().
 * ----------------------------------------------------------------------- */

/**
 * fdice_port_stat_strings - Port-level statistic names
 *
 * Aggregated from per-queue counters. Reported first in ethtool -S output.
 */
const char fdice_port_stat_strings[][ETH_GSTRING_LEN] = {
	"rx_packets",
	"tx_packets",
	"rx_bytes",
	"tx_bytes",
	"rx_errors",
	"tx_errors",
};

/**
 * fdice_txq_stat_strings - Per-TX-queue statistic names
 *
 * Order matches struct fdice_txq_stats field order.
 * Prefixed with "txN_" by fdice_get_strings() in fdice_ethtool.c.
 */
const char fdice_txq_stat_strings[][ETH_GSTRING_LEN] = {
	"packets",
	"bytes",
	"tso_packets",
	"tso_bytes",
	"linearize",
	"csum_partial",
	"stopped",
	"wake",
	"dropped",
};

/**
 * fdice_rxq_stat_strings - Per-RX-queue statistic names
 *
 * Order matches struct fdice_rxq_stats field order.
 * Prefixed with "rxN_" by fdice_get_strings() in fdice_ethtool.c.
 */
const char fdice_rxq_stat_strings[][ETH_GSTRING_LEN] = {
	"packets",
	"bytes",
	"alloc_fail",
	"csum_complete",
	"csum_none",
	"csum_err",
	"xdp_drop",
	"xdp_tx",
	"xdp_redirect",
	"lro_packets",
	"lro_bytes",
};

/* -----------------------------------------------------------------------
 * Port-Level Statistics Aggregation
 * ----------------------------------------------------------------------- */

/**
 * fdice_stats_get - Aggregate per-queue stats into rtnl_link_stats64
 * @fdev: core device structure
 * @stats: rtnl_link_stats64 structure to populate
 *
 * Iterates all channels and accumulates TX/RX packet, byte, and error
 * counters into the kernel stats structure. Called from ndo_get_stats64.
 *
 * Per-queue stats are updated from NAPI poll context (single writer),
 * so we use READ_ONCE for safe lockless reads from arbitrary context.
 */
void fdice_stats_get(struct fdice_dev *fdev,
		     struct rtnl_link_stats64 *stats)
{
	struct fdice_channel *ch;
	u16 q;

	memset(stats, 0, sizeof(*stats));

	if (!fdev->channels)
		return;

	for (q = 0; q < fdev->num_channels; q++) {
		struct fdice_txq_stats *ts;
		struct fdice_rxq_stats *rs;

		ch = &fdev->channels[q];

		/* Aggregate TX stats from TC 0 (primary TX queue) */
		ts = &ch->txq[0].stats;
		stats->tx_packets += READ_ONCE(ts->packets);
		stats->tx_bytes += READ_ONCE(ts->bytes);
		stats->tx_dropped += READ_ONCE(ts->dropped);

		/* Aggregate RX stats */
		rs = &ch->rxq.stats;
		stats->rx_packets += READ_ONCE(rs->packets);
		stats->rx_bytes += READ_ONCE(rs->bytes);
		stats->rx_errors += READ_ONCE(rs->csum_err);
		stats->rx_dropped += READ_ONCE(rs->alloc_fail);

		/* Multicast count includes XDP redirected packets */
		stats->multicast += READ_ONCE(rs->xdp_redirect);
	}
}

/* -----------------------------------------------------------------------
 * Per-Queue Statistics Update
 * ----------------------------------------------------------------------- */

/**
 * fdice_stats_update_txq - Update TX queue statistics after completion
 * @txq: transmit queue
 * @packets: number of packets completed
 * @bytes: number of bytes completed
 *
 * Called from TX completion path in NAPI poll. Single-writer per queue,
 * no locking needed. Uses WRITE_ONCE for safe publication to readers.
 */
void fdice_stats_update_txq(struct fdice_txq *txq, u64 packets, u64 bytes)
{
	struct fdice_txq_stats *s = &txq->stats;

	WRITE_ONCE(s->packets, s->packets + packets);
	WRITE_ONCE(s->bytes, s->bytes + bytes);
}

/**
 * fdice_stats_update_rxq - Update RX queue statistics after poll
 * @rxq: receive queue
 * @packets: number of packets processed
 * @bytes: number of bytes processed
 *
 * Called from RX completion path in NAPI poll. Single-writer per queue,
 * no locking needed. Uses WRITE_ONCE for safe publication to readers.
 */
void fdice_stats_update_rxq(struct fdice_rxq *rxq, u64 packets, u64 bytes)
{
	struct fdice_rxq_stats *s = &rxq->stats;

	WRITE_ONCE(s->packets, s->packets + packets);
	WRITE_ONCE(s->bytes, s->bytes + bytes);
}

/**
 * fdice_stats_inc_txq_tso - Increment TSO packet/byte counters
 * @txq: transmit queue
 * @packets: number of TSO segments
 * @bytes: total TSO payload bytes
 *
 * Called when a TSO offload is submitted on the TX path.
 */
void fdice_stats_inc_txq_tso(struct fdice_txq *txq, u64 packets, u64 bytes)
{
	struct fdice_txq_stats *s = &txq->stats;

	WRITE_ONCE(s->tso_packets, s->tso_packets + packets);
	WRITE_ONCE(s->tso_bytes, s->tso_bytes + bytes);
}

/**
 * fdice_stats_inc_txq_stop - Record a TX queue stop event
 * @txq: transmit queue
 *
 * Called when the TX ring is full and the queue is stopped.
 */
void fdice_stats_inc_txq_stop(struct fdice_txq *txq)
{
	WRITE_ONCE(txq->stats.stopped, txq->stats.stopped + 1);
}

/**
 * fdice_stats_inc_txq_wake - Record a TX queue wake event
 * @txq: transmit queue
 *
 * Called when TX completions free enough space to restart the queue.
 */
void fdice_stats_inc_txq_wake(struct fdice_txq *txq)
{
	WRITE_ONCE(txq->stats.wake, txq->stats.wake + 1);
}

/**
 * fdice_stats_inc_rxq_alloc_fail - Record an RX buffer allocation failure
 * @rxq: receive queue
 *
 * Called when page_pool_alloc_pages fails during RX refill.
 */
void fdice_stats_inc_rxq_alloc_fail(struct fdice_rxq *rxq)
{
	WRITE_ONCE(rxq->stats.alloc_fail, rxq->stats.alloc_fail + 1);
}

/**
 * fdice_stats_inc_rxq_csum - Record an RX checksum result
 * @rxq: receive queue
 * @complete: true if HW checksum was verified correct
 * @err: true if HW detected a checksum error
 *
 * Called during RX descriptor parsing to classify checksum results.
 */
void fdice_stats_inc_rxq_csum(struct fdice_rxq *rxq, bool complete, bool err)
{
	struct fdice_rxq_stats *s = &rxq->stats;

	if (err)
		WRITE_ONCE(s->csum_err, s->csum_err + 1);
	else if (complete)
		WRITE_ONCE(s->csum_complete, s->csum_complete + 1);
	else
		WRITE_ONCE(s->csum_none, s->csum_none + 1);
}

/**
 * fdice_stats_inc_rxq_xdp - Record an XDP action
 * @rxq: receive queue
 * @action: XDP action taken (XDP_DROP, XDP_TX, XDP_REDIRECT)
 *
 * Called after XDP program returns a verdict to track XDP statistics.
 */
void fdice_stats_inc_rxq_xdp(struct fdice_rxq *rxq, u32 action)
{
	struct fdice_rxq_stats *s = &rxq->stats;

	switch (action) {
	case 1: /* XDP_DROP */
		WRITE_ONCE(s->xdp_drop, s->xdp_drop + 1);
		break;
	case 2: /* XDP_TX */
		WRITE_ONCE(s->xdp_tx, s->xdp_tx + 1);
		break;
	case 4: /* XDP_REDIRECT */
		WRITE_ONCE(s->xdp_redirect, s->xdp_redirect + 1);
		break;
	}
}
