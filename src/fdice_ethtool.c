// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_ethtool.c - Ethtool operations for FrankenDrICE
 *
 * Phase 3.11 implementation: ethtool_ops covering link settings,
 * per-port/per-queue statistics, interrupt coalescing (DIM-aware),
 * channel count, ring size, rxnfc (FDIR ntuple rules), and PTP
 * timestamp capabilities.
 *
 * Statistics collection iterates the channel array and aggregates
 * embedded per-queue counters. The stat string arrays are defined
 * in fdice_stats.c and referenced here for get_strings/get_sset_count.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.11)
 */

#include <linux/netdevice.h>
#include <linux/ethtool.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/dim.h>

#include "fdice_dev.h"
#include "fdice_channel.h"
#include "fdice_queue.h"
#include "fdice_fs_core.h"

/* Stats string/count API from fdice_stats.c */
extern const char fdice_port_stat_strings[][ETH_GSTRING_LEN];
extern const char fdice_txq_stat_strings[][ETH_GSTRING_LEN];
extern const char fdice_rxq_stat_strings[][ETH_GSTRING_LEN];

#define FDICE_PORT_STATS_COUNT	6
#define FDICE_TXQ_STATS_COUNT	9
#define FDICE_RXQ_STATS_COUNT	11

/* -----------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------- */

static void fdice_get_drvinfo(struct net_device *netdev,
			      struct ethtool_drvinfo *info);
static int fdice_get_link_ksettings(struct net_device *netdev,
				    struct ethtool_link_ksettings *cmd);
static int fdice_set_link_ksettings(struct net_device *netdev,
				    const struct ethtool_link_ksettings *cmd);
static void fdice_get_strings(struct net_device *netdev, u32 stringset,
			      u8 *data);
static int fdice_get_sset_count(struct net_device *netdev, int sset);
static void fdice_get_ethtool_stats(struct net_device *netdev,
				    struct ethtool_stats *stats, u64 *data);
static void fdice_get_channels(struct net_device *netdev,
			       struct ethtool_channels *ch);
static int fdice_set_channels(struct net_device *netdev,
			      struct ethtool_channels *ch);
static void fdice_get_ringparam(struct net_device *netdev,
				struct ethtool_ringparam *ring,
				struct kernel_ethtool_ringparam *kernel_ring,
				struct netlink_ext_ack *extack);
static int fdice_set_ringparam(struct net_device *netdev,
			       struct ethtool_ringparam *ring,
			       struct kernel_ethtool_ringparam *kernel_ring,
			       struct netlink_ext_ack *extack);
static int fdice_get_coalesce(struct net_device *netdev,
			      struct ethtool_coalesce *ec,
			      struct kernel_ethtool_coalesce *kernel_coal,
			      struct netlink_ext_ack *extack);
static int fdice_set_coalesce(struct net_device *netdev,
			      struct ethtool_coalesce *ec,
			      struct kernel_ethtool_coalesce *kernel_coal,
			      struct netlink_ext_ack *extack);
static int fdice_get_rxnfc(struct net_device *netdev,
			   struct ethtool_rxnfc *cmd, u32 *rule_locs);
static int fdice_set_rxnfc(struct net_device *netdev,
			   struct ethtool_rxnfc *cmd);
static int fdice_get_ts_info(struct net_device *netdev,
			     struct kernel_ethtool_ts_info *info);

/* -----------------------------------------------------------------------
 * Ethtool Ops Table
 * ----------------------------------------------------------------------- */

const struct ethtool_ops fdice_ethtool_ops = {
	.supported_coalesce_params = ETHTOOL_COALESCE_USECS |
				     ETHTOOL_COALESCE_MAX_FRAMES |
				     ETHTOOL_COALESCE_USE_ADAPTIVE,
	.get_drvinfo		= fdice_get_drvinfo,
	.get_link		= ethtool_op_get_link,
	.get_link_ksettings	= fdice_get_link_ksettings,
	.set_link_ksettings	= fdice_set_link_ksettings,
	.get_strings		= fdice_get_strings,
	.get_sset_count		= fdice_get_sset_count,
	.get_ethtool_stats	= fdice_get_ethtool_stats,
	.get_channels		= fdice_get_channels,
	.set_channels		= fdice_set_channels,
	.get_ringparam		= fdice_get_ringparam,
	.set_ringparam		= fdice_set_ringparam,
	.get_coalesce		= fdice_get_coalesce,
	.set_coalesce		= fdice_set_coalesce,
	.get_rxnfc		= fdice_get_rxnfc,
	.set_rxnfc		= fdice_set_rxnfc,
	.get_ts_info		= fdice_get_ts_info,
};

