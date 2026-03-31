// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_DCB_H_
#define _FDICE_DCB_H_

/**
 * DOC: FrankenDrICE DCB/QoS Header
 *
 * Data Center Bridging support for the E810 — ETS bandwidth allocation,
 * Priority Flow Control, DCBX negotiation, and trust-state management.
 *
 * Hybrid architecture informed by both ice and mlx5 patterns:
 * - ice-inspired local/desired/remote configuration triple for clean state
 *   separation across DCBX negotiation rounds.
 * - mlx5-inspired trust-state model (PCP vs DSCP) for flexible per-port
 *   QoS classification without a heavyweight port-wide mode switch.
 * - E810 AQ LLDP MIB interface for hardware programming (below-AQ boundary).
 *
 * Architecture reference: docs/exploration/10-dcb-qos.md
 */

#include <linux/types.h>
#include <linux/dcbnl.h>
#include <net/dcbnl.h>

/* Forward declarations */
struct fdice_dev;
struct net_device;

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

/** Maximum traffic classes supported by E810 hardware */
#define FDICE_DCB_MAX_TC		8

/** Maximum user priorities (IEEE 802.1p) */
#define FDICE_DCB_MAX_UP		8

/** Required ETS bandwidth sum for non-strict TCs */
#define FDICE_DCB_BW_SUM		100

/** Maximum PFC-capable priorities on E810 */
#define FDICE_DCB_PFC_CAP		8

/* -----------------------------------------------------------------------
 * DCBX Mode
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_dcbx_mode - DCBX operational mode
 * @FDICE_DCBX_MODE_IEEE: IEEE 802.1Qaz DCBX (default)
 * @FDICE_DCBX_MODE_CEE: Converged Enhanced Ethernet (legacy) DCBX
 *
 * Only one mode is active at a time (mutually exclusive, ice pattern).
 * Mode selection determines which TLV set is exchanged with the peer
 * and which dcbnl callback family is authoritative.
 */
enum fdice_dcbx_mode {
	FDICE_DCBX_MODE_IEEE = 0,
	FDICE_DCBX_MODE_CEE,
};

/* -----------------------------------------------------------------------
 * Trust State
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_trust_state - QoS classification trust source
 * @FDICE_TRUST_PCP: Classify by VLAN PCP (L2, default)
 * @FDICE_TRUST_DSCP: Classify by IP DSCP (L3)
 *
 * mlx5-inspired trust-state model: the driver selects whether hardware
 * inspects L2 VLAN priority or L3 DSCP to determine the traffic class.
 * Switching trust state triggers a channel rebuild so the hardware
 * classification pipeline is reconfigured atomically.
 */
enum fdice_trust_state {
	FDICE_TRUST_PCP = 0,
	FDICE_TRUST_DSCP,
};

/* -----------------------------------------------------------------------
 * DCB Configuration Structures
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_dcb_ets_cfg - ETS (Enhanced Transmission Selection) config
 * @tc_bw: Per-TC bandwidth weight (percent, sum of ETS TCs = 100)
 * @tc_tsa: Per-TC Transmission Selection Algorithm
 *          (IEEE_8021QAZ_TSA_STRICT, IEEE_8021QAZ_TSA_ETS, etc.)
 * @prio_tc: User-priority to traffic-class mapping
 * @max_tcs: Maximum TCs reported by hardware capability
 * @willing: Willing bit for DCBX negotiation
 *
 * TSA state is cached in the driver because the E810 firmware stores
 * bandwidth allocations via LLDP MIB but does not natively report TSA
 * type per TC (same caching rationale as mlx5).
 */
struct fdice_dcb_ets_cfg {
	u8	tc_bw[FDICE_DCB_MAX_TC];
	u8	tc_tsa[FDICE_DCB_MAX_TC];
	u8	prio_tc[FDICE_DCB_MAX_UP];
	u8	max_tcs;
	u8	willing;
};

/**
 * struct fdice_dcb_pfc_cfg - Priority Flow Control configuration
 * @pfc_en: Per-priority PFC enable bitmask (bit N = priority N)
 * @pfc_cap: Number of PFC-capable traffic classes (hardware limit)
 * @mbc: MACsec bypass capability
 * @willing: Willing bit for DCBX negotiation
 *
 * PFC enable is a simple bitmask programmed into the E810 via the
 * LLDP MIB AQ interface. Buffer allocation for PFC headroom is
 * firmware-managed on E810 (unlike mlx5 SW-owned buffers).
 */
struct fdice_dcb_pfc_cfg {
	u8	pfc_en;
	u8	pfc_cap;
	u8	mbc;
	u8	willing;
};

/**
 * struct fdice_dcb_cfg - Complete DCB configuration snapshot
 * @ets: ETS bandwidth and scheduling configuration
 * @pfc: Priority Flow Control configuration
 * @num_tc: Number of active traffic classes in this config
 *
 * One instance exists for each role in the DCBX negotiation triple
 * (local, desired, remote). The 'local' config represents the
 * currently active hardware state; 'desired' holds pending changes
 * from the host; 'remote' reflects the peer's advertisement.
 */
struct fdice_dcb_cfg {
	struct fdice_dcb_ets_cfg	ets;
	struct fdice_dcb_pfc_cfg	pfc;
	u8				num_tc;
};

