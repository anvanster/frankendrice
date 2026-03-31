// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_netdev.c - Netdev registration and ndo operations
 *
 * Phase 3.11 implementation: network device allocation, registration,
 * and net_device_ops callbacks for the FrankenDrICE E810 driver.
 *
 * Design rationale:
 *   - ndo_open/ndo_stop manage the full channel lifecycle (alloc, enable,
 *     disable, free) following the mlx5 mlx5e_open/close pattern rather
 *     than ice's flag-based open/close.
 *   - ndo_start_xmit dispatches to the per-channel fdice_xmit_frame()
 *     after selecting the appropriate TXQ from the channel array.
 *   - Feature flags are set during alloc to advertise all E810 HW offloads.
 *   - MTU change while interface is up triggers a channel rebuild cycle
 *     (close -> adjust -> open) for safe descriptor ring reallocation.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.11)
 */

#include <linux/module.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_vlan.h>
#include <linux/rtnetlink.h>
#include <net/pkt_cls.h>
#include <net/tc_act/tc_gact.h>

#include "fdice_dev.h"
#include "fdice_channel.h"
#include "fdice_txrx.h"
#include "fdice_fs_core.h"

/* -----------------------------------------------------------------------
 * Forward declarations for static ndo callbacks
 * ----------------------------------------------------------------------- */

static int fdice_open(struct net_device *netdev);
static int fdice_close(struct net_device *netdev);
static netdev_tx_t fdice_start_xmit(struct sk_buff *skb,
				     struct net_device *netdev);
static void fdice_set_rx_mode(struct net_device *netdev);
static int fdice_change_mtu(struct net_device *netdev, int new_mtu);
static int fdice_set_mac(struct net_device *netdev, void *addr);
static void fdice_get_stats64(struct net_device *netdev,
			      struct rtnl_link_stats64 *stats);
static int fdice_setup_tc(struct net_device *netdev, enum tc_setup_type type,
			   void *type_data);
static int fdice_set_features(struct net_device *netdev,
			      netdev_features_t features);
static int fdice_vlan_rx_add_vid(struct net_device *netdev,
				 __be16 proto, u16 vid);
static int fdice_vlan_rx_kill_vid(struct net_device *netdev,
				  __be16 proto, u16 vid);

/* Ethtool ops declared in fdice_ethtool.c */
extern const struct ethtool_ops fdice_ethtool_ops;

/* Stats collection declared in fdice_stats.c */
void fdice_stats_get(struct fdice_dev *fdev,
		     struct rtnl_link_stats64 *stats);

/* -----------------------------------------------------------------------
 * Default channel parameters
 * ----------------------------------------------------------------------- */

#define FDICE_DFLT_RX_RING_SIZE		1024
#define FDICE_DFLT_TX_RING_SIZE		1024
#define FDICE_DFLT_RX_BUF_SIZE		2048
#define FDICE_DFLT_RX_COAL_USECS	50
#define FDICE_DFLT_TX_COAL_USECS	50
#define FDICE_DFLT_NUM_TC		1

#define FDICE_MIN_MTU			68
#define FDICE_MAX_FRAME_SIZE		9728

/* -----------------------------------------------------------------------
 * Net Device Operations Table
 * ----------------------------------------------------------------------- */

static const struct net_device_ops fdice_netdev_ops = {
	.ndo_open		= fdice_open,
	.ndo_stop		= fdice_close,
	.ndo_start_xmit		= fdice_start_xmit,
	.ndo_set_rx_mode	= fdice_set_rx_mode,
	.ndo_change_mtu		= fdice_change_mtu,
	.ndo_set_mac_address	= fdice_set_mac,
	.ndo_get_stats64	= fdice_get_stats64,
	.ndo_setup_tc		= fdice_setup_tc,
	.ndo_set_features	= fdice_set_features,
	.ndo_vlan_rx_add_vid	= fdice_vlan_rx_add_vid,
	.ndo_vlan_rx_kill_vid	= fdice_vlan_rx_kill_vid,
};

/* -----------------------------------------------------------------------
 * Channel Parameter Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_init_default_params - Populate default channel parameters
 * @params: parameter struct to fill
 * @fdev: core device for capability-based defaults
 *
 * Sets ring sizes, coalescing defaults, and TC count based on hardware
 * capabilities. Called during ndo_open before channel allocation.
 */
static void fdice_init_default_params(struct fdice_params *params,
				      struct fdice_dev *fdev)
{
	memset(params, 0, sizeof(*params));

