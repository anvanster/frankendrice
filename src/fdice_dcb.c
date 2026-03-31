// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE DCB/QoS Implementation
 *
 * IEEE 802.1Qaz dcbnl_rtnl_ops and core DCB lifecycle for the E810.
 *
 * This file implements:
 * - IEEE ETS get/set (bandwidth allocation, TSA, priority mapping)
 * - IEEE PFC get/set (per-priority flow control)
 * - DCBX mode get/set with FW LLDP agent awareness
 * - Trust-state management (PCP vs DSCP)
 * - Hardware programming via AQ LLDP MIB commands
 *
 * The configuration triple (local/desired/remote) follows ice's state
 * model. The trust-state approach follows mlx5's per-port classification.
 * When the FW LLDP agent is active, all set operations are blocked and
 * the interface operates in read-only mode.
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
 * Internal Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_is_fw_managed - Check if FW LLDP agent controls DCB
 * @fdev: FrankenDrICE device
 *
 * When the firmware LLDP agent is active, the host cannot modify DCB
 * configuration. All set operations must check this before proceeding.
 *
 * Return: true if firmware manages DCB (read-only mode)
 */
static bool fdice_dcb_is_fw_managed(struct fdice_dev *fdev)
{
	return fdev->dcb.fw_lldp_agent;
}

/**
 * fdice_dcb_is_host_mode - Check if host controls DCBX
 * @fdev: FrankenDrICE device
 *
 * Host mode is required for all configuration changes. This combines
 * the DCBX capability check with the FW agent state.
 *
 * Return: true if the host has DCB control authority
 */
static bool fdice_dcb_is_host_mode(struct fdice_dev *fdev)
{
	if (fdice_dcb_is_fw_managed(fdev))
		return false;
	return !!(fdev->dcb.dcbx_cap & DCB_CAP_DCBX_HOST);
}

/**
 * fdice_dcb_cfg_to_ets - Export DCB config to IEEE ETS structure
 * @cfg: source DCB configuration
 * @ets: destination IEEE 802.1Qaz ETS structure
 *
 * Translates the internal fdice ETS representation into the standard
 * ieee_ets structure used by the dcbnl interface.
 */
static void fdice_dcb_cfg_to_ets(const struct fdice_dcb_cfg *cfg,
				 struct ieee_ets *ets)
{
	int i;

	memset(ets, 0, sizeof(*ets));
	ets->ets_cap = cfg->ets.max_tcs;
	ets->willing = cfg->ets.willing;

	for (i = 0; i < FDICE_DCB_MAX_UP; i++)
		ets->prio_tc[i] = cfg->ets.prio_tc[i];

	for (i = 0; i < FDICE_DCB_MAX_TC; i++) {
		ets->tc_tx_bw[i] = cfg->ets.tc_bw[i];
		ets->tc_tsa[i] = cfg->ets.tc_tsa[i];
	}
}

/**
 * fdice_dcb_ets_to_cfg - Import IEEE ETS structure into DCB config
 * @ets: source IEEE 802.1Qaz ETS structure
 * @cfg: destination DCB configuration
 *
 * Translates the standard ieee_ets structure into the internal
 * fdice ETS representation. Preserves hardware limits (max_tcs).
 */
static void fdice_dcb_ets_to_cfg(const struct ieee_ets *ets,
				 struct fdice_dcb_cfg *cfg)
{
	int i;

	cfg->ets.willing = ets->willing;

	for (i = 0; i < FDICE_DCB_MAX_UP; i++)
		cfg->ets.prio_tc[i] = ets->prio_tc[i];

	for (i = 0; i < FDICE_DCB_MAX_TC; i++) {
		cfg->ets.tc_bw[i] = ets->tc_tx_bw[i];
		cfg->ets.tc_tsa[i] = ets->tc_tsa[i];
	}
}

/**
 * fdice_dcb_cfg_to_pfc - Export DCB config to IEEE PFC structure
 * @cfg: source DCB configuration
 * @pfc: destination IEEE 802.1Qaz PFC structure
 */
static void fdice_dcb_cfg_to_pfc(const struct fdice_dcb_cfg *cfg,
				 struct ieee_pfc *pfc)
{
	memset(pfc, 0, sizeof(*pfc));
	pfc->pfc_cap = cfg->pfc.pfc_cap;
	pfc->pfc_en = cfg->pfc.pfc_en;
	pfc->mbc = cfg->pfc.mbc;
}

