// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE Port Representor Lifecycle
 *
 * Implements port representor netdevs for the eSwitch switchdev mode.
 * Each VF gets a representor that provides the switchdev control plane
 * interface. TC flower rules and bridge FDB entries are programmed on
 * representor netdevs and offloaded to hardware.
 *
 * TX on a representor uses metadata_dst with METADATA_HW_PORT_MUX to
 * steer packets through the PF uplink VSI to the target VF VSI. This
 * is a slow-path model -- all representor traffic traverses the PF.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.10)
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/ethtool.h>
#include <net/dst_metadata.h>

#include "fdice_dev.h"
#include "fdice_eswitch.h"
#include "fdice_repr.h"

/* -----------------------------------------------------------------------
 * Representor Netdev Operations
 * ----------------------------------------------------------------------- */

/**
 * fdice_repr_open - Open (bring up) a representor netdev
 * @netdev: representor network device
 *
 * No hardware configuration needed -- representors rely on the uplink
 * VSI for actual data path. Simply marks the netdev as running.
 *
 * Return: 0 always
 */
static int fdice_repr_open(struct net_device *netdev)
{
	netif_carrier_on(netdev);
	netif_tx_start_all_queues(netdev);
	return 0;
}

/**
 * fdice_repr_stop - Stop (bring down) a representor netdev
 * @netdev: representor network device
 *
 * Stops TX queues and drops carrier.
 *
 * Return: 0 always
 */
static int fdice_repr_stop(struct net_device *netdev)
{
	netif_carrier_off(netdev);
	netif_tx_stop_all_queues(netdev);
	return 0;
}

/**
 * fdice_repr_xmit - Transmit a packet on a representor netdev
 * @skb: socket buffer to transmit
 * @netdev: representor network device
 *
 * Attaches metadata_dst to the SKB carrying the target VF's VSI number,
 * then redirects the packet to the uplink netdev for transmission. The
 * uplink TX path reads the metadata to steer the packet to the correct
 * VF VSI via the E810 switch context descriptor.
 *
 * This is the slow-path TX model: all representor traffic goes through
 * the PF's uplink VSI. Future hardware support could enable direct
 * register-based steering (like mlx5's reg_c0).
 *
 * Return: NETDEV_TX_OK always (drops on error)
 */
static netdev_tx_t fdice_repr_xmit(struct sk_buff *skb,
				    struct net_device *netdev)
{
	struct fdice_repr *repr = fdice_repr_priv(netdev);
	struct net_device *uplink_netdev;

	if (!repr->fdev || !repr->fdev->esw) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	uplink_netdev = repr->fdev->priv.netdev;

	if (!uplink_netdev || !netif_running(uplink_netdev)) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	/* Attach metadata_dst for TX steering to VF VSI */
	if (repr->metadata_dst)
		skb_dst_set(skb, &repr->metadata_dst->dst);

	skb->dev = uplink_netdev;
	dev_queue_xmit(skb);

	return NETDEV_TX_OK;
}

/**
 * fdice_repr_get_phys_port_name - Return representor port name
 * @netdev: representor network device
 * @buf: output buffer for the port name string
 * @len: buffer length
 *
 * Returns a port name in the format "pf0vfN" where N is the VF number.
 * Used by udev and devlink to identify representor ports.
 *
 * Return: 0 on success, -EOPNOTSUPP if not a representor
 */
static int fdice_repr_get_phys_port_name(struct net_device *netdev,
					 char *buf, size_t len)
{
	struct fdice_repr *repr = fdice_repr_priv(netdev);
	int written;

	written = snprintf(buf, len, "pf0vf%u", repr->vf_id);
	if (written >= len)
		return -EOPNOTSUPP;

	return 0;
}

/**
 * fdice_repr_get_stats64 - Get representor netdev statistics
 * @netdev: representor network device
 * @stats: output structure for statistics
 *
 * Returns zeroed statistics for now. Full per-representor counters
 * require per-VSI statistics collection from E810 hardware.
 */