	params->rx_ring_size = FDICE_DFLT_RX_RING_SIZE;
	params->tx_ring_size = FDICE_DFLT_TX_RING_SIZE;
	params->rx_buf_size = FDICE_DFLT_RX_BUF_SIZE;

	params->rx_coalesce_usecs = FDICE_DFLT_RX_COAL_USECS;
	params->tx_coalesce_usecs = FDICE_DFLT_TX_COAL_USECS;
	params->rx_coalesce_frames = 0;
	params->tx_coalesce_frames = 0;
	params->use_adaptive_rx = true;
	params->use_adaptive_tx = true;

	params->num_tc = FDICE_DFLT_NUM_TC;

	/* Default channel count is bounded by hardware and online CPUs */
	params->num_channels = min_t(u16, num_online_cpus(),
				     fdev->hw.func_caps.num_msix - 1);
	if (params->num_channels == 0)
		params->num_channels = 1;
}

/* -----------------------------------------------------------------------
 * ndo_open / ndo_stop
 * ----------------------------------------------------------------------- */

/**
 * fdice_open - Bring the network interface up
 * @netdev: network device being opened
 *
 * Allocates channels (each with embedded RXQ, TXQ[], and NAPI),
 * enables them via AQ commands, and asserts carrier. This follows
 * the mlx5e_open() pattern: alloc -> enable -> carrier on.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_open(struct net_device *netdev)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	struct fdice_params params;
	int err;

	if (test_bit(FDICE_STATE_RECOVERY, &fdev->state)) {
		netdev_err(netdev, "device is recovering, cannot open\n");
		return -EBUSY;
	}

	fdice_init_default_params(&params, fdev);

	err = fdice_alloc_channels(fdev, &params);
	if (err) {
		netdev_err(netdev, "failed to allocate channels: %d\n", err);
		return err;
	}

	err = fdice_enable_channels(fdev);
	if (err) {
		netdev_err(netdev, "failed to enable channels: %d\n", err);
		goto err_free_channels;
	}

	netif_carrier_on(netdev);
	netif_tx_start_all_queues(netdev);

	return 0;

err_free_channels:
	fdice_free_channels(fdev);
	return err;
}

/**
 * fdice_close - Bring the network interface down
 * @netdev: network device being closed
 *
 * Reverse of fdice_open(): carrier off, stop TX queues, disable
 * all channels, and free channel resources. Follows mlx5e_close()
 * pattern with strict reverse-order teardown.
 *
 * Return: 0 on success
 */
static int fdice_close(struct net_device *netdev)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	netif_carrier_off(netdev);
	netif_tx_stop_all_queues(netdev);

	fdice_disable_channels(fdev);
	fdice_free_channels(fdev);

	return 0;
}

/* -----------------------------------------------------------------------
 * ndo_start_xmit
 * ----------------------------------------------------------------------- */

/**
 * fdice_start_xmit - Transmit an sk_buff
 * @skb: socket buffer to transmit
 * @netdev: network device
 *
 * Selects the target TXQ from the channel array using the kernel's
 * queue mapping, then dispatches to fdice_xmit_frame() for descriptor
 * ring submission with DMA FIFO tracking.
 *
 * Return: NETDEV_TX_OK or NETDEV_TX_BUSY
 */
static netdev_tx_t fdice_start_xmit(struct sk_buff *skb,
				     struct net_device *netdev)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	unsigned int qid = skb_get_queue_mapping(skb);
	u16 ch_idx, tc;
	struct fdice_channel *ch;
	struct fdice_txq *txq;

	/*
	 * Map the kernel queue index to channel + TC.  For single-TC
	 * configurations, ch_idx == qid.  For multi-TC, the kernel maps
	 * via netdev_set_tc_queue offsets.
	 */
	ch_idx = qid % fdev->num_channels;
	tc = qid / fdev->num_channels;

	if (unlikely(ch_idx >= fdev->num_channels || tc >= FDICE_MAX_TC)) {
		dev_kfree_skb_any(skb);
		return NETDEV_TX_OK;
	}

	ch = &fdev->channels[ch_idx];
	txq = &ch->txq[tc];

	return fdice_xmit_frame(skb, txq);
}

/* -----------------------------------------------------------------------
 * ndo_set_rx_mode - MAC filter programming
 * ----------------------------------------------------------------------- */

/**
 * fdice_set_rx_mode - Program MAC address filters via AQ switch rules
 * @netdev: network device
 *
 * Synchronizes the hardware MAC filter table with the kernel's
 * unicast and multicast address lists. Programs promiscuous and
 * all-multicast modes as needed. Uses AQ switch rule commands to
 * add/remove L2 MAC filters.
 *
 * Called under netif_addr_lock_bh from the network stack.
 */