/* -----------------------------------------------------------------------
 * IEEE dcbnl Callbacks
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcbnl_ieee_getets - Report current ETS configuration
 * @netdev: network device
 * @ets: output IEEE ETS structure
 *
 * Exports the active (local) ETS configuration. Always succeeds.
 *
 * Return: 0
 */
static int fdice_dcbnl_ieee_getets(struct net_device *netdev,
				   struct ieee_ets *ets)
{
	struct fdice_dev *fdev = netdev_priv(netdev);

	fdice_dcb_cfg_to_ets(&fdev->dcb.local_cfg, ets);
	return 0;
}

/**
 * fdice_dcbnl_ieee_setets - Apply new ETS configuration
 * @netdev: network device
 * @ets: desired IEEE ETS structure
 *
 * Validates the proposed ETS config, stages it in the desired config,
 * and applies it to hardware. Blocked when the FW LLDP agent is active.
 *
 * Return: 0 on success, -EPERM if FW-managed, -EINVAL on validation failure
 */
static int fdice_dcbnl_ieee_setets(struct net_device *netdev,
				   struct ieee_ets *ets)
{
	struct fdice_dev *fdev = netdev_priv(netdev);
	struct fdice_dcb_cfg staged;
	int err;

	if (!fdice_dcb_is_host_mode(fdev))
		return -EPERM;

	/* Stage: copy local, overlay ETS changes */
	staged = fdev->dcb.local_cfg;
	fdice_dcb_ets_to_cfg(ets, &staged);
	staged.num_tc = fdice_dcb_get_num_tc(&staged);

	err = fdice_dcb_validate_config(&staged);
	if (err)
		return err;

	fdev->dcb.desired_cfg = staged;

	return fdice_dcb_apply_config(fdev);
}

/**
 * fdice_dcbnl_ieee_getpfc - Report current PFC configuration
 * @netdev: network device
 * @pfc: output IEEE PFC structure
 *
 * Exports the active (local) PFC configuration. Always succeeds.
 *
 * Return: 0
 */
static int fdice_dcbnl_ieee_getpfc(struct net_device *netdev,
				   struct ieee_pfc *pfc)
{
	struct fdice_dev *fdev = netdev_priv(netdev);

	fdice_dcb_cfg_to_pfc(&fdev->dcb.local_cfg, pfc);
	return 0;
}

/**
 * fdice_dcbnl_ieee_setpfc - Apply new PFC configuration
 * @netdev: network device
 * @pfc: desired IEEE PFC structure
 *
 * Stages PFC changes in the desired config and applies to hardware.
 * Blocked when the FW LLDP agent is active.
 *
 * Return: 0 on success, -EPERM if FW-managed
 */
static int fdice_dcbnl_ieee_setpfc(struct net_device *netdev,
				   struct ieee_pfc *pfc)
{
	struct fdice_dev *fdev = netdev_priv(netdev);

	if (!fdice_dcb_is_host_mode(fdev))
		return -EPERM;

	/* Stage PFC changes in desired config */
	fdev->dcb.desired_cfg.pfc.pfc_en = pfc->pfc_en;
	fdev->dcb.desired_cfg.pfc.mbc = pfc->mbc;

	return fdice_dcb_apply_config(fdev);
}

/**
 * fdice_dcbnl_getdcbx - Report DCBX capabilities
 * @netdev: network device
 *
 * Returns the DCBX capability bitmask indicating which DCBX versions
 * the driver supports and whether host mode is active.
 *
 * Return: DCBX capability bitmask
 */
static u8 fdice_dcbnl_getdcbx(struct net_device *netdev)
{
	struct fdice_dev *fdev = netdev_priv(netdev);

	return fdev->dcb.dcbx_cap;
}

/**
 * fdice_dcbnl_setdcbx - Change DCBX mode
 * @netdev: network device
 * @mode: new DCBX capability bitmask
 *
 * Allows switching between IEEE and CEE DCBX modes. The modes are
 * mutually exclusive. Host mode (DCB_CAP_DCBX_HOST) must always be
 * present in the requested mode for the switch to be permitted.
 * LLD_MANAGED mode is not supported.
 *
 * Return: 0 on success, -EINVAL for unsupported mode, -EPERM if FW-managed
 */
