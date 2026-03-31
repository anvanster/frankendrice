// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_channel.c - Channel management and NAPI integration
 *
 * Phase 3.3 implementation: per-NAPI channel allocation with embedded
 * queues, multi-TC TX queue support, and coordinated enable/disable.
 *
 * Design rationale:
 *   - Each channel groups one RXQ, up to FDICE_MAX_TC TXQs, and a NAPI
 *     instance on a single CPU/EQ/MSI-X vector. Queues are embedded
 *     (not pointed to) in the channel struct for cache locality, following
 *     mlx5's mlx5e_channel pattern rather than ice's linked-list approach.
 *   - Channel create/destroy follow strict reverse-order cleanup with
 *     goto chains to prevent resource leaks on partial init.
 *   - alloc_channels/free_channels manage the full device channel array;
 *     open/close add enable/disable around that.
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/cpumask.h>

#include "fdice_dev.h"
#include "fdice_queue.h"
#include "fdice_eq.h"
#include "fdice_channel.h"
#include "fdice_txrx.h"

/* ---- Single Channel Operations ---- */

/**
 * fdice_channel_create - Allocate a channel with all its queues
 * @ch: channel structure to initialize (pre-allocated in channel array)
 * @fdev: core device structure
 * @params: channel configuration parameters
 * @idx: channel index (maps to EQ index for CPU/NUMA binding)
 *
 * Creates the channel's RXQ, TXQ[0..num_tc-1], and initializes NAPI.
 * The channel is bound to the completion EQ at the matching index,
 * inheriting its CPU affinity and NUMA node.
 *
 * Error handling: if TXQ creation fails at TC N, previously created
 * TXQs (0..N-1) and the RXQ are destroyed before returning.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_channel_create(struct fdice_channel *ch, struct fdice_dev *fdev,
			 struct fdice_params *params, u16 idx)
{
	struct fdice_eq *eq;
	int err;
	u8 tc;

	memset(ch, 0, sizeof(*ch));
	ch->priv = &fdev->priv;
	ch->params = params;
	ch->idx = idx;

	/* Bind to the corresponding completion EQ */
	eq = fdice_eq_get_comp(&fdev->eq_table, idx);
	if (!eq) {
		dev_err(&fdev->pdev->dev,
			"channel %u: no matching completion EQ\n", idx);
		return -EINVAL;
	}
	ch->eq = eq;
	ch->cpu = eq->cpu;
	ch->numa_node = cpu_to_node(eq->cpu);

	/* Initialize NAPI before creating queues (queues reference napi) */
	netif_napi_add(ch->priv->netdev, &ch->napi, fdice_napi_poll);

	/* Create the RX queue */
	err = fdice_rxq_create(&ch->rxq, ch, params);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"channel %u: RXQ create failed: %d\n", idx, err);
		goto err_del_napi;
	}

	/* Register RX CQ in parent EQ for interrupt dispatch */
	err = fdice_eq_add_cq(eq, &ch->rxq.cq);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"channel %u: RX CQ register failed: %d\n", idx, err);
		goto err_free_rxq;
	}

	/* Create TX queues, one per traffic class */
	for (tc = 0; tc < params->num_tc; tc++) {
		err = fdice_txq_create(&ch->txq[tc], ch, params, tc);
		if (err) {
			dev_err(&fdev->pdev->dev,
				"channel %u TC %u: TXQ create failed: %d\n",
				idx, tc, err);
			goto err_free_txqs;
		}

		/* Register TX CQ in parent EQ */
		err = fdice_eq_add_cq(eq, &ch->txq[tc].cq);
		if (err) {
			fdice_txq_destroy(&ch->txq[tc]);
			dev_err(&fdev->pdev->dev,
				"channel %u TC %u: TX CQ register failed: %d\n",
				idx, tc, err);
			goto err_free_txqs;
		}
	}

	return 0;

err_free_txqs:
	while (tc > 0) {
		tc--;
		fdice_eq_del_cq(eq, ch->txq[tc].cq.cqn);
		fdice_txq_destroy(&ch->txq[tc]);
	}
	fdice_eq_del_cq(eq, ch->rxq.cq.cqn);
err_free_rxq:
	fdice_rxq_destroy(&ch->rxq);
err_del_napi:
	netif_napi_del(&ch->napi);
	return err;
}

/**
 * fdice_channel_destroy - Release all channel resources
 * @ch: channel to destroy
 *
 * Unregisters CQs from parent EQ, destroys TX queues (reverse TC order),
 * RX queue, and removes NAPI. Safe to call on a partially initialized
 * channel.
 */
void fdice_channel_destroy(struct fdice_channel *ch)
{
	u8 tc;

	if (!ch->params)
		return;

	/* Unregister CQs and destroy TX queues in reverse TC order */
	for (tc = ch->params->num_tc; tc > 0; tc--) {
		if (ch->eq)
			fdice_eq_del_cq(ch->eq, ch->txq[tc - 1].cq.cqn);
		fdice_txq_destroy(&ch->txq[tc - 1]);
	}

	/* Unregister RX CQ and destroy RX queue */
	if (ch->eq)
		fdice_eq_del_cq(ch->eq, ch->rxq.cq.cqn);
	fdice_rxq_destroy(&ch->rxq);

	netif_napi_del(&ch->napi);

	ch->eq = NULL;
	ch->params = NULL;
}