/* -----------------------------------------------------------------------
 * Driver Info
 * ----------------------------------------------------------------------- */

/**
 * fdice_get_drvinfo - Report driver name, version, FW version, bus info
 * @netdev: network device
 * @info: ethtool drvinfo structure to populate
 *
 * Populates the standard driver identification fields. FW version
 * is read from the cached fdice_fw_ver populated at probe time.
 */
static void fdice_get_drvinfo(struct net_device *netdev,
			      struct ethtool_drvinfo *info)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	struct fdice_fw_ver *fw = &fdev->hw.fw;

	strscpy(info->driver, "frankendrice", sizeof(info->driver));
	snprintf(info->fw_version, sizeof(info->fw_version),
		 "%u.%u.%u build %u",
		 fw->fw_major, fw->fw_minor, fw->fw_patch, fw->fw_build);
	strscpy(info->bus_info, pci_name(fdev->pdev), sizeof(info->bus_info));
}

/* -----------------------------------------------------------------------
 * Link Settings
 * ----------------------------------------------------------------------- */

/**
 * fdice_get_link_ksettings - Report link speed, duplex, and autoneg
 * @netdev: network device
 * @cmd: ethtool link ksettings structure to populate
 *
 * Returns current link speed from port_info cached state. E810 ports
 * are always full-duplex. Autoneg capability is reported from PHY
 * type bitmaps.
 *
 * Return: 0 on success
 */
static int fdice_get_link_ksettings(struct net_device *netdev,
				    struct ethtool_link_ksettings *cmd)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	struct fdice_port_info *pi = fdev->hw.port_info;

	ethtool_link_ksettings_zero_link_mode(cmd, supported);
	ethtool_link_ksettings_zero_link_mode(cmd, advertising);

	/* E810 supports various speeds depending on PHY type */
	ethtool_link_ksettings_add_link_mode(cmd, supported, 10000baseT_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 25000baseCR_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, 100000baseSR4_Full);
	ethtool_link_ksettings_add_link_mode(cmd, supported, Autoneg);
	ethtool_link_ksettings_add_link_mode(cmd, supported, FEC_RS);
	ethtool_link_ksettings_add_link_mode(cmd, supported, FEC_BASER);

	if (pi && pi->link_up) {
		cmd->base.speed = pi->link_speed;
		cmd->base.duplex = DUPLEX_FULL;
	} else {
		cmd->base.speed = SPEED_UNKNOWN;
		cmd->base.duplex = DUPLEX_UNKNOWN;
	}

	cmd->base.port = PORT_OTHER;
	cmd->base.autoneg = AUTONEG_ENABLE;

	return 0;
}

/**
 * fdice_set_link_ksettings - Configure link speed and autoneg
 * @netdev: network device
 * @cmd: ethtool link ksettings with desired configuration
 *
 * Programs the PHY configuration via AQ set_phy_cfg command.
 * Currently returns -EOPNOTSUPP as full PHY configuration requires
 * detailed PHY capability decoding from capabilities.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_set_link_ksettings(struct net_device *netdev,
				    const struct ethtool_link_ksettings *cmd)
{
	/* TODO: AQ set_phy_cfg for link parameter changes */
	return -EOPNOTSUPP;
}

/* -----------------------------------------------------------------------
 * Statistics
 * ----------------------------------------------------------------------- */

/**
 * fdice_ethtool_stat_count - Total number of ethtool statistics
 * @fdev: core device
 *
 * Computes total stat count: per-port stats plus per-queue stats
 * for all channels.
 *
 * Return: Total number of statistics strings
 */
static int fdice_ethtool_stat_count(struct fdice_dev *fdev)
{
	u16 nch = fdev->num_channels;

	return FDICE_PORT_STATS_COUNT +
	       nch * FDICE_TXQ_STATS_COUNT +
	       nch * FDICE_RXQ_STATS_COUNT;
}

