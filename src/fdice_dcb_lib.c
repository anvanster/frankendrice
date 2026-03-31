// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE DCB Library / Helpers
 *
 * Mid-layer DCB utilities that bridge the dcbnl interface (fdice_dcb.c)
 * and the hardware-facing channel and RDMA subsystems. Responsibilities:
 *
 * - Configuration validation (TC count, BW sum, contiguity)
 * - Channel rebuild orchestration on TC reconfiguration
 * - RDMA event dispatch for TC change notifications
 * - IIDC QoS snapshot synchronization
 *
 * Separated from fdice_dcb.c to keep the dcbnl callback file focused
 * on the netlink interface, following ice's dcb_nl / dcb_lib split.
 *
 * Architecture reference: docs/exploration/10-dcb-qos.md
 */

#include <linux/netdevice.h>
#include <linux/dcbnl.h>
#include <net/dcbnl.h>

#include "fdice_dcb.h"
#include "fdice_dev.h"
#include "fdice_channel.h"
#include "fdice_iidc.h"
#include "fdice_rdma.h"

/* -----------------------------------------------------------------------
 * Configuration Validation
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_tc_contiguous - Check that active TCs are contiguous from TC0
 * @cfg: DCB configuration to check
 *
 * E810 hardware requires traffic classes to be contiguous starting from
 * TC0. A priority-to-TC mapping that skips a TC (e.g., priorities map to
 * TC0 and TC2 but not TC1) is invalid.
 *
 * Return: true if TCs are contiguous, false otherwise
 */
static bool fdice_dcb_tc_contiguous(const struct fdice_dcb_cfg *cfg)
{
	bool tc_seen[FDICE_DCB_MAX_TC] = {};
	u8 max_tc = 0;
	u8 count = 0;
	int i;

	for (i = 0; i < FDICE_DCB_MAX_UP; i++) {
		u8 tc = cfg->ets.prio_tc[i];

		if (tc >= FDICE_DCB_MAX_TC)
			return false;
		if (!tc_seen[tc]) {
			tc_seen[tc] = true;
			count++;
			if (tc > max_tc)
				max_tc = tc;
		}
	}

	/* Contiguous means highest TC + 1 == number of distinct TCs */
	return (max_tc + 1) == count;
}

/**
 * fdice_dcb_validate_config - Validate a DCB configuration
 * @cfg: Configuration to validate
 *
 * Performs comprehensive validation:
 * 1. TC count within hardware limits (1..FDICE_DCB_MAX_TC)
 * 2. All priority-to-TC mappings reference valid TCs
 * 3. ETS bandwidth weights sum to 100 for non-strict TCs
 * 4. TCs are contiguous starting from TC0 (E810 requirement)
 *
 * Return: 0 if valid, -EINVAL on validation failure
 */
int fdice_dcb_validate_config(const struct fdice_dcb_cfg *cfg)
{
	u8 num_tc = cfg->num_tc;
	u8 bw_sum = 0;
	u8 ets_count = 0;
	int i;

	/* TC count range check */
	if (num_tc < 1 || num_tc > FDICE_DCB_MAX_TC)
		return -EINVAL;

	/* Priority-to-TC mapping: all must reference valid TCs */
	for (i = 0; i < FDICE_DCB_MAX_UP; i++) {
		if (cfg->ets.prio_tc[i] >= num_tc)
			return -EINVAL;
	}

	/* Contiguity check */
	if (!fdice_dcb_tc_contiguous(cfg))
		return -EINVAL;

	/*
	 * Bandwidth sum validation: for TCs using ETS (not strict priority),
	 * bandwidth weights must sum to exactly 100.
	 */
	for (i = 0; i < num_tc; i++) {
		if (cfg->ets.tc_tsa[i] == IEEE_8021QAZ_TSA_ETS) {
			bw_sum += cfg->ets.tc_bw[i];
			ets_count++;
		}
	}

	/* If any TCs use ETS scheduling, their weights must sum to 100 */
	if (ets_count > 0 && bw_sum != FDICE_DCB_BW_SUM)
		return -EINVAL;

	return 0;
}

/**
 * fdice_dcb_get_num_tc - Determine active TC count from config
 * @cfg: DCB configuration to inspect
 *
 * Scans the priority-to-TC mapping to find the highest referenced TC
 * and returns that value plus one. This gives the actual number of
 * active traffic classes regardless of what num_tc was set to.
 *
 * Return: Number of active traffic classes (1..FDICE_DCB_MAX_TC)
 */
u8 fdice_dcb_get_num_tc(const struct fdice_dcb_cfg *cfg)
{
	u8 max_tc = 0;
	int i;

	for (i = 0; i < FDICE_DCB_MAX_UP; i++) {
		if (cfg->ets.prio_tc[i] > max_tc)
			max_tc = cfg->ets.prio_tc[i];
	}

	return max_tc + 1;
}