static u8 fdice_dcbnl_setdcbx(struct net_device *netdev, u8 mode)
{
	struct fdice_dev *fdev = netdev_priv(netdev);

	/* Must have host control */
	if (!(mode & DCB_CAP_DCBX_HOST))
		return -EINVAL;

	/* LLD managed not supported */
	if (mode & DCB_CAP_DCBX_LLD_MANAGED)
		return -EINVAL;

	/* Must select exactly one version */
	if ((mode & DCB_CAP_DCBX_VER_IEEE) && (mode & DCB_CAP_DCBX_VER_CEE))
		return -EINVAL;

	if (!(mode & (DCB_CAP_DCBX_VER_IEEE | DCB_CAP_DCBX_VER_CEE)))
		return -EINVAL;

	if (fdice_dcb_is_fw_managed(fdev))
		return -EPERM;

	fdev->dcb.dcbx_cap = mode;

	if (mode & DCB_CAP_DCBX_VER_IEEE)
		fdev->dcb.dcbx_mode = FDICE_DCBX_MODE_IEEE;
	else
		fdev->dcb.dcbx_mode = FDICE_DCBX_MODE_CEE;

	return 0;
}

/* -----------------------------------------------------------------------
 * dcbnl_rtnl_ops
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcbnl_ops - DCB netlink operations for FrankenDrICE
 *
 * Registers IEEE ETS/PFC callbacks and DCBX mode management.
 * CEE callbacks are not implemented in this phase but the structure
 * is extensible for future addition.
 */
const struct dcbnl_rtnl_ops fdice_dcbnl_ops = {
	/* IEEE 802.1Qaz */
	.ieee_getets	= fdice_dcbnl_ieee_getets,
	.ieee_setets	= fdice_dcbnl_ieee_setets,
	.ieee_getpfc	= fdice_dcbnl_ieee_getpfc,
	.ieee_setpfc	= fdice_dcbnl_ieee_setpfc,

	/* DCBX mode */
	.getdcbx	= fdice_dcbnl_getdcbx,
	.setdcbx	= fdice_dcbnl_setdcbx,
};

/* -----------------------------------------------------------------------
 * Hardware Programming
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_apply_ets_hw - Program ETS config into E810 via AQ
 * @fdev: FrankenDrICE device
 * @cfg: ETS configuration to program
 *
 * Builds and sends the LLDP MIB AQ command to program ETS parameters.
 * In a production driver this would construct IEEE 802.1Qaz ETS TLVs
 * and issue opcode 0x0A08 (Set LLDP MIB). Currently a validated stub
 * that logs the operation.
 *
 * Return: 0 on success, negative errno on AQ failure
 */
static int fdice_dcb_apply_ets_hw(struct fdice_dev *fdev,
				  const struct fdice_dcb_ets_cfg *cfg)
{
	struct device *dev = &fdev->pdev->dev;

	/*
	 * Production path: construct LLDP MIB TLV with ETS data and
	 * send via AQ Set LLDP MIB (opcode 0x0A08). The E810
	 * firmware parses the TLV and programs the scheduler.
	 *
	 * Stub: log the configuration for development verification.
	 */
	dev_dbg(dev, "DCB: programming ETS — %u TCs, bw=[%u %u %u %u %u %u %u %u]\n",
		fdice_dcb_get_num_tc(&fdev->dcb.desired_cfg),
		cfg->tc_bw[0], cfg->tc_bw[1], cfg->tc_bw[2], cfg->tc_bw[3],
		cfg->tc_bw[4], cfg->tc_bw[5], cfg->tc_bw[6], cfg->tc_bw[7]);

	return 0;
}

/**
 * fdice_dcb_apply_pfc_hw - Program PFC config into E810 via AQ
 * @fdev: FrankenDrICE device
 * @cfg: PFC configuration to program
 *
 * Sends the PFC enable bitmask to firmware via the LLDP MIB interface.
 * Buffer allocation for PFC headroom is firmware-managed on E810.
 *
 * Return: 0 on success, negative errno on AQ failure
 */