static void fdice_repr_get_stats64(struct net_device *netdev,
				   struct rtnl_link_stats64 *stats)
{
	/* Per-representor statistics are a future enhancement */
	memset(stats, 0, sizeof(*stats));
}

/**
 * fdice_repr_has_offload_stats - Check if representor has offload stats
 * @netdev: representor network device
 * @attr_id: attribute ID for the stats type
 *
 * Return: false (offload stats not yet implemented)
 */
static bool fdice_repr_has_offload_stats(const struct net_device *netdev,
					 int attr_id)
{
	return false;
}

const struct net_device_ops fdice_repr_netdev_ops = {
	.ndo_open		= fdice_repr_open,
	.ndo_stop		= fdice_repr_stop,
	.ndo_start_xmit		= fdice_repr_xmit,
	.ndo_get_phys_port_name	= fdice_repr_get_phys_port_name,
	.ndo_get_stats64	= fdice_repr_get_stats64,
	.ndo_has_offload_stats	= fdice_repr_has_offload_stats,
	.ndo_setup_tc		= fdice_repr_setup_tc,
};

/* -----------------------------------------------------------------------
 * Representor Lifecycle
 * ----------------------------------------------------------------------- */

/**
 * fdice_repr_create - Create a port representor for a vport
 * @esw: eswitch instance
 * @vport: vport to create a representor for
 *
 * Allocates a netdev for the representor, configures metadata_dst for
 * TX steering, sets up netdev_ops (including TC flower hooks), and
 * registers the netdev. The representor name is "fdice_reprN" where
 * N is the VF number.
 *
 * Return: pointer to created repr on success, ERR_PTR on failure
 */
struct fdice_repr *fdice_repr_create(struct fdice_eswitch *esw,
				     struct fdice_vport *vport)
{
	struct fdice_dev *fdev = esw->fdev;
	struct net_device *netdev;
	struct fdice_repr *repr;
	struct metadata_dst *md_dst;
	int err;

	netdev = alloc_etherdev(sizeof(struct fdice_repr));
	if (!netdev)
		return ERR_PTR(-ENOMEM);

	repr = netdev_priv(netdev);
	repr->netdev = netdev;
	repr->vport = vport;
	repr->fdev = fdev;
	repr->vf_id = vport->vf_id;
	repr->vsi_id = vport->vsi_id;

	/* Configure metadata_dst for TX steering to VF VSI */
	md_dst = metadata_dst_alloc(0, METADATA_HW_PORT_MUX, GFP_KERNEL);
	if (IS_ERR(md_dst)) {
		err = PTR_ERR(md_dst);
		goto err_free_netdev;
	}
	md_dst->u.port_info.port_id = vport->vsi_id;
	repr->metadata_dst = md_dst;

	/* Set up netdev properties */
	netdev->netdev_ops = &fdice_repr_netdev_ops;
	SET_NETDEV_DEV(netdev, &fdev->pdev->dev);

	/* Generate a unique MAC for the representor */
	eth_hw_addr_random(netdev);

	/* Name the representor netdev */
	snprintf(netdev->name, IFNAMSIZ, "fdice_repr%u", vport->vf_id);

	netdev->features |= NETIF_F_HW_TC;
	netdev->hw_features |= NETIF_F_HW_TC;

	err = register_netdev(netdev);
	if (err)
		goto err_free_md;

	return repr;

err_free_md:
	metadata_dst_free(md_dst);
err_free_netdev:
	free_netdev(netdev);
	return ERR_PTR(err);
}

/**
 * fdice_repr_destroy - Destroy a port representor
 * @repr: representor to destroy
 *
 * Unregisters the netdev, frees metadata_dst, and releases all
 * representor resources. The vport's repr pointer must be cleared
 * by the caller.
 */
void fdice_repr_destroy(struct fdice_repr *repr)
{
	if (!repr)
		return;

	if (repr->netdev->reg_state == NETREG_REGISTERED)
		unregister_netdev(repr->netdev);

	if (repr->metadata_dst)
		metadata_dst_free(repr->metadata_dst);

	/* free_netdev also frees the repr (netdev_priv) */
	free_netdev(repr->netdev);
}