/* -----------------------------------------------------------------------
 * Channel Rebuild
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_rebuild_channels - Rebuild channels for TC reconfiguration
 * @fdev: FrankenDrICE device
 *
 * Performs the close-reconfigure-reopen sequence required when the TC
 * count changes. The sequence:
 * 1. Disable all active channels (NAPI, interrupts)
 * 2. Update netdev TC queue mapping
 * 3. Re-enable channels with new TC layout
 *
 * This avoids the heavyweight dev_close/dev_open cycle used by ice,
 * instead performing a targeted channel rebuild similar to mlx5's
 * safe_switch_params approach.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_dcb_rebuild_channels(struct fdice_dev *fdev)
{
	struct net_device *netdev = fdev->netdev;
	struct device *dev = &fdev->pdev->dev;
	u8 num_tc = fdev->dcb.desired_cfg.num_tc;
	int err;
	int i;

	if (!netdev) {
		dev_dbg(dev, "DCB: no netdev, skipping channel rebuild\n");
		return 0;
	}

	dev_dbg(dev, "DCB: rebuilding channels for %u TCs\n", num_tc);

	/* Step 1: Disable channels */
	fdice_disable_channels(fdev);

	/* Step 2: Update netdev TC configuration */
	if (num_tc > 1) {
		err = netdev_set_num_tc(netdev, num_tc);
		if (err) {
			dev_err(dev, "DCB: netdev_set_num_tc(%u) failed: %d\n",
				num_tc, err);
			goto reopen;
		}

		/*
		 * Map each TC to its queue range. With uniform distribution,
		 * each TC gets num_channels / num_tc queues. TC0 gets any
		 * remainder.
		 */
		if (fdev->num_channels > 0) {
			u16 qcount = fdev->num_channels / num_tc;
			u16 offset = 0;

			for (i = 0; i < num_tc; i++) {
				u16 tc_qcount = qcount;

				/* TC0 absorbs the remainder */
				if (i == 0)
					tc_qcount += fdev->num_channels % num_tc;

				netdev_set_tc_queue(netdev, i, tc_qcount,
						    offset);
				offset += tc_qcount;
			}
		}

		/* Map user priorities to TCs per DCB config */
		for (i = 0; i < FDICE_DCB_MAX_UP; i++)
			netdev_set_prio_tc_map(netdev, i,
					       fdev->dcb.desired_cfg.ets.prio_tc[i]);
	} else {
		netdev_reset_tc(netdev);
	}

reopen:
	/* Step 3: Re-enable channels */
	err = fdice_enable_channels(fdev);
	if (err)
		dev_err(dev, "DCB: channel re-enable failed: %d\n", err);

	return err;
}

/* -----------------------------------------------------------------------
 * RDMA Integration
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_notify_rdma - Send TC change notifications to RDMA driver
 * @fdev: FrankenDrICE device
 * @pre: true for PRE_TC_CHANGE, false for POST_TC_CHANGE
 *
 * Dispatches IIDC TC change events following the ice/irdma proven
 * BEFORE/AFTER pattern:
 * - PRE: RDMA driver suspends QPs and drains completions
 * - POST: RDMA driver remaps QPs to new TC layout using updated QoS
 *
 * For POST events, the QoS snapshot is refreshed before notification
 * so the RDMA driver receives current TC mapping information.
 *
 * Return: 0 on success, negative errno on failure (logged, not fatal)
 */
int fdice_dcb_notify_rdma(struct fdice_dev *fdev, bool pre)
{
	struct fdice_iidc_event event = {};
	struct device *dev = &fdev->pdev->dev;
	int err;

	if (!fdev->rdma.enabled || !fdev->rdma.registered)
		return 0;

	if (pre) {
		event.type = FDICE_IIDC_EVENT_PRE_TC_CHANGE;
	} else {
		event.type = FDICE_IIDC_EVENT_POST_TC_CHANGE;
		/* Ensure QoS snapshot is current before POST notification */
		fdice_dcb_update_qos(fdev);
		if (fdev->rdma.iidc_core)
			event.qos = &fdev->rdma.iidc_core->qos;
	}

	err = fdice_iidc_notify_event(fdev, &event);
	if (err)
		dev_dbg(dev, "DCB: RDMA %s TC notify returned %d\n",
			pre ? "PRE" : "POST", err);

	return err;
}

/**
 * fdice_dcb_update_qos - Refresh IIDC QoS snapshot from active DCB config
 * @fdev: FrankenDrICE device
 *
 * Copies the local DCB configuration into the RDMA QoS snapshot
 * structure within the IIDC core. The RDMA auxiliary driver reads
 * this snapshot to determine QP-to-TC mapping, bandwidth weights,
 * and PFC state.
 *
 * Must be called after any DCB configuration change that affects
 * the traffic class layout visible to RDMA.
 */
void fdice_dcb_update_qos(struct fdice_dev *fdev)
{
	struct fdice_rdma_qos *qos;
	const struct fdice_dcb_cfg *cfg;
	int i;

	if (!fdev->rdma.iidc_core)
		return;

	qos = &fdev->rdma.iidc_core->qos;
	cfg = &fdev->dcb.local_cfg;

	qos->num_tc = cfg->num_tc;
	qos->pfc_enabled = cfg->pfc.pfc_en;
	qos->dscp_mode = (fdev->dcb.trust == FDICE_TRUST_DSCP);

	for (i = 0; i < FDICE_DCB_MAX_UP; i++)
		qos->up_to_tc_map[i] = cfg->ets.prio_tc[i];

	for (i = 0; i < FDICE_DCB_MAX_TC; i++)
		qos->tc_to_bw_map[i] = cfg->ets.tc_bw[i];

	/*
	 * DSCP map is not populated here — it would be filled by the
	 * dcb_app framework callbacks (setapp/delapp) in a future
	 * DSCP implementation phase.
	 */
}
