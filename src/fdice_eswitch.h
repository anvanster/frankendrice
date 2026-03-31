// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_ESWITCH_H_
#define _FDICE_ESWITCH_H_

/**
 * DOC: FrankenDrICE eSwitch Header
 *
 * Two-mode embedded switch (legacy + switchdev) with mlx5-inspired vport
 * abstraction and pluggable representor ops. The eSwitch manages VF lifecycle,
 * port representors, bridge FDB offload, and TC flower offload on representors.
 *
 * Design: mlx5-inspired vport xarray and rep_ops above the AQ boundary,
 * using E810 switch rules and metadata_dst TX steering below.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.10)
 */

#include <linux/types.h>
#include <linux/xarray.h>
#include <linux/mutex.h>
#include <linux/rhashtable.h>
#include <linux/netdevice.h>
#include <net/devlink.h>

/* Forward declarations */
struct fdice_dev;
struct fdice_repr;
struct fdice_esw_bridge;

/* -----------------------------------------------------------------------
 * eSwitch Mode and VF State Enums
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_eswitch_mode - eSwitch operating mode
 * @FDICE_ESWITCH_LEGACY: Traditional SR-IOV with MAC/VLAN filtering
 * @FDICE_ESWITCH_SWITCHDEV: Programmable switchdev with port representors
 *
 * Mode transitions are performed via devlink eswitch mode set. In legacy
 * mode, VFs are managed through ndo_set_vf_* ops. In switchdev mode,
 * each VF gets a port representor netdev and traffic steering is done
 * through TC flower on representors and bridge FDB offload.
 */
enum fdice_eswitch_mode {
	FDICE_ESWITCH_LEGACY,
	FDICE_ESWITCH_SWITCHDEV,
};

/**
 * enum fdice_vf_state - VF lifecycle state machine
 * @FDICE_VF_STATE_DISABLED: VF not initialized or fully torn down
 * @FDICE_VF_STATE_INITIALIZING: VF resources being allocated
 * @FDICE_VF_STATE_ACTIVE: VF operational and ready for traffic
 * @FDICE_VF_STATE_QS_ENABLED: VF queues enabled (data path active)
 * @FDICE_VF_STATE_RESETTING: VF undergoing reset (VFLR or SW reset)
 *
 * Transition diagram:
 *   DISABLED -> INITIALIZING -> ACTIVE -> QS_ENABLED -> RESETTING -> DISABLED
 *   Any state -> RESETTING (on VF reset)
 *   RESETTING -> INITIALIZING (on recovery)
 */
enum fdice_vf_state {
	FDICE_VF_STATE_DISABLED,
	FDICE_VF_STATE_INITIALIZING,
	FDICE_VF_STATE_ACTIVE,
	FDICE_VF_STATE_QS_ENABLED,
	FDICE_VF_STATE_RESETTING,
};

/* -----------------------------------------------------------------------
 * Representor Ops
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_rep_type - Representor protocol types
 * @FDICE_REP_ETH: Ethernet netdev representor
 * @FDICE_NUM_REP_TYPES: sentinel for rep_ops array sizing
 *
 * Currently only ETH is implemented. IB (RDMA) representor type is
 * reserved for future irdma integration via the same pluggable ops.
 */
enum fdice_rep_type {
	FDICE_REP_ETH,
	FDICE_NUM_REP_TYPES,
};

/**
 * enum fdice_rep_event - Representor event types
 * @FDICE_REP_EVENT_VF_RESET: VF undergoing reset
 * @FDICE_REP_EVENT_VF_LINK_CHANGE: VF link state changed
 *
 * Events dispatched to rep_ops callbacks during VF lifecycle changes.
 */
enum fdice_rep_event {
	FDICE_REP_EVENT_VF_RESET,
	FDICE_REP_EVENT_VF_LINK_CHANGE,
};

/**
 * struct fdice_rep_ops - Pluggable per-type representor operations
 * @load: Create the protocol-specific representor (netdev for ETH)
 * @unload: Destroy the protocol-specific representor
 * @get_proto_dev: Return the protocol device (net_device for ETH)
 * @event: Handle switchdev events for this representor
 *
 * Modeled after mlx5's mlx5_eswitch_rep_ops. The ETH type creates a
 * full netdev representor with TC flower hooks. Future IB type would
 * create an IB port representor.
 */