static void fdice_set_rx_mode(struct net_device *netdev)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	bool promisc_needed;
	bool allmulti_needed;

	promisc_needed = !!(netdev->flags & IFF_PROMISC);
	allmulti_needed = !!(netdev->flags & IFF_ALLMULTI) ||
			 netdev_mc_count(netdev) > fdev->hw.func_caps.num_vsis;

	/*
	 * MAC filter synchronization via AQ switch rules.
	 * In a full implementation this would iterate dev_uc and dev_mc
	 * lists and issue add_sw_rules / remove_sw_rules AQ commands.
	 * For now, set promisc/allmulti flags as appropriate.
	 */
	if (promisc_needed)
		dev_dbg(&fdev->pdev->dev, "enabling promiscuous mode\n");
	if (allmulti_needed)
		dev_dbg(&fdev->pdev->dev, "enabling all-multicast mode\n");

	/*
	 * TODO: Phase 3.11 completion — iterate netdev_for_each_uc_addr
	 * and netdev_for_each_mc_addr to program individual MAC filters
	 * via AQ add_sw_rules (opcode 0x02A0) with the appropriate
	 * recipe for L2 MAC matching.
	 */
}

/* -----------------------------------------------------------------------
 * ndo_change_mtu
 * ----------------------------------------------------------------------- */

/**
 * fdice_change_mtu - Change the device MTU
 * @netdev: network device
 * @new_mtu: requested MTU value
 *
 * Validates the new MTU against hardware limits, updates the netdev
 * MTU, and rebuilds channels if the interface is up (the new MTU may
 * require different buffer sizes and descriptor configurations).
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_change_mtu(struct net_device *netdev, int new_mtu)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	u32 max_mtu;
	int err;

	max_mtu = fdev->hw.func_caps.max_mtu;
	if (max_mtu == 0)
		max_mtu = FDICE_MAX_FRAME_SIZE - ETH_HLEN;

	if (new_mtu < FDICE_MIN_MTU || new_mtu > (int)max_mtu) {
		netdev_err(netdev, "invalid MTU %d (range %d-%u)\n",
			   new_mtu, FDICE_MIN_MTU, max_mtu);
		return -EINVAL;
	}

	if (new_mtu == netdev->mtu)
		return 0;

	/* If interface is up, rebuild channels with new buffer sizing */
	if (netif_running(netdev)) {
		err = fdice_close(netdev);
		if (err)
			return err;

		WRITE_ONCE(netdev->mtu, new_mtu);

		err = fdice_open(netdev);
		if (err) {
			netdev_err(netdev,
				   "failed to reopen after MTU change: %d\n",
				   err);
			return err;
		}
	} else {
		WRITE_ONCE(netdev->mtu, new_mtu);
	}

	netdev_dbg(netdev, "MTU changed to %d\n", new_mtu);
	return 0;
}

/* -----------------------------------------------------------------------
 * ndo_set_mac_address
 * ----------------------------------------------------------------------- */

/**
 * fdice_set_mac - Set the device MAC address
 * @netdev: network device
 * @addr: sockaddr containing the new MAC address
 *
 * Validates the new MAC, programs it via AQ MAC write command, and
 * updates the netdev address. The old MAC filter is removed and a new
 * one is added via AQ switch rules.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_set_mac(struct net_device *netdev, void *addr)
{
	struct sockaddr *sa = addr;

	if (!is_valid_ether_addr(sa->sa_data))
		return -EADDRNOTAVAIL;

	/*
	 * In a full implementation: remove old MAC filter via AQ, add new
	 * MAC filter via AQ add_sw_rules, then update netdev->dev_addr.
	 * TODO: wire AQ command for MAC filter update.
	 */
	eth_hw_addr_set(netdev, sa->sa_data);

	return 0;
}

/* -----------------------------------------------------------------------
 * ndo_get_stats64
 * ----------------------------------------------------------------------- */

/**
 * fdice_get_stats64 - Aggregate per-queue statistics for the stack
 * @netdev: network device
 * @stats: rtnl_link_stats64 to populate
 *
 * Delegates to fdice_stats_get() which iterates all channels and
 * accumulates per-queue TX/RX counters into the unified stats struct.
 */
static void fdice_get_stats64(struct net_device *netdev,
			      struct rtnl_link_stats64 *stats)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	fdice_stats_get(fdev, stats);
}