static int fdice_dcb_apply_pfc_hw(struct fdice_dev *fdev,
				  const struct fdice_dcb_pfc_cfg *cfg)
{
	struct device *dev = &fdev->pdev->dev;

	/*
	 * Production path: include PFC TLV in the LLDP MIB update.
	 * Stub: log for development verification.
	 */
	dev_dbg(dev, "DCB: programming PFC — enable=0x%02x cap=%u\n",
		cfg->pfc_en, cfg->pfc_cap);

	return 0;
}

/**
 * fdice_dcb_apply_config - Push desired DCB config to hardware
 * @fdev: FrankenDrICE device
 *
 * Orchestrates the full DCB reconfiguration sequence:
 * 1. Notify RDMA (PRE_TC_CHANGE) to quiesce traffic
 * 2. Program ETS and PFC into hardware
 * 3. Rebuild channels if TC count changed
 * 4. Commit desired -> local config
 * 5. Update IIDC QoS snapshot
 * 6. Notify RDMA (POST_TC_CHANGE) with updated QoS
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_dcb_apply_config(struct fdice_dev *fdev)
{
	struct device *dev = &fdev->pdev->dev;
	bool tc_changed;
	int err;

	tc_changed = (fdev->dcb.desired_cfg.num_tc !=
		      fdev->dcb.local_cfg.num_tc);

	/* Step 1: Notify RDMA before TC change */
	if (tc_changed) {
		err = fdice_dcb_notify_rdma(fdev, true);
		if (err)
			dev_warn(dev, "DCB: RDMA pre-TC notify failed: %d\n",
				 err);
	}

	/* Step 2: Program hardware */
	err = fdice_dcb_apply_ets_hw(fdev, &fdev->dcb.desired_cfg.ets);
	if (err) {
		dev_err(dev, "DCB: failed to apply ETS config: %d\n", err);
		goto notify_post;
	}

	err = fdice_dcb_apply_pfc_hw(fdev, &fdev->dcb.desired_cfg.pfc);
	if (err) {
		dev_err(dev, "DCB: failed to apply PFC config: %d\n", err);
		goto notify_post;
	}

	/* Step 3: Rebuild channels if TC count changed */
	if (tc_changed) {
		err = fdice_dcb_rebuild_channels(fdev);
		if (err) {
			dev_err(dev, "DCB: channel rebuild failed: %d\n", err);
			goto notify_post;
		}
	}

	/* Step 4: Commit desired -> local */
	fdev->dcb.local_cfg = fdev->dcb.desired_cfg;
	fdev->dcb.dcb_enabled = (fdev->dcb.local_cfg.num_tc > 1);

	/* Step 5: Update IIDC QoS snapshot */
	fdice_dcb_update_qos(fdev);

notify_post:
	/* Step 6: Notify RDMA after TC change (always, even on error) */
	if (tc_changed)
		fdice_dcb_notify_rdma(fdev, false);

	return err;
}

/* -----------------------------------------------------------------------
 * Trust State Management
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_set_trust - Change QoS classification trust state
 * @fdev: FrankenDrICE device
 * @trust: New trust state (PCP or DSCP)
 *
 * mlx5-inspired trust-state model. Switching trust source requires
 * a channel rebuild because the hardware classification pipeline
 * must be reconfigured to inspect either L2 VLAN PCP or L3 DSCP.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_dcb_set_trust(struct fdice_dev *fdev,
			enum fdice_trust_state trust)
{
	struct device *dev = &fdev->pdev->dev;
	int err;

	if (trust == fdev->dcb.trust)
		return 0;

	if (!fdice_dcb_is_host_mode(fdev))
		return -EPERM;

	dev_dbg(dev, "DCB: switching trust state %s -> %s\n",
		fdev->dcb.trust == FDICE_TRUST_PCP ? "PCP" : "DSCP",
		trust == FDICE_TRUST_PCP ? "PCP" : "DSCP");

	/* Notify RDMA before reconfiguration */
	err = fdice_dcb_notify_rdma(fdev, true);
	if (err)
		dev_warn(dev, "DCB: RDMA pre-trust notify failed: %d\n", err);

	fdev->dcb.trust = trust;

	/* Rebuild channels for new classification pipeline */
	err = fdice_dcb_rebuild_channels(fdev);
	if (err) {
		dev_err(dev, "DCB: channel rebuild for trust change failed: %d\n",
			err);
		/* Revert trust state on failure */
		fdev->dcb.trust = (trust == FDICE_TRUST_PCP) ?
				  FDICE_TRUST_DSCP : FDICE_TRUST_PCP;
	}

	/* Update QoS snapshot with new trust mode */
	fdice_dcb_update_qos(fdev);

	/* Notify RDMA after reconfiguration */
	fdice_dcb_notify_rdma(fdev, false);

	return err;
}

