// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_REPR_H_
#define _FDICE_REPR_H_

/**
 * DOC: FrankenDrICE Port Representor Header
 *
 * Port representor netdevs provide the switchdev control plane interface
 * for VF traffic steering. Each VF in switchdev mode gets a representor
 * netdev on which TC flower rules and bridge FDB entries are programmed.
 *
 * TX on a representor uses metadata_dst with METADATA_HW_PORT_MUX to
 * steer packets through the uplink VSI to the target VF VSI.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.10)
 */

#include <linux/types.h>
#include <linux/netdevice.h>
#include <net/dst_metadata.h>

/* Forward declarations */
struct fdice_dev;
struct fdice_vport;
struct fdice_eswitch;

/**
 * struct fdice_repr - Port representor context
 * @netdev: representor network device
 * @vport: backpointer to the associated vport
 * @fdev: backpointer to core device
 * @metadata_dst: metadata destination for TX steering
 * @vf_id: VF number this representor maps to
 * @vsi_id: VSI number for TX descriptor steering
 *
 * Each representor wraps a netdev that acts as the switchdev control plane
 * endpoint for one VF. Traffic received on a VF is delivered to software
 * through the representor (slow path). Traffic sent on the representor
 * is steered to the VF VSI via metadata_dst encoding in the uplink TX path.
 */
struct fdice_repr {
	struct net_device		*netdev;
	struct fdice_vport		*vport;
	struct fdice_dev		*fdev;
	struct metadata_dst		*metadata_dst;
	u16				vf_id;
	u16				vsi_id;
};

/* -----------------------------------------------------------------------
 * Representor Lifecycle API
 * ----------------------------------------------------------------------- */

struct fdice_repr *fdice_repr_create(struct fdice_eswitch *esw,
				     struct fdice_vport *vport);
void fdice_repr_destroy(struct fdice_repr *repr);

/* -----------------------------------------------------------------------
 * Representor Netdev Ops
 * ----------------------------------------------------------------------- */

extern const struct net_device_ops fdice_repr_netdev_ops;

/* -----------------------------------------------------------------------
 * Representor TC Flower API (fdice_repr_tc.c)
 * ----------------------------------------------------------------------- */

struct tc_cls_flower_offload;

int fdice_repr_setup_tc(struct net_device *dev, enum tc_setup_type type,
			void *type_data);

/* -----------------------------------------------------------------------
 * Inline Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_repr_priv - Get repr context from representor netdev
 * @netdev: representor network device
 *
 * Return: pointer to fdice_repr stored in netdev_priv
 */
static inline struct fdice_repr *fdice_repr_priv(struct net_device *netdev)
{
	return netdev_priv(netdev);
}

/**
 * fdice_is_repr_netdev - Check if a netdev is a representor
 * @netdev: network device to check
 *
 * Return: true if the netdev uses representor netdev_ops
 */
static inline bool fdice_is_repr_netdev(const struct net_device *netdev)
{
	return netdev->netdev_ops == &fdice_repr_netdev_ops;
}

#endif /* _FDICE_REPR_H_ */