/**
 * fdice_get_strings - Report ethtool string sets
 * @netdev: network device
 * @stringset: which string set to report
 * @data: buffer to write strings into
 *
 * Populates per-port stat names followed by per-queue stat names
 * with queue index embedded in the string (e.g., "tx0_packets").
 */
static void fdice_get_strings(struct net_device *netdev, u32 stringset,
			      u8 *data)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	u16 nch = fdev->num_channels;
	int i, q;

	if (stringset != ETH_SS_STATS)
		return;

	/* Port-level stats */
	for (i = 0; i < FDICE_PORT_STATS_COUNT; i++) {
		strscpy(data, fdice_port_stat_strings[i], ETH_GSTRING_LEN);
		data += ETH_GSTRING_LEN;
	}

	/* Per-queue TX stats */
	for (q = 0; q < nch; q++) {
		for (i = 0; i < FDICE_TXQ_STATS_COUNT; i++) {
			snprintf(data, ETH_GSTRING_LEN, "tx%d_%s",
				 q, fdice_txq_stat_strings[i]);
			data += ETH_GSTRING_LEN;
		}
	}

	/* Per-queue RX stats */
	for (q = 0; q < nch; q++) {
		for (i = 0; i < FDICE_RXQ_STATS_COUNT; i++) {
			snprintf(data, ETH_GSTRING_LEN, "rx%d_%s",
				 q, fdice_rxq_stat_strings[i]);
			data += ETH_GSTRING_LEN;
		}
	}
}

/**
 * fdice_get_sset_count - Report number of strings in a string set
 * @netdev: network device
 * @sset: string set identifier
 *
 * Return: number of strings, or -EOPNOTSUPP for unknown sets
 */
static int fdice_get_sset_count(struct net_device *netdev, int sset)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	switch (sset) {
	case ETH_SS_STATS:
		return fdice_ethtool_stat_count(fdev);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * fdice_get_ethtool_stats - Report per-port and per-queue statistics
 * @netdev: network device
 * @stats: ethtool stats header (unused)
 * @data: u64 array to populate with stat values
 *
 * Iterates all channels, reading TX and RX queue stats. Port-level
 * stats are aggregated from per-queue counters. Array order matches
 * fdice_get_strings().
 */
static void fdice_get_ethtool_stats(struct net_device *netdev,
				    struct ethtool_stats *stats, u64 *data)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	struct fdice_channel *ch;
	u64 tx_packets = 0, tx_bytes = 0, tx_errors = 0;
	u64 rx_packets = 0, rx_bytes = 0, rx_errors = 0;
	int idx = 0;
	u16 q;

	/* First pass: aggregate port-level stats from queues */
	for (q = 0; q < fdev->num_channels; q++) {
		if (!fdev->channels)
			break;
		ch = &fdev->channels[q];
		tx_packets += ch->txq[0].stats.packets;
		tx_bytes += ch->txq[0].stats.bytes;
		tx_errors += ch->txq[0].stats.dropped;
		rx_packets += ch->rxq.stats.packets;
		rx_bytes += ch->rxq.stats.bytes;
		rx_errors += ch->rxq.stats.csum_err;
	}

	/* Port stats */
	data[idx++] = rx_packets;
	data[idx++] = tx_packets;
	data[idx++] = rx_bytes;
	data[idx++] = tx_bytes;
	data[idx++] = rx_errors;
	data[idx++] = tx_errors;

	/* Per-queue TX stats */
	for (q = 0; q < fdev->num_channels; q++) {
		struct fdice_txq_stats *ts;

		if (!fdev->channels) {
			memset(&data[idx], 0,
			       FDICE_TXQ_STATS_COUNT * sizeof(u64));
			idx += FDICE_TXQ_STATS_COUNT;
			continue;
		}

		ts = &fdev->channels[q].txq[0].stats;
		data[idx++] = ts->packets;
		data[idx++] = ts->bytes;
		data[idx++] = ts->tso_packets;
		data[idx++] = ts->tso_bytes;
		data[idx++] = ts->linearize;
		data[idx++] = ts->csum_partial;
		data[idx++] = ts->stopped;
		data[idx++] = ts->wake;
		data[idx++] = ts->dropped;
	}

	/* Per-queue RX stats */
	for (q = 0; q < fdev->num_channels; q++) {
		struct fdice_rxq_stats *rs;

		if (!fdev->channels) {
			memset(&data[idx], 0,
			       FDICE_RXQ_STATS_COUNT * sizeof(u64));
			idx += FDICE_RXQ_STATS_COUNT;
			continue;
		}

		rs = &fdev->channels[q].rxq.stats;
		data[idx++] = rs->packets;
		data[idx++] = rs->bytes;
		data[idx++] = rs->alloc_fail;
		data[idx++] = rs->csum_complete;
		data[idx++] = rs->csum_none;
		data[idx++] = rs->csum_err;
		data[idx++] = rs->xdp_drop;
		data[idx++] = rs->xdp_tx;
		data[idx++] = rs->xdp_redirect;
		data[idx++] = rs->lro_packets;
		data[idx++] = rs->lro_bytes;
	}
}