/**
 * fdice_channel_enable - Enable all queues in a channel and start NAPI
 * @ch: channel to enable
 *
 * Enables the RX queue, all TX queues, arms CQs, and calls napi_enable().
 * Queue enable posts initial RX buffers and writes tail registers.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_channel_enable(struct fdice_channel *ch)
{
	int err;
	u8 tc;

	/* Enable RX queue first (post buffers, enable in HW) */
	err = fdice_rxq_enable(&ch->rxq);
	if (err) {
		dev_err(&ch->priv->fdev->pdev->dev,
			"channel %u: RXQ enable failed: %d\n",
			ch->idx, err);
		return err;
	}

	/* Arm RX CQ */
	fdice_cq_arm(&ch->rxq.cq);

	/* Enable TX queues for all traffic classes */
	for (tc = 0; tc < ch->params->num_tc; tc++) {
		err = fdice_txq_enable(&ch->txq[tc]);
		if (err) {
			dev_err(&ch->priv->fdev->pdev->dev,
				"channel %u TC %u: TXQ enable failed: %d\n",
				ch->idx, tc, err);
			goto err_disable_txqs;
		}
		fdice_cq_arm(&ch->txq[tc].cq);
	}

	/* Start NAPI processing */
	napi_enable(&ch->napi);

	return 0;

err_disable_txqs:
	while (tc > 0) {
		tc--;
		fdice_cq_disarm(&ch->txq[tc].cq);
		fdice_txq_disable(&ch->txq[tc]);
	}
	fdice_cq_disarm(&ch->rxq.cq);
	fdice_rxq_disable(&ch->rxq);
	return err;
}

/**
 * fdice_channel_disable - Stop NAPI and disable all queues in a channel
 * @ch: channel to disable
 *
 * Stops NAPI first to prevent new poll invocations, disarms CQs, then
 * disables all TX and RX queues. Hardware drain is awaited during each
 * queue disable.
 */
void fdice_channel_disable(struct fdice_channel *ch)
{
	u8 tc;

	/* Stop NAPI before disabling queues */
	napi_disable(&ch->napi);

	/* Disarm and disable TX queues in reverse TC order */
	for (tc = ch->params->num_tc; tc > 0; tc--) {
		fdice_cq_disarm(&ch->txq[tc - 1].cq);
		fdice_txq_disable(&ch->txq[tc - 1]);
	}

	/* Disarm and disable RX queue last */
	fdice_cq_disarm(&ch->rxq.cq);
	fdice_rxq_disable(&ch->rxq);
}

/* ---- Device-Level Channel Array Operations ---- */

/**
 * fdice_alloc_channels - Allocate all channels for the device
 * @fdev: core device structure
 * @params: channel configuration parameters
 *
 * Allocates the channel array and creates one channel per completion EQ.
 * Each channel gets its own RXQ, TXQ[num_tc], and NAPI instance.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_alloc_channels(struct fdice_dev *fdev, struct fdice_params *params)
{
	struct fdice_channel *channels;
	u16 num_ch = params->num_channels;
	int err;
	u16 i;

	channels = kvcalloc(num_ch, sizeof(struct fdice_channel), GFP_KERNEL);
	if (!channels)
		return -ENOMEM;

	for (i = 0; i < num_ch; i++) {
		err = fdice_channel_create(&channels[i], fdev, params, i);
		if (err) {
			dev_err(&fdev->pdev->dev,
				"failed to create channel %u: %d\n", i, err);
			goto err_free_channels;
		}
	}

	fdev->channels = channels;
	fdev->num_channels = num_ch;
	return 0;

err_free_channels:
	while (i > 0) {
		i--;
		fdice_channel_destroy(&channels[i]);
	}
	kvfree(channels);
	return err;
}

/**
 * fdice_free_channels - Destroy all channels for the device
 * @fdev: core device structure
 *
 * Destroys all channels in reverse order and frees the channel array.
 */
void fdice_free_channels(struct fdice_dev *fdev)
{
	u16 i;

	if (!fdev->channels)
		return;

	for (i = fdev->num_channels; i > 0; i--)
		fdice_channel_destroy(&fdev->channels[i - 1]);

	kvfree(fdev->channels);
	fdev->channels = NULL;
	fdev->num_channels = 0;
}

/**
 * fdice_enable_channels - Enable all channels for the device
 * @fdev: core device structure
 *
 * Enables all channels (queues + NAPI). On failure, disables any
 * channels that were successfully enabled.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_enable_channels(struct fdice_dev *fdev)
{
	int err;
	u16 i;

	for (i = 0; i < fdev->num_channels; i++) {
		err = fdice_channel_enable(&fdev->channels[i]);
		if (err) {
			dev_err(&fdev->pdev->dev,
				"failed to enable channel %u: %d\n", i, err);
			goto err_disable;
		}
	}

	return 0;

err_disable:
	while (i > 0) {
		i--;
		fdice_channel_disable(&fdev->channels[i]);
	}
	return err;
}

/**
 * fdice_disable_channels - Disable all channels for the device
 * @fdev: core device structure
 *
 * Disables all channels (NAPI + queues). Called during close or
 * before reconfiguration.
 */
void fdice_disable_channels(struct fdice_dev *fdev)
{
	u16 i;

	if (!fdev->channels)
		return;

	for (i = 0; i < fdev->num_channels; i++)
		fdice_channel_disable(&fdev->channels[i]);
}