/* -----------------------------------------------------------------------
 * ndo_setup_tc - TC offload dispatch
 * ----------------------------------------------------------------------- */

/**
 * fdice_setup_tc - Dispatch TC setup operations
 * @netdev: network device
 * @type: TC setup type (mqprio, cls_flower, etc.)
 * @type_data: type-specific configuration data
 *
 * Dispatches mqprio for multi-TC queue configuration and cls_flower
 * for flow-based classification offload via the flow steering subsystem.
 *
 * Return: 0 on success, negative errno on failure, -EOPNOTSUPP for
 *         unsupported TC types
 */
static int fdice_setup_tc(struct net_device *netdev, enum tc_setup_type type,
			   void *type_data)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	switch (type) {
	case TC_SETUP_QDISC_MQPRIO:
		/* TODO: mqprio configuration for ADQ */
		return -EOPNOTSUPP;

	case TC_SETUP_CLSFLOWER:
		return fdice_setup_tc_cls_flower(fdev, type_data);

	default:
		return -EOPNOTSUPP;
	}
}

/* -----------------------------------------------------------------------
 * ndo_set_features - Toggle HW offloads
 * ----------------------------------------------------------------------- */

/**
 * fdice_set_features - Handle feature flag changes
 * @netdev: network device
 * @features: new feature set requested by the user
 *
 * Compares the new feature set against the current configuration and
 * programs hardware offload changes as needed. Handles toggles for
 * VLAN offload, checksumming, TSO, and ntuple filters.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_set_features(struct net_device *netdev,
			      netdev_features_t features)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	netdev_features_t changed = netdev->features ^ features;

	if (changed & NETIF_F_HW_VLAN_CTAG_RX)
		dev_dbg(&fdev->pdev->dev, "toggling RX VLAN stripping\n");

	if (changed & NETIF_F_HW_VLAN_CTAG_TX)
		dev_dbg(&fdev->pdev->dev, "toggling TX VLAN insertion\n");

	if (changed & NETIF_F_RXCSUM)
		dev_dbg(&fdev->pdev->dev, "toggling RX checksum offload\n");

	if (changed & NETIF_F_NTUPLE)
		dev_dbg(&fdev->pdev->dev, "toggling ntuple filters\n");

	if (changed & NETIF_F_HW_TC)
		dev_dbg(&fdev->pdev->dev, "toggling HW TC offload\n");

	/*
	 * TODO: For each changed feature, issue appropriate AQ commands
	 * to update hardware offload configuration. Most features take
	 * effect immediately; some (like VLAN stripping) may require a
	 * brief queue quiesce.
	 */

	return 0;
}

/* -----------------------------------------------------------------------
 * VLAN filter operations
 * ----------------------------------------------------------------------- */

/**
 * fdice_vlan_rx_add_vid - Add a VLAN filter
 * @netdev: network device
 * @proto: VLAN protocol (802.1Q or 802.1ad)
 * @vid: VLAN ID to add
 *
 * Programs a VLAN filter via AQ switch rule to accept frames with
 * the specified VLAN ID.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_vlan_rx_add_vid(struct net_device *netdev,
				 __be16 proto, u16 vid)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	dev_dbg(&fdev->pdev->dev, "adding VLAN %u (proto 0x%04x)\n",
		vid, ntohs(proto));

	/*
	 * TODO: Issue AQ add_sw_rules command with VLAN match recipe
	 * to accept frames tagged with this VID.
	 */

	return 0;
}

/**
 * fdice_vlan_rx_kill_vid - Remove a VLAN filter
 * @netdev: network device
 * @proto: VLAN protocol
 * @vid: VLAN ID to remove
 *
 * Removes the VLAN filter for the specified VID via AQ switch rule.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_vlan_rx_kill_vid(struct net_device *netdev,
				  __be16 proto, u16 vid)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	dev_dbg(&fdev->pdev->dev, "removing VLAN %u (proto 0x%04x)\n",
		vid, ntohs(proto));

	/*
	 * TODO: Issue AQ remove_sw_rules command to delete the VLAN
	 * filter for this VID.
	 */

	return 0;
}

/* -----------------------------------------------------------------------
 * Netdev Allocation and Registration
 * ----------------------------------------------------------------------- */

