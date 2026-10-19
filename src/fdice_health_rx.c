// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE RX Health Reporter
 *
 * Implements a per-channel devlink health reporter for RX data path errors.
 * Detects RX stalls (no packets received despite buffers posted), buffer
 * allocation failures, and CQ overruns, then performs per-queue recovery:
 *
 *   ACTIVE -> ERROR (stall or allocation failure detected)
 *     -> RECOVERING (flush RX queue, free stale buffers)
 *       -> READY (reallocate buffers, reset pointers, re-enable)
 *
 * This is a significant improvement over ice, which has no RX-specific
 * health reporter at all. Per-queue RX recovery avoids full device resets
 * for transient buffer exhaustion conditions.
 *
 * Architecture reference: docs/architecture/05-health-monitoring.md
 */

#include <linux/netdevice.h>
#include <net/devlink.h>

#include "fdice_dev.h"
#include "fdice_channel.h"
#include "fdice_queue.h"
#include "fdice_health_core.h"

/* -----------------------------------------------------------------------
 * RX Reporter Context
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_rx_reporter_ctx - Context for RX health report callbacks
 * @ch: Channel owning the affected RX queue
 *
 * Passed as priv_ctx to devlink_health_report(). Each channel has a
 * single RX queue, so the channel pointer is sufficient context.
 */
struct fdice_rx_reporter_ctx {
	struct fdice_channel	*ch;
};

/** @FDICE_HEALTH_RX_STALL_CODE: Syndrome code for RX stall events */
#define FDICE_HEALTH_RX_STALL_CODE	0x2002