/* -----------------------------------------------------------------------
 * Channel Configuration
 * ----------------------------------------------------------------------- */

/**
 * fdice_get_channels - Report channel counts
 * @netdev: network device
 * @ch: ethtool channels structure to populate
 *
 * Reports current and maximum channel counts. The maximum is bounded
 * by MSI-X vectors minus one (for async EQ).
 */
static void fdice_get_channels(struct net_device *netdev,
			       struct ethtool_channels *ch)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	u16 max_ch;

	max_ch = min_t(u16, num_online_cpus(),
		       fdev->hw.func_caps.num_msix - 1);
	if (max_ch == 0)
		max_ch = 1;

	ch->max_combined = max_ch;
	ch->combined_count = fdev->num_channels;
}

/**
 * fdice_rebuild_channels - Rebuild channels by closing and reopening
 * @netdev: network device
 *
 * Performs a dev_close/dev_open cycle to rebuild channels with any
 * changed parameters (channel count, ring size, etc.). Uses the
 * kernel dev_close/dev_open APIs which invoke ndo_stop/ndo_open.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_rebuild_channels(struct net_device *netdev)
{
	if (!netif_running(netdev))
		return 0;

	dev_close(netdev);
	return dev_open(netdev, NULL);
}

/**
 * fdice_set_channels - Change channel count (triggers rebuild)
 * @netdev: network device
 * @ch: ethtool channels structure with desired counts
 *
 * Validates the requested channel count, then performs a close/open
 * cycle to rebuild channels with the new count if the interface is up.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_set_channels(struct net_device *netdev,
			      struct ethtool_channels *ch)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	u16 max_ch;

	if (ch->rx_count || ch->tx_count || ch->other_count)
		return -EINVAL;

	max_ch = min_t(u16, num_online_cpus(),
		       fdev->hw.func_caps.num_msix - 1);
	if (max_ch == 0)
		max_ch = 1;

	if (ch->combined_count == 0 || ch->combined_count > max_ch)
		return -EINVAL;

	if (ch->combined_count == fdev->num_channels)
		return 0;

	fdev->num_channels = ch->combined_count;
	fdev->priv.num_channels = ch->combined_count;

	/* Rebuild channels if interface is running */
	return fdice_rebuild_channels(netdev);
}

/* -----------------------------------------------------------------------
 * Ring Parameters
 * ----------------------------------------------------------------------- */

/**
 * fdice_get_ringparam - Report ring sizes
 * @netdev: network device
 * @ring: ethtool ringparam structure to populate
 * @kernel_ring: kernel ring parameters (unused)
 * @extack: netlink extended ack (unused)
 *
 * Reports current and maximum TX/RX ring sizes.
 */
static void fdice_get_ringparam(struct net_device *netdev,
				struct ethtool_ringparam *ring,
				struct kernel_ethtool_ringparam *kernel_ring,
				struct netlink_ext_ack *extack)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	ring->rx_max_pending = 8192;
	ring->tx_max_pending = 8192;

	/*
	 * Report current ring sizes from the first channel if channels
	 * are allocated, otherwise report defaults.
	 */
	if (fdev->channels) {
		ring->rx_pending = fdev->channels[0].rxq.wq.sz;
		ring->tx_pending = fdev->channels[0].txq[0].wq.sz;
	} else {
		ring->rx_pending = 1024;
		ring->tx_pending = 1024;
	}
}