struct fdice_rep_ops {
	int (*load)(struct fdice_repr *repr);
	void (*unload)(struct fdice_repr *repr);
	struct net_device *(*get_proto_dev)(struct fdice_repr *repr);
	void (*event)(struct fdice_repr *repr, enum fdice_rep_event event);
};

/* -----------------------------------------------------------------------
 * Vport and eSwitch Structures
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_vport - Per-VF/uplink virtual port context
 * @vsi_id: VSI index assigned to this vport
 * @vf_id: VF number (0 for uplink vport)
 * @state: current VF lifecycle state
 * @repr: port representor (non-NULL in switchdev mode)
 * @is_uplink: true if this is the PF uplink vport
 * @mac: default MAC address for this vport
 *
 * Provides a unified abstraction for PF uplink and VF vports,
 * following mlx5's mlx5_vport pattern. Each vport may have a
 * port representor in switchdev mode.
 */
struct fdice_vport {
	u16				vsi_id;
	u16				vf_id;
	enum fdice_vf_state		state;
	struct fdice_repr		*repr;
	bool				is_uplink;
	u8				mac[ETH_ALEN];
};

/**
 * struct fdice_eswitch - Embedded switch state
 * @fdev: backpointer to core device
 * @mode: current operating mode (legacy or switchdev)
 * @vports: xarray of fdice_vport keyed by vport index
 * @uplink_vport: uplink (PF) vport entry
 * @num_vfs: number of VFs currently attached
 * @is_running: true when switchdev environment is fully active
 * @mode_lock: serializes mode transitions and VF attach/detach
 * @bridge: bridge offload state (non-NULL when bridge active)
 * @rep_ops: pluggable representor ops per type
 *
 * The eSwitch manages VF lifecycle and traffic steering in both
 * legacy and switchdev modes. In switchdev mode, it creates port
 * representors and enables bridge FDB offload and TC flower on
 * representors. Lazy initialization: switchdev environment is set
 * up on first VF attach, not during mode_set.
 */
struct fdice_eswitch {
	struct fdice_dev		*fdev;
	enum fdice_eswitch_mode		mode;
	struct xarray			vports;
	struct fdice_vport		*uplink_vport;
	u16				num_vfs;
	bool				is_running;
	struct mutex			mode_lock;

	/* Bridge offload state */
	struct fdice_esw_bridge		*bridge;

	/* Pluggable representor ops (one per rep type) */
	const struct fdice_rep_ops	*rep_ops[FDICE_NUM_REP_TYPES];
};

/* -----------------------------------------------------------------------
 * eSwitch Lifecycle API
 * ----------------------------------------------------------------------- */

int fdice_esw_init(struct fdice_dev *fdev);
void fdice_esw_cleanup(struct fdice_dev *fdev);

int fdice_esw_mode_set(struct fdice_dev *fdev, u16 mode);
int fdice_esw_mode_get(struct fdice_dev *fdev, u16 *mode);

int fdice_esw_vf_attach(struct fdice_eswitch *esw, u16 vf_id, u16 vsi_id);
void fdice_esw_vf_detach(struct fdice_eswitch *esw, u16 vf_id);

void fdice_esw_register_rep_ops(struct fdice_eswitch *esw,
				enum fdice_rep_type type,
				const struct fdice_rep_ops *ops);

/* -----------------------------------------------------------------------
 * Bridge Offload API (fdice_eswitch_br.c)
 * ----------------------------------------------------------------------- */

int fdice_esw_bridge_init(struct fdice_eswitch *esw);
void fdice_esw_bridge_cleanup(struct fdice_eswitch *esw);
int fdice_esw_br_fdb_event(struct fdice_eswitch *esw,
			   const u8 *mac, u16 vid, u16 vsi_id, bool adding);

/* -----------------------------------------------------------------------
 * Inline Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_is_switchdev - Check if eswitch is in switchdev mode
 * @esw: eswitch pointer (may be NULL)
 *
 * Return: true if eswitch exists and is in switchdev mode
 */
static inline bool fdice_esw_is_switchdev(const struct fdice_eswitch *esw)
{
	return esw && esw->mode == FDICE_ESWITCH_SWITCHDEV;
}

/**
 * fdice_esw_is_running - Check if switchdev environment is active
 * @esw: eswitch pointer (may be NULL)
 *
 * Return: true if eswitch is running in switchdev mode
 */
static inline bool fdice_esw_is_running(const struct fdice_eswitch *esw)
{
	return esw && esw->is_running;
}

#endif /* _FDICE_ESWITCH_H_ */