/* -----------------------------------------------------------------------
 * Top-Level DCB State
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_dcb - Per-device DCB subsystem state
 * @local_cfg: Active hardware configuration
 * @desired_cfg: Pending configuration (host-side changes staged here)
 * @remote_cfg: Peer's advertised configuration (from LLDP)
 * @dcbx_mode: Current DCBX negotiation mode (IEEE or CEE)
 * @dcbx_cap: DCBX capability flags (DCB_CAP_DCBX_VER_IEEE, etc.)
 * @trust: QoS classification trust source (PCP or DSCP)
 * @fw_lldp_agent: True when the FW LLDP agent is active (read-only mode)
 * @dcb_capable: Hardware reports DCB capability
 * @dcb_enabled: DCB is active (more than one TC configured)
 *
 * Embedded in fdice_dev. The configuration triple pattern follows
 * ice's state model for clean DCBX negotiation semantics while the
 * trust-state field follows mlx5's per-port classification model.
 */
struct fdice_dcb {
	struct fdice_dcb_cfg		local_cfg;
	struct fdice_dcb_cfg		desired_cfg;
	struct fdice_dcb_cfg		remote_cfg;

	enum fdice_dcbx_mode		dcbx_mode;
	u8				dcbx_cap;
	enum fdice_trust_state		trust;

	bool				fw_lldp_agent;
	bool				dcb_capable;
	bool				dcb_enabled;
};

/* -----------------------------------------------------------------------
 * DCB Core Functions (fdice_dcb.c)
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_init - Initialize DCB subsystem and register dcbnl ops
 * @fdev: FrankenDrICE device
 *
 * Detects DCB capability, initializes the configuration triple with
 * defaults (single TC, no PFC), queries FW LLDP agent state, and
 * registers the dcbnl_rtnl_ops on the network device.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_dcb_init(struct fdice_dev *fdev);

/**
 * fdice_dcb_cleanup - Tear down DCB subsystem
 * @fdev: FrankenDrICE device
 *
 * Safe to call even if fdice_dcb_init() was not called or failed.
 */
void fdice_dcb_cleanup(struct fdice_dev *fdev);

/**
 * fdice_dcb_apply_config - Push desired DCB config to hardware
 * @fdev: FrankenDrICE device
 *
 * Programs the E810 ETS and PFC configuration via LLDP MIB AQ commands,
 * notifies the RDMA subsystem (BEFORE/AFTER TC change), and triggers
 * a channel rebuild when the TC count changes.
 *
 * Must not be called when fw_lldp_agent is active.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_dcb_apply_config(struct fdice_dev *fdev);

/**
 * fdice_dcb_set_trust - Change QoS classification trust state
 * @fdev: FrankenDrICE device
 * @trust: New trust state (PCP or DSCP)
 *
 * Switching trust triggers a channel rebuild so the hardware packet
 * classification pipeline is reconfigured.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_dcb_set_trust(struct fdice_dev *fdev,
			enum fdice_trust_state trust);

/* dcbnl_rtnl_ops -- registered on netdev */
extern const struct dcbnl_rtnl_ops fdice_dcbnl_ops;

/* -----------------------------------------------------------------------
 * DCB Library Functions (fdice_dcb_lib.c)
 * ----------------------------------------------------------------------- */

/**
 * fdice_dcb_validate_config - Validate a DCB configuration
 * @cfg: Configuration to validate
 *
 * Checks: TC count <= hardware max, ETS bandwidth sums to 100 for
 * non-strict TCs, TCs are contiguous starting from TC0, and priority
 * mappings reference valid TCs.
 *
 * Return: 0 if valid, negative errno on validation failure
 */
int fdice_dcb_validate_config(const struct fdice_dcb_cfg *cfg);

/**
 * fdice_dcb_get_num_tc - Determine active TC count from config
 * @cfg: DCB configuration to inspect
 *
 * Scans the priority-to-TC map to find the highest TC in use,
 * then returns that value plus one.
 *
 * Return: Number of active traffic classes (1..FDICE_DCB_MAX_TC)
 */
u8 fdice_dcb_get_num_tc(const struct fdice_dcb_cfg *cfg);

/**
 * fdice_dcb_rebuild_channels - Rebuild channels for TC reconfiguration
 * @fdev: FrankenDrICE device
 *
 * Performs the close-reconfigure-reopen sequence: disables channels,
 * updates the TC queue mapping on the netdev, and re-enables channels
 * with the new layout.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_dcb_rebuild_channels(struct fdice_dev *fdev);

/**
 * fdice_dcb_notify_rdma - Send TC change notifications to RDMA driver
 * @fdev: FrankenDrICE device
 * @pre: true for PRE_TC_CHANGE, false for POST_TC_CHANGE
 *
 * Dispatches IIDC events to the RDMA auxiliary driver. For POST events,
 * the updated QoS snapshot is delivered so the RDMA driver can remap
 * QPs to the new TC layout.
 *
 * Return: 0 on success, negative errno on failure (logged, not fatal)
 */
int fdice_dcb_notify_rdma(struct fdice_dev *fdev, bool pre);

/**
 * fdice_dcb_update_qos - Refresh IIDC QoS snapshot from active DCB config
 * @fdev: FrankenDrICE device
 *
 * Copies the local DCB configuration into the RDMA QoS snapshot
 * structure so the RDMA auxiliary driver has a consistent view
 * of the current traffic class layout.
 */
void fdice_dcb_update_qos(struct fdice_dev *fdev);

#endif /* _FDICE_DCB_H_ */