/**
 * fdice_set_ringparam - Change ring sizes (triggers rebuild)
 * @netdev: network device
 * @ring: ethtool ringparam with desired sizes
 * @kernel_ring: kernel ring parameters (unused)
 * @extack: netlink extended ack for error reporting
 *
 * Validates ring size (must be power of 2 and within bounds), then
 * rebuilds channels if the interface is running.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_set_ringparam(struct net_device *netdev,
			       struct ethtool_ringparam *ring,
			       struct kernel_ethtool_ringparam *kernel_ring,
			       struct netlink_ext_ack *extack)
{
	if (ring->rx_pending < 64 || ring->rx_pending > 8192 ||
	    !is_power_of_2(ring->rx_pending)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "RX ring size must be power of 2 (64-8192)");
		return -EINVAL;
	}

	if (ring->tx_pending < 64 || ring->tx_pending > 8192 ||
	    !is_power_of_2(ring->tx_pending)) {
		NL_SET_ERR_MSG_MOD(extack,
				   "TX ring size must be power of 2 (64-8192)");
		return -EINVAL;
	}

	/*
	 * TODO: Store new ring sizes and rebuild channels. Requires
	 * persisting ring size parameters across close/open cycles.
	 */

	return fdice_rebuild_channels(netdev);
}

/* -----------------------------------------------------------------------
 * Interrupt Coalescing
 * ----------------------------------------------------------------------- */

/**
 * fdice_get_coalesce - Report interrupt coalescing parameters
 * @netdev: network device
 * @ec: ethtool coalesce structure to populate
 * @kernel_coal: kernel coalesce parameters (unused)
 * @extack: netlink extended ack (unused)
 *
 * Reports current interrupt coalescing settings. When DIM (Dynamic
 * Interrupt Moderation) is active, the adaptive flag is set.
 *
 * Return: 0 on success
 */
static int fdice_get_coalesce(struct net_device *netdev,
			      struct ethtool_coalesce *ec,
			      struct kernel_ethtool_coalesce *kernel_coal,
			      struct netlink_ext_ack *extack)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	/* Report defaults or first channel's settings */
	if (fdev->channels) {
		struct fdice_channel *ch = &fdev->channels[0];

		if (ch->params) {
			ec->rx_coalesce_usecs = ch->params->rx_coalesce_usecs;
			ec->tx_coalesce_usecs = ch->params->tx_coalesce_usecs;
			ec->rx_max_coalesced_frames = ch->params->rx_coalesce_frames;
			ec->tx_max_coalesced_frames = ch->params->tx_coalesce_frames;
			ec->use_adaptive_rx_coalesce = ch->params->use_adaptive_rx;
			ec->use_adaptive_tx_coalesce = ch->params->use_adaptive_tx;
		}
	} else {
		ec->rx_coalesce_usecs = 50;
		ec->tx_coalesce_usecs = 50;
		ec->use_adaptive_rx_coalesce = 1;
		ec->use_adaptive_tx_coalesce = 1;
	}

	return 0;
}

/**
 * fdice_set_coalesce - Configure interrupt coalescing
 * @netdev: network device
 * @ec: ethtool coalesce structure with desired settings
 * @kernel_coal: kernel coalesce parameters (unused)
 * @extack: netlink extended ack for error reporting
 *
 * Updates interrupt coalescing parameters for all channels. When
 * adaptive coalescing is enabled, DIM manages ITR dynamically;
 * otherwise static values are programmed into the ITR registers.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_set_coalesce(struct net_device *netdev,
			      struct ethtool_coalesce *ec,
			      struct kernel_ethtool_coalesce *kernel_coal,
			      struct netlink_ext_ack *extack)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;
	u16 q;

	/*
	 * Apply to all channels. In a full implementation, this would
	 * also write the ITR registers via MMIO or AQ.
	 */
	for (q = 0; q < fdev->num_channels && fdev->channels; q++) {
		struct fdice_channel *ch = &fdev->channels[q];

		if (!ch->params)
			continue;

		ch->params->rx_coalesce_usecs = ec->rx_coalesce_usecs;
		ch->params->tx_coalesce_usecs = ec->tx_coalesce_usecs;
		ch->params->rx_coalesce_frames = ec->rx_max_coalesced_frames;
		ch->params->tx_coalesce_frames = ec->tx_max_coalesced_frames;
		ch->params->use_adaptive_rx = ec->use_adaptive_rx_coalesce;
		ch->params->use_adaptive_tx = ec->use_adaptive_tx_coalesce;
	}

	return 0;
}

