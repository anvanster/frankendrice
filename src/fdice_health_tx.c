// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE TX Health Reporter
 *
 * Implements a per-channel devlink health reporter for TX data path errors.
 * Unlike ice's device-level tx_hang reporter (dump-only, no recovery),
 * this reporter performs per-queue recovery without a full device reset:
 *
 *   ACTIVE -> ERROR (timeout or CQE error detected)
 *     -> RESETTING (disable TX queue via AQ, flush completions)
 *       -> READY (reset state, re-enable queue via AQ, resume NAPI)
 *
 * If per-queue recovery fails, the reporter escalates to channel-level
 * recovery, then PF-level reset, following the escalation policy in
 * fdice_health_core.h.
 *
 * Architecture reference: docs/architecture/05-health-monitoring.md
 */

#include <linux/netdevice.h>
#include <net/devlink.h>

#include "fdice_dev.h"
#include "fdice_channel.h"
#include "fdice_queue.h"
#include "fdice_health_core.h"

/** @FDICE_HEALTH_TX_TIMEOUT_CODE: Syndrome code for TX timeout events */
#define FDICE_HEALTH_TX_TIMEOUT_CODE	0x2001

/* -----------------------------------------------------------------------
 * TX Reporter Context
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_tx_reporter_ctx - Context for TX health report callbacks
 * @ch: Channel owning the affected TX queue
 * @tc: Traffic class index of the affected TX queue
 *
 * Passed as priv_ctx to devlink_health_report(). Enables the callbacks
 * to locate the specific queue pair experiencing the problem.
 */
struct fdice_tx_reporter_ctx {
	struct fdice_channel	*ch;
	u8			tc;
};

/* -----------------------------------------------------------------------
 * TX Reporter: Diagnose
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_tx_diagnose - Report TX queue state via devlink fmsg
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @extack: netlink extended ACK for error reporting
 *
 * Reports the state of all TX queues on the channel associated with this
 * reporter: SQ head/tail, producer/consumer counters, CQ state, and
 * pending DMA mapping count.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_tx_diagnose(struct devlink_health_reporter *reporter,
			 struct devlink_fmsg *fmsg,
			 struct netlink_ext_ack *extack)
{
	struct fdice_channel *ch = devlink_health_reporter_priv(reporter);
	struct fdice_txq *txq;
	int tc;

	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "channel_idx", ch->idx);

	devlink_fmsg_arr_pair_nest_start(fmsg, "tx_queues");

	for (tc = 0; tc < FDICE_MAX_TC; tc++) {
		txq = &ch->txq[tc];
		if (txq->state == FDICE_QS_UNINIT)
			continue;

		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u8_pair_put(fmsg, "tc", tc);
		devlink_fmsg_u32_pair_put(fmsg, "state", (u32)txq->state);

		/* SQ state: producer and consumer counters */
		devlink_fmsg_pair_nest_start(fmsg, "sq");
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "pc", txq->pc);
		devlink_fmsg_u32_pair_put(fmsg, "cc", txq->cc);
		devlink_fmsg_u32_pair_put(fmsg, "wq_sz", txq->wq.sz);
		devlink_fmsg_u32_pair_put(fmsg, "wq_pc", txq->wq.pc);
		devlink_fmsg_u32_pair_put(fmsg, "wq_cc", txq->wq.cc);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);

		/* CQ state */
		devlink_fmsg_pair_nest_start(fmsg, "cq");
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "cc", txq->cq.wq.cc);
		devlink_fmsg_u32_pair_put(fmsg, "sz", txq->cq.wq.sz);
		devlink_fmsg_u32_pair_put(fmsg, "cqn", txq->cq.cqn);
		devlink_fmsg_u32_pair_put(fmsg, "event_ctr", txq->cq.event_ctr);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);

		/* Pending DMA mappings */
		devlink_fmsg_u32_pair_put(fmsg, "dma_fifo_pc",
					  txq->dma_fifo_pc);
		devlink_fmsg_u32_pair_put(fmsg, "dma_fifo_cc",
					  txq->dma_fifo_cc);
		devlink_fmsg_u32_pair_put(fmsg, "pending_dma",
					  txq->dma_fifo_pc - txq->dma_fifo_cc);

		/* Queue statistics */
		devlink_fmsg_pair_nest_start(fmsg, "stats");
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u64_pair_put(fmsg, "packets", txq->stats.packets);
		devlink_fmsg_u64_pair_put(fmsg, "bytes", txq->stats.bytes);
		devlink_fmsg_u64_pair_put(fmsg, "stopped", txq->stats.stopped);
		devlink_fmsg_u64_pair_put(fmsg, "dropped", txq->stats.dropped);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);

		devlink_fmsg_obj_nest_end(fmsg);
	}

	devlink_fmsg_arr_pair_nest_end(fmsg);
	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * TX Reporter: Dump
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_tx_dump - Dump TX descriptor ring and pending SKBs
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @priv_ctx: optional fdice_tx_reporter_ctx from the report trigger
 * @extack: netlink extended ACK for error reporting
 *
 * Dumps the full TX descriptor ring contents for the affected queue,
 * information about pending SKBs, and associated CQ entries. If priv_ctx
 * identifies a specific TC, only that queue is dumped; otherwise all
 * active TCs on the channel are included.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_tx_dump(struct devlink_health_reporter *reporter,
		     struct devlink_fmsg *fmsg, void *priv_ctx,
		     struct netlink_ext_ack *extack)
{
	struct fdice_channel *ch = devlink_health_reporter_priv(reporter);
	struct fdice_tx_reporter_ctx *ctx = priv_ctx;
	struct fdice_txq *txq;
	int tc_start, tc_end, tc;
	u32 i;