/* -----------------------------------------------------------------------
 * DCB Lifecycle
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_init_defaults - Set default DCB configuration
 * @dcb: DCB state to initialize
 *
 * Configures single-TC default: all priorities map to TC0 with 100%
 * ETS bandwidth, no PFC, IEEE DCBX mode, PCP trust.
 */
static void fdice_dcb_init_defaults(struct fdice_dcb *dcb)
{
	int i;

	memset(&dcb->local_cfg, 0, sizeof(dcb->local_cfg));
	memset(&dcb->desired_cfg, 0, sizeof(dcb->desired_cfg));
	memset(&dcb->remote_cfg, 0, sizeof(dcb->remote_cfg));

	/* Single TC default: all priorities -> TC0 */
	dcb->local_cfg.num_tc = 1;
	dcb->local_cfg.ets.max_tcs = FDICE_DCB_MAX_TC;
	dcb->local_cfg.ets.tc_bw[0] = FDICE_DCB_BW_SUM;
	dcb->local_cfg.ets.tc_tsa[0] = IEEE_8021QAZ_TSA_ETS;
	dcb->local_cfg.pfc.pfc_cap = FDICE_DCB_PFC_CAP;

	for (i = 0; i < FDICE_DCB_MAX_UP; i++)
		dcb->local_cfg.ets.prio_tc[i] = 0;

	/* Mirror local into desired */
	dcb->desired_cfg = dcb->local_cfg;

	/* Default mode: IEEE, host control, PCP trust */
	dcb->dcbx_mode = FDICE_DCBX_MODE_IEEE;
	dcb->dcbx_cap = DCB_CAP_DCBX_VER_IEEE | DCB_CAP_DCBX_HOST;
	dcb->trust = FDICE_TRUST_PCP;
	dcb->dcb_enabled = false;
}

/**
 * fdice_dcb_init - Initialize DCB subsystem and register dcbnl ops
 * @fdev: FrankenDrICE device
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_dcb_init(struct fdice_dev *fdev)
{
	struct device *dev = &fdev->pdev->dev;

	/* Check hardware capability */
	fdev->dcb.dcb_capable = fdev->hw.func_caps.dcb_capable;
	if (!fdev->dcb.dcb_capable) {
		dev_info(dev, "DCB: hardware does not support DCB\n");
		return 0;
	}

	fdice_dcb_init_defaults(&fdev->dcb);

	/*
	 * Query FW LLDP agent state. On E810, the firmware can run its
	 * own LLDP agent. When active, DCB config is read-only from the
	 * host perspective. In a production driver this would issue
	 * Get LLDP Agent Status AQ command; stub as host-controlled.
	 */
	fdev->dcb.fw_lldp_agent = false;

	/* Register dcbnl ops on the network device */
	if (fdev->netdev)
		fdev->netdev->dcbnl_ops = &fdice_dcbnl_ops;

	/* Initialize IIDC QoS snapshot */
	fdice_dcb_update_qos(fdev);

	dev_info(dev, "DCB: initialized — %s mode, %s trust, FW LLDP %s\n",
		 fdev->dcb.dcbx_mode == FDICE_DCBX_MODE_IEEE ? "IEEE" : "CEE",
		 fdev->dcb.trust == FDICE_TRUST_PCP ? "PCP" : "DSCP",
		 fdev->dcb.fw_lldp_agent ? "active" : "inactive");

	return 0;
}

/**
 * fdice_dcb_cleanup - Tear down DCB subsystem
 * @fdev: FrankenDrICE device
 */
void fdice_dcb_cleanup(struct fdice_dev *fdev)
{
	if (!fdev->dcb.dcb_capable)
		return;

	if (fdev->netdev)
		fdev->netdev->dcbnl_ops = NULL;

	memset(&fdev->dcb, 0, sizeof(fdev->dcb));
}