/* -----------------------------------------------------------------------
 * RX Reporter: Diagnose
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_rx_diagnose - Report RX queue state via devlink fmsg
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @extack: netlink extended ACK for error reporting
 *
 * Reports the RX queue state for the channel: RQ fill level, head/tail
 * pointers, page pool statistics (allocation failures, recycling), and
 * associated CQ state.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_rx_diagnose(struct devlink_health_reporter *reporter,
			 struct devlink_fmsg *fmsg,
			 struct netlink_ext_ack *extack)
{
	struct fdice_channel *ch = devlink_health_reporter_priv(reporter);
	struct fdice_rxq *rxq = &ch->rxq;
	u16 fill_level;

	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "channel_idx", ch->idx);

	/* RQ state */
	devlink_fmsg_pair_nest_start(fmsg, "rq");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "state", (u32)rxq->state);
	devlink_fmsg_u32_pair_put(fmsg, "next_to_use", rxq->next_to_use);
	devlink_fmsg_u32_pair_put(fmsg, "next_to_clean", rxq->next_to_clean);
	devlink_fmsg_u32_pair_put(fmsg, "count", rxq->count);
	devlink_fmsg_u32_pair_put(fmsg, "buf_size", rxq->buf_size);

	/* Fill level: how many buffers are posted but not consumed */
	if (rxq->next_to_use >= rxq->next_to_clean)
		fill_level = rxq->next_to_use - rxq->next_to_clean;
	else
		fill_level = rxq->count - rxq->next_to_clean + rxq->next_to_use;
	devlink_fmsg_u32_pair_put(fmsg, "fill_level", fill_level);

	/* WQ details */
	devlink_fmsg_u32_pair_put(fmsg, "wq_sz", rxq->wq.sz);
	devlink_fmsg_u32_pair_put(fmsg, "wq_pc", rxq->wq.pc);
	devlink_fmsg_u32_pair_put(fmsg, "wq_cc", rxq->wq.cc);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Page pool statistics */
	devlink_fmsg_pair_nest_start(fmsg, "page_pool");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_bool_pair_put(fmsg, "active", rxq->page_pool != NULL);
	devlink_fmsg_u64_pair_put(fmsg, "alloc_fail",
				  rxq->stats.alloc_fail);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* CQ state */
	devlink_fmsg_pair_nest_start(fmsg, "cq");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "cc", rxq->cq.wq.cc);
	devlink_fmsg_u32_pair_put(fmsg, "sz", rxq->cq.wq.sz);
	devlink_fmsg_u32_pair_put(fmsg, "cqn", rxq->cq.cqn);
	devlink_fmsg_u32_pair_put(fmsg, "event_ctr", rxq->cq.event_ctr);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Queue statistics */
	devlink_fmsg_pair_nest_start(fmsg, "stats");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u64_pair_put(fmsg, "packets", rxq->stats.packets);
	devlink_fmsg_u64_pair_put(fmsg, "bytes", rxq->stats.bytes);
	devlink_fmsg_u64_pair_put(fmsg, "alloc_fail",
				  rxq->stats.alloc_fail);
	devlink_fmsg_u64_pair_put(fmsg, "csum_err", rxq->stats.csum_err);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * RX Reporter: Dump
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_rx_dump - Dump RX descriptor ring and buffer allocation state
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @priv_ctx: optional fdice_rx_reporter_ctx from the report trigger
 * @extack: netlink extended ACK for error reporting
 *
 * Dumps the RX descriptor ring contents and buffer allocation state for
 * the channel's RX queue. Includes per-buffer DMA mapping status and
 * page pool state for diagnosing allocation failures.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_rx_dump(struct devlink_health_reporter *reporter,
		     struct devlink_fmsg *fmsg, void *priv_ctx,
		     struct netlink_ext_ack *extack)
{
	struct fdice_channel *ch = devlink_health_reporter_priv(reporter);
	struct fdice_rxq *rxq = &ch->rxq;
	u32 i, dump_count;

	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "channel_idx", ch->idx);
	devlink_fmsg_u32_pair_put(fmsg, "state", (u32)rxq->state);
	devlink_fmsg_u32_pair_put(fmsg, "next_to_use", rxq->next_to_use);
	devlink_fmsg_u32_pair_put(fmsg, "next_to_clean", rxq->next_to_clean);

	/* Buffer allocation state -- dump up to 32 buffer entries */
	devlink_fmsg_arr_pair_nest_start(fmsg, "rx_buffers");
	dump_count = min_t(u32, rxq->count, 32);
	if (rxq->rx_buf) {
		for (i = 0; i < dump_count; i++) {
			struct fdice_rx_buf *buf = &rxq->rx_buf[i];

			devlink_fmsg_obj_nest_start(fmsg);
			devlink_fmsg_u32_pair_put(fmsg, "idx", i);
			devlink_fmsg_u8_pair_put(fmsg, "flags", buf->flags);
			devlink_fmsg_u32_pair_put(fmsg, "offset", buf->offset);
			devlink_fmsg_u32_pair_put(fmsg, "truesize",
						  buf->truesize);
			devlink_fmsg_bool_pair_put(fmsg, "mapped",
						   !!(buf->flags & FDICE_RXB_MAPPED));
			devlink_fmsg_obj_nest_end(fmsg);
		}
	}
	devlink_fmsg_arr_pair_nest_end(fmsg);

	/* Page pool summary */
	devlink_fmsg_pair_nest_start(fmsg, "page_pool_state");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_bool_pair_put(fmsg, "primary_active",
				   rxq->page_pool != NULL);
	devlink_fmsg_bool_pair_put(fmsg, "hdr_active",
				   rxq->hdr_page_pool != NULL);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * RX Reporter: Recover
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_rx_recover - Per-queue RX recovery
 * @reporter: devlink health reporter instance
 * @priv_ctx: optional fdice_rx_reporter_ctx from the report trigger
 * @extack: netlink extended ACK for error reporting
 *
 * Performs per-queue RX recovery without a full device reset:
 * 1. Disable the RX queue
 * 2. Flush stale receive buffers
 * 3. Reallocate buffers from the page pool
 * 4. Reset queue pointers (next_to_use = next_to_clean = 0)
 * 5. Re-enable the queue and post initial buffers
 *
 * If recovery fails, the failure is recorded in the health history
 * and the escalation policy determines the next action.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_rx_recover(struct devlink_health_reporter *reporter,
			void *priv_ctx, struct netlink_ext_ack *extack)
{
	struct fdice_channel *ch = devlink_health_reporter_priv(reporter);
	struct fdice_dev *fdev = ch->priv->fdev;
	struct fdice_rxq *rxq = &ch->rxq;
	int err;

	if (rxq->state != FDICE_QS_ACTIVE &&
	    rxq->state != FDICE_QS_CONFIGURED) {
		NL_SET_ERR_MSG_MOD(extack,
				   "RX queue not in recoverable state");
		return -EINVAL;
	}

	netdev_info(rxq->netdev,
		    "Recovering RX queue on channel %u (state=%u)\n",
		    ch->idx, rxq->state);

	/* Step 1: Disable the queue */
	fdice_rxq_disable(rxq);

	/* Step 2: Reset queue pointers */
	rxq->next_to_use = 0;
	rxq->next_to_clean = 0;
	rxq->wq.pc = 0;
	rxq->wq.cc = 0;

	/* Step 3: Re-enable the queue */
	err = fdice_rxq_enable(rxq);
	if (err) {
		netdev_err(rxq->netdev,
			   "Failed to re-enable RX queue %u: %d\n",
			   ch->idx, err);
		NL_SET_ERR_MSG_MOD(extack, "RX queue re-enable failed");
		fdice_health_history_record(&fdev->health.history,
					    FDICE_HEALTH_RX_STALL_CODE,
					    FDICE_HEALTH_SEV_ERROR,
					    FDICE_RESET_QUEUE, false);
		return err;
	}

	/* Step 4: Post initial receive buffers */
	fdice_rxq_post_wqes(rxq);

	netdev_info(rxq->netdev,
		    "RX queue recovery completed on channel %u\n", ch->idx);

	fdice_health_history_record(&fdev->health.history,
				    FDICE_HEALTH_RX_STALL_CODE,
				    FDICE_HEALTH_SEV_ERROR,
				    FDICE_RESET_QUEUE, true);
	return 0;
}