	if (ctx) {
		tc_start = ctx->tc;
		tc_end = ctx->tc + 1;
	} else {
		tc_start = 0;
		tc_end = FDICE_MAX_TC;
	}

	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "channel_idx", ch->idx);

	devlink_fmsg_arr_pair_nest_start(fmsg, "tx_queue_dumps");

	for (tc = tc_start; tc < tc_end; tc++) {
		txq = &ch->txq[tc];
		if (txq->state == FDICE_QS_UNINIT)
			continue;

		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u8_pair_put(fmsg, "tc", (u8)tc);
		devlink_fmsg_u32_pair_put(fmsg, "state", (u32)txq->state);
		devlink_fmsg_u32_pair_put(fmsg, "pc", txq->pc);
		devlink_fmsg_u32_pair_put(fmsg, "cc", txq->cc);

		/* Pending TX buffer info */
		devlink_fmsg_arr_pair_nest_start(fmsg, "pending_bufs");
		if (txq->tx_buf) {
			u32 pending = txq->pc - txq->cc;
			u32 dump_count = min_t(u32, pending, 32);

			for (i = 0; i < dump_count; i++) {
				u32 idx = (txq->cc + i) & txq->wq.mask;
				struct fdice_tx_buf *buf = &txq->tx_buf[idx];

				devlink_fmsg_obj_nest_start(fmsg);
				devlink_fmsg_u32_pair_put(fmsg, "idx", idx);
				devlink_fmsg_u8_pair_put(fmsg, "type",
							 buf->type);
				devlink_fmsg_u32_pair_put(fmsg, "num_dma",
							  buf->num_dma);
				devlink_fmsg_u32_pair_put(fmsg, "num_wqebbs",
							  buf->num_wqebbs);
				devlink_fmsg_obj_nest_end(fmsg);
			}
		}
		devlink_fmsg_arr_pair_nest_end(fmsg);

		/* CQ entries summary */
		devlink_fmsg_pair_nest_start(fmsg, "cq_state");
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "cc", txq->cq.wq.cc);
		devlink_fmsg_u32_pair_put(fmsg, "sz", txq->cq.wq.sz);
		devlink_fmsg_u32_pair_put(fmsg, "arm_state", txq->cq.state);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);

		devlink_fmsg_obj_nest_end(fmsg);
	}

	devlink_fmsg_arr_pair_nest_end(fmsg);
	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * TX Reporter: Recover
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_tx_recover_queue - Per-queue TX recovery via AQ
 * @ch: channel owning the queue
 * @tc: traffic class of the queue to recover
 *
 * Performs per-queue recovery without a full device reset:
 * 1. Mark queue as recovering
 * 2. Disable the TX queue via AQ (dis_txqs, 0x0C31)
 * 3. Flush pending completions
 * 4. Reset queue state (head = tail = 0)
 * 5. Re-enable the TX queue via AQ (add_txqs, 0x0C30)
 * 6. Resume NAPI processing
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_health_tx_recover_queue(struct fdice_channel *ch, u8 tc)
{
	struct fdice_txq *txq = &ch->txq[tc];
	int err;

	if (txq->state != FDICE_QS_ACTIVE &&
	    txq->state != FDICE_QS_CONFIGURED)
		return -EINVAL;

	/* Step 1: Disable the queue */
	fdice_txq_disable(txq);

	/* Step 2: Wait for pending completions to drain */
	{
		unsigned long timeout = jiffies + msecs_to_jiffies(2000);

		while (txq->cc != txq->pc) {
			if (time_after(jiffies, timeout)) {
				netdev_warn(ch->rxq.netdev,
					    "TX queue %u tc %u flush timeout (cc=%u pc=%u)\n",
					    ch->idx, tc, txq->cc, txq->pc);
				break;
			}
			msleep(20);
		}
	}

	/* Step 3: Reset queue state */
	WARN_ONCE(txq->cc != txq->pc,
		  "TX queue %u tc %u: cc (%u) != pc (%u) after flush\n",
		  ch->idx, tc, txq->cc, txq->pc);
	txq->pc = 0;
	txq->cc = 0;
	txq->wq.pc = 0;
	txq->wq.cc = 0;
	txq->dma_fifo_pc = 0;
	txq->dma_fifo_cc = 0;

	/* Step 4: Re-enable the queue */
	err = fdice_txq_enable(txq);
	if (err) {
		netdev_err(ch->rxq.netdev,
			   "Failed to re-enable TX queue %u tc %u: %d\n",
			   ch->idx, tc, err);
		return err;
	}

	return 0;
}