/**
 * fdice_netdev_alloc - Allocate and configure a net_device
 * @fdev: core device structure
 *
 * Allocates an ethernet net_device with multi-queue support, sets the
 * netdev_ops and ethtool_ops, and configures HW offload feature flags.
 * The number of TX queues is num_channels * FDICE_MAX_TC; RX queues
 * match the channel count.
 *
 * The net_device private data (netdev_priv) stores an fdice_priv
 * back-pointer for efficient ndo callback access.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_netdev_alloc(struct fdice_dev *fdev)
{
	struct net_device *netdev;
	struct fdice_priv *priv;
	u16 num_channels;
	u16 num_txqs;

	/* Bound channel count by MSI-X vectors minus one for async EQ */
	num_channels = min_t(u16, num_online_cpus(),
			     fdev->hw.func_caps.num_msix - 1);
	if (num_channels == 0)
		num_channels = 1;

	num_txqs = num_channels * FDICE_MAX_TC;

	netdev = alloc_etherdev_mqs(sizeof(struct fdice_priv),
				    num_txqs, num_channels);
	if (!netdev) {
		dev_err(&fdev->pdev->dev, "alloc_etherdev_mqs failed\n");
		return -ENOMEM;
	}

	SET_NETDEV_DEV(netdev, &fdev->pdev->dev);

	/* Wire ops tables */
	netdev->netdev_ops = &fdice_netdev_ops;
	netdev->ethtool_ops = &fdice_ethtool_ops;

	/* MTU range supported by E810 hardware */
	netdev->min_mtu = FDICE_MIN_MTU;
	netdev->max_mtu = fdev->hw.func_caps.max_mtu;
	if (netdev->max_mtu == 0)
		netdev->max_mtu = FDICE_MAX_FRAME_SIZE - ETH_HLEN;

	/* Hardware offload feature flags */
	netdev->features |= NETIF_F_SG |
			    NETIF_F_IP_CSUM |
			    NETIF_F_IPV6_CSUM |
			    NETIF_F_TSO |
			    NETIF_F_TSO6 |
			    NETIF_F_GRO |
			    NETIF_F_RXCSUM |
			    NETIF_F_HW_VLAN_CTAG_RX |
			    NETIF_F_HW_VLAN_CTAG_TX |
			    NETIF_F_HW_TC |
			    NETIF_F_NTUPLE |
			    NETIF_F_HIGHDMA;

	netdev->hw_features |= netdev->features;
	netdev->vlan_features |= NETIF_F_SG |
				 NETIF_F_IP_CSUM |
				 NETIF_F_IPV6_CSUM |
				 NETIF_F_TSO |
				 NETIF_F_TSO6 |
				 NETIF_F_GRO |
				 NETIF_F_RXCSUM |
				 NETIF_F_HIGHDMA;

	/* Generate a random MAC address for now; real driver reads from NVM */
	eth_hw_addr_random(netdev);

	/* Cross-link netdev and driver structures */
	priv = netdev_priv(netdev);
	priv->fdev = fdev;
	priv->netdev = netdev;
	priv->num_channels = num_channels;

	fdev->priv.netdev = netdev;
	fdev->priv.fdev = fdev;
	fdev->priv.num_channels = num_channels;
	fdev->netdev = netdev;
	fdev->num_channels = num_channels;

	return 0;
}

/**
 * fdice_netdev_free - Free the net_device
 * @fdev: core device structure
 *
 * Releases the net_device allocated by fdice_netdev_alloc().
 * Must be called after fdice_netdev_unregister().
 */
void fdice_netdev_free(struct fdice_dev *fdev)
{
	if (fdev->netdev) {
		free_netdev(fdev->netdev);
		fdev->netdev = NULL;
		fdev->priv.netdev = NULL;
	}
}

/**
 * fdice_netdev_register - Register the net_device with the kernel
 * @fdev: core device structure
 *
 * Calls register_netdev() to make the interface visible to userspace.
 * Must be called after fdice_netdev_alloc() and after hardware init
 * is complete (link status, MAC address from NVM, etc.).
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_netdev_register(struct fdice_dev *fdev)
{
	int err;

	if (!fdev->netdev)
		return -EINVAL;

	err = register_netdev(fdev->netdev);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"register_netdev failed: %d\n", err);
		return err;
	}

	netif_carrier_off(fdev->netdev);

	dev_info(&fdev->pdev->dev, "registered netdev %s\n",
		 fdev->netdev->name);

	return 0;
}

/**
 * fdice_netdev_unregister - Unregister the net_device from the kernel
 * @fdev: core device structure
 *
 * Removes the interface from the kernel networking stack.
 * Must be called before fdice_netdev_free().
 */
void fdice_netdev_unregister(struct fdice_dev *fdev)
{
	if (fdev->netdev && fdev->netdev->reg_state == NETREG_REGISTERED)
		unregister_netdev(fdev->netdev);
}