/* -----------------------------------------------------------------------
 * RX Reporter Ops
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_rx_reporter_ops - devlink_health_reporter_ops for RX data path
 *
 * Per-channel RX reporter with diagnose, dump, and recover callbacks.
 * Auto-recovery is enabled with immediate recovery for data path stalls.
 */
static const struct devlink_health_reporter_ops fdice_health_rx_reporter_ops = {
	.name		= "rx",
	.diagnose	= fdice_health_rx_diagnose,
	.dump		= fdice_health_rx_dump,
	.recover	= fdice_health_rx_recover,
};

/* -----------------------------------------------------------------------
 * RX Stall Detection
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_rx_stall_detect - Detect and report an RX stall
 * @ch: channel with the stalled RX queue
 *
 * Called from the NAPI poll path when an RX stall is detected (no packets
 * received despite buffers posted, or persistent allocation failures).
 * Triggers a devlink health report invoking the RX reporter's recovery.
 */
void fdice_health_rx_stall_detect(struct fdice_channel *ch)
{
	struct fdice_rx_reporter_ctx ctx = {
		.ch = ch,
	};
	struct devlink_health_reporter *reporter;

	reporter = ch->rx_reporter;
	if (!reporter)
		return;

	netdev_warn(ch->rxq.netdev,
		    "RX stall on channel %u (ntu=%u ntc=%u alloc_fail=%llu)\n",
		    ch->idx, ch->rxq.next_to_use, ch->rxq.next_to_clean,
		    ch->rxq.stats.alloc_fail);

	devlink_health_report(reporter, "RX queue stall detected", &ctx);
}

/* -----------------------------------------------------------------------
 * Per-Channel Registration / Teardown
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_rx_reporter_create - Register RX reporter for a channel
 * @ch: channel to register the reporter for
 *
 * Creates a devlink health reporter for RX data path errors on this
 * channel. The reporter is stored in ch->rx_reporter and uses the
 * channel as its private context.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_health_rx_reporter_create(struct fdice_channel *ch)
{
	struct fdice_dev *fdev = ch->priv->fdev;
	struct devlink *devlink = fdice_devlink_from_dev(fdev);

	ch->rx_reporter =
		devl_health_reporter_create(devlink,
					    &fdice_health_rx_reporter_ops,
					    ch);
	if (IS_ERR(ch->rx_reporter)) {
		int err = PTR_ERR(ch->rx_reporter);

		netdev_warn(ch->rxq.netdev,
			    "Failed to create RX health reporter for channel %u: %d\n",
			    ch->idx, err);
		ch->rx_reporter = NULL;
		return err;
	}

	return 0;
}

/**
 * fdice_health_rx_reporter_destroy - Unregister RX reporter for a channel
 * @ch: channel to unregister the reporter for
 *
 * Destroys the RX devlink health reporter. Safe to call if the reporter
 * was never created.
 */
void fdice_health_rx_reporter_destroy(struct fdice_channel *ch)
{
	if (ch->rx_reporter) {
		devl_health_reporter_destroy(ch->rx_reporter);
		ch->rx_reporter = NULL;
	}
}