/**
 * fdice_health_tx_recover - TX reporter recover callback
 * @reporter: devlink health reporter instance
 * @priv_ctx: fdice_tx_reporter_ctx identifying the affected queue
 * @extack: netlink extended ACK for error reporting
 *
 * Attempts per-queue recovery. If priv_ctx identifies a specific queue,
 * only that queue is recovered. Otherwise all active TX queues on the
 * channel are recovered.
 *
 * If per-queue recovery fails, escalation to channel-level or PF-level
 * recovery occurs based on the history tracking thresholds.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_tx_recover(struct devlink_health_reporter *reporter,
			void *priv_ctx, struct netlink_ext_ack *extack)
{
	struct fdice_channel *ch = devlink_health_reporter_priv(reporter);
	struct fdice_dev *fdev = ch->priv->fdev;
	struct fdice_tx_reporter_ctx *ctx = priv_ctx;
	int tc, err;

	/* If triggered with context, recover only the specific queue */
	if (ctx) {
		err = fdice_health_tx_recover_queue(ch, ctx->tc);
		if (err) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Per-queue TX recovery failed");
			fdice_health_history_record(&fdev->health.history,
						    FDICE_HEALTH_TX_TIMEOUT_CODE,
						    FDICE_HEALTH_SEV_ERROR,
						    FDICE_RESET_QUEUE, false);
			return err;
		}

		fdice_health_history_record(&fdev->health.history,
					    FDICE_HEALTH_TX_TIMEOUT_CODE,
					    FDICE_HEALTH_SEV_ERROR,
					    FDICE_RESET_QUEUE, true);
		return 0;
	}

	/* No context: recover all active TX queues on this channel */
	for (tc = 0; tc < FDICE_MAX_TC; tc++) {
		if (ch->txq[tc].state == FDICE_QS_UNINIT)
			continue;

		err = fdice_health_tx_recover_queue(ch, tc);
		if (err) {
			NL_SET_ERR_MSG_MOD(extack,
					   "TX queue recovery failed");
			fdice_health_history_record(&fdev->health.history,
						    FDICE_HEALTH_TX_TIMEOUT_CODE,
						    FDICE_HEALTH_SEV_ERROR,
						    FDICE_RESET_QUEUE, false);
			return err;
		}
	}

	fdice_health_history_record(&fdev->health.history,
				    FDICE_HEALTH_TX_TIMEOUT_CODE,
				    FDICE_HEALTH_SEV_ERROR,
				    FDICE_RESET_QUEUE, true);
	return 0;
}