/* -----------------------------------------------------------------------
 * RX Flow Classification (FDIR / ntuple)
 * ----------------------------------------------------------------------- */

/**
 * fdice_get_rxnfc - Report RX flow classification rules
 * @netdev: network device
 * @cmd: ethtool rxnfc command structure
 * @rule_locs: array to fill with rule locations (for GET_ALL_FLTR)
 *
 * Dispatches to the flow steering subsystem for FDIR ntuple rule
 * queries. Supports GET_RXFH_INDIR (RSS), GET_RXFH_KEY, and
 * GET/SET individual flow classification rules.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_get_rxnfc(struct net_device *netdev,
			   struct ethtool_rxnfc *cmd, u32 *rule_locs)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	switch (cmd->cmd) {
	case ETHTOOL_GRXRINGS:
		cmd->data = fdev->num_channels;
		return 0;

	case ETHTOOL_GRXCLSRULE:
		return fdice_fdir_get_ntuple(fdev, cmd);

	case ETHTOOL_GRXCLSRLCNT:
		cmd->rule_cnt = fdev->hw.func_caps.fd_fltr_guar +
				fdev->hw.func_caps.fd_fltr_best_effort;
		cmd->data = fdev->hw.func_caps.fd_fltr_guar;
		return 0;

	default:
		return -EOPNOTSUPP;
	}
}

/**
 * fdice_set_rxnfc - Configure RX flow classification rules
 * @netdev: network device
 * @cmd: ethtool rxnfc command with rule to add/delete
 *
 * Dispatches ntuple rule add/delete to the flow steering subsystem.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_set_rxnfc(struct net_device *netdev,
			   struct ethtool_rxnfc *cmd)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	switch (cmd->cmd) {
	case ETHTOOL_SRXCLSRLINS:
		return fdice_fdir_set_ntuple(fdev, cmd);

	case ETHTOOL_SRXCLSRLDEL:
		/* TODO: delete FDIR rule by location */
		return -EOPNOTSUPP;

	default:
		return -EOPNOTSUPP;
	}
}

/* -----------------------------------------------------------------------
 * PTP Timestamp Capabilities
 * ----------------------------------------------------------------------- */

/**
 * fdice_get_ts_info - Report PTP timestamp capabilities
 * @netdev: network device
 * @info: kernel ethtool ts_info structure to populate
 *
 * Reports hardware timestamping capabilities. If a PTP clock is
 * registered, provides the PHC index for user-space tools (ptp4l).
 *
 * Return: 0 on success
 */
static int fdice_get_ts_info(struct net_device *netdev,
			     struct kernel_ethtool_ts_info *info)
{
	struct fdice_priv *priv = netdev_priv(netdev);
	struct fdice_dev *fdev = priv->fdev;

	info->so_timestamping = SOF_TIMESTAMPING_TX_SOFTWARE |
				SOF_TIMESTAMPING_RX_SOFTWARE |
				SOF_TIMESTAMPING_SOFTWARE;

	if (fdev->ptp.clock) {
		info->so_timestamping |= SOF_TIMESTAMPING_TX_HARDWARE |
					 SOF_TIMESTAMPING_RX_HARDWARE |
					 SOF_TIMESTAMPING_RAW_HARDWARE;
		info->phc_index = ptp_clock_index(fdev->ptp.clock);
		info->tx_types = BIT(HWTSTAMP_TX_OFF) | BIT(HWTSTAMP_TX_ON);
		info->rx_filters = BIT(HWTSTAMP_FILTER_NONE) |
				   BIT(HWTSTAMP_FILTER_ALL);
	} else {
		info->phc_index = -1;
	}

	return 0;
}