/* -----------------------------------------------------------------------
 * TX Reporter Ops
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_tx_reporter_ops - devlink_health_reporter_ops for TX data path
 *
 * Per-queue TX reporter with diagnose, dump, and recover callbacks.
 * Auto-recovery is enabled with a 0ms grace period (immediate recovery
 * for data path errors).
 */
static const struct devlink_health_reporter_ops fdice_health_tx_reporter_ops = {
	.name		= "tx",
	.diagnose	= fdice_health_tx_diagnose,
	.dump		= fdice_health_tx_dump,
	.recover	= fdice_health_tx_recover,
};

/* -----------------------------------------------------------------------
 * TX Timeout Detection
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_tx_timeout_detect - Detect and report a TX timeout
 * @ch: channel containing the stuck TX queue
 * @tc: traffic class of the affected queue
 *
 * Called from the NAPI poll path or network watchdog when a TX queue
 * appears stuck (no completions for an extended period). Triggers a
 * devlink health report which invokes the TX reporter's diagnose, dump,
 * and recover callbacks.
 */
void fdice_health_tx_timeout_detect(struct fdice_channel *ch, u8 tc)
{
	struct fdice_tx_reporter_ctx ctx = {
		.ch = ch,
		.tc = tc,
	};
	struct devlink_health_reporter *reporter;

	reporter = ch->tx_reporter;
	if (!reporter)
		return;

	netdev_warn(ch->rxq.netdev,
		    "TX timeout on channel %u tc %u (pc=%u cc=%u)\n",
		    ch->idx, tc, ch->txq[tc].pc, ch->txq[tc].cc);

	devlink_health_report(reporter, "TX queue timeout detected", &ctx);
}

/* -----------------------------------------------------------------------
 * Per-Channel Registration / Teardown
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_tx_reporter_create - Register TX reporter for a channel
 * @ch: channel to register the reporter for
 *
 * Creates a devlink health reporter for TX data path errors on this
 * channel. The reporter is stored in ch->tx_reporter and uses the
 * channel as its private context.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_health_tx_reporter_create(struct fdice_channel *ch)
{
	struct fdice_dev *fdev = ch->priv->fdev;
	struct devlink *devlink = fdice_devlink_from_dev(fdev);

	ch->tx_reporter =
		devl_health_reporter_create(devlink,
					    &fdice_health_tx_reporter_ops,
					    ch);
	if (IS_ERR(ch->tx_reporter)) {
		int err = PTR_ERR(ch->tx_reporter);

		netdev_warn(ch->rxq.netdev,
			    "Failed to create TX health reporter for channel %u: %d\n",
			    ch->idx, err);
		ch->tx_reporter = NULL;
		return err;
	}

	return 0;
}

/**
 * fdice_health_tx_reporter_destroy - Unregister TX reporter for a channel
 * @ch: channel to unregister the reporter for
 *
 * Destroys the TX devlink health reporter. Safe to call if the reporter
 * was never created.
 */
void fdice_health_tx_reporter_destroy(struct fdice_channel *ch)
{
	if (ch->tx_reporter) {
		devl_health_reporter_destroy(ch->tx_reporter);
		ch->tx_reporter = NULL;
	}
}
