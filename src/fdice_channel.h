// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_CHANNEL_H_
#define _FDICE_CHANNEL_H_

/**
 * DOC: FrankenDrICE Channel Model
 *
 * The channel is the per-NAPI grouping of all queues serviced by a
 * single interrupt vector. Each channel embeds (not points to) its
 * queue structs for cache locality, following the principle that the
 * RXQ, its CQ, and the NAPI struct should be in adjacent cache lines.
 *
 * Traffic class management uses xarray-based channel groups and
 * rhashtable-based TC flower filter lookup for O(1) operations.
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md
 */

#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/xarray.h>
#include <linux/rhashtable.h>
#include <linux/mutex.h>

#include "fdice_queue.h"

/* Forward declarations */
struct fdice_eq;
struct fdice_priv;
struct fdice_dev;
struct devlink_health_reporter;

/* Maximum traffic classes supported by E810 hardware */
#define FDICE_MAX_TC		8

/* -----------------------------------------------------------------------
 * Channel Parameters
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_params - Runtime channel configuration parameters
 * @rx_ring_size: RX descriptor ring size (power of 2)
 * @tx_ring_size: TX descriptor ring size per TC (power of 2)
 * @rx_buf_size: Per-RX-buffer allocation size
 * @rx_coalesce_usecs: RX interrupt coalescing delay (microseconds)
 * @tx_coalesce_usecs: TX interrupt coalescing delay (microseconds)
 * @rx_coalesce_frames: RX interrupt coalescing frame count
 * @tx_coalesce_frames: TX interrupt coalescing frame count
 * @use_adaptive_rx: Enable DIM adaptive RX coalescing
 * @use_adaptive_tx: Enable DIM adaptive TX coalescing
 * @xdp_enabled: XDP program attached
 * @xsk_enabled: AF_XDP zero-copy active
 * @hdr_split_enabled: RX header-data split active
 * @timestamp_enabled: PTP timestamping active
 * @num_tc: Number of active traffic classes
 * @num_channels: Number of channels (NAPI contexts)
 *
 * Parameter changes follow the safe-switch pattern: build new params,
 * validate, swap atomically, then rebuild affected channels.
 */
struct fdice_params {
	u32			rx_ring_size;
	u32			tx_ring_size;
	u16			rx_buf_size;

	/* Interrupt coalescing */
	u16			rx_coalesce_usecs;
	u16			tx_coalesce_usecs;
	u16			rx_coalesce_frames;
	u16			tx_coalesce_frames;
	bool			use_adaptive_rx;
	bool			use_adaptive_tx;

	/* Feature flags */
	bool			xdp_enabled;
	bool			xsk_enabled;
	bool			hdr_split_enabled;
	bool			timestamp_enabled;

	/* TC and channel counts */
	u8			num_tc;
	u16			num_channels;
};

/* -----------------------------------------------------------------------
 * Channel Structure
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_channel - Per-NAPI queue group
 * @napi: NAPI scheduling context (kept near top for cache locality)
 * @rxq: Embedded receive queue
 * @txq: Embedded transmit queues, one per traffic class
 * @eq: Parent event queue providing the MSI-X interrupt vector
 * @params: Pointer to channel configuration parameters
 * @cpu: CPU this channel is bound to
 * @numa_node: NUMA node for memory allocation affinity
 * @idx: Channel index within the device
 * @priv: Back-pointer to driver private data
 *
 * Queues are embedded directly rather than pointed-to, ensuring that
 * the RXQ, its CQ, and the NAPI struct occupy adjacent cache lines.
 * The TXQ array is indexed by traffic class for O(1) TC-to-queue mapping.
 */
struct fdice_channel {
	struct napi_struct	napi;
	struct fdice_rxq	rxq;
	struct fdice_txq	txq[FDICE_MAX_TC];

	struct fdice_eq		*eq;
	struct fdice_params	*params;

	int			cpu;
	int			numa_node;
	u16			idx;
	struct fdice_priv	*priv;

	/* Per-channel devlink health reporters (Phase 3.4) */
	struct devlink_health_reporter	*tx_reporter;
	struct devlink_health_reporter	*rx_reporter;
};

/* -----------------------------------------------------------------------
 * Traffic Class Management
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_tc_info - Per-traffic-class queue group descriptor
 * @qoffset: First queue index in the TC group
 * @qcount: Number of queues in the TC group
 * @netdev_tc: Kernel TC number (maps to netdev_set_tc_queue)
 * @max_bw: Maximum bandwidth in Kbps (0 = unlimited)
 * @min_bw: Guaranteed minimum bandwidth in Kbps
 *
 * TC0 always uses the main PF channel array. TC1..FDICE_MAX_TC each
 * get a channel group allocated from the device queue pool.
 */
struct fdice_tc_info {
	u16			qoffset;
	u16			qcount;
	u8			netdev_tc;
	u64			max_bw;
	u64			min_bw;
};

/**
 * struct fdice_tc_mgr - Traffic class manager
 * @tc: Per-TC configuration array
 * @num_tc: Number of active traffic classes
 * @ena_tc: TC enable bitmap
 * @channel_xa: xarray of channels keyed by FDICE_TC_CHAN_KEY(tc, idx)
 * @flower_ht: rhashtable for O(1) TC flower filter cookie lookup
 * @lock: Serializes TC configuration changes
 *
 * Manages ADQ channel groups with xarray-based O(1) lookup and
 * rhashtable-based TC flower filter management.
 */
struct fdice_tc_mgr {
	struct fdice_tc_info	tc[FDICE_MAX_TC];
	u8			num_tc;
	u16			ena_tc;
	struct xarray		channel_xa;
	struct rhashtable	flower_ht;
	struct mutex		lock;
};

/**
 * FDICE_TC_CHAN_KEY - Compute xarray key for TC channel lookup
 * @tc: Traffic class number (0..FDICE_MAX_TC-1)
 * @idx: Channel index within the TC group
 *
 * Encodes TC number in upper 16 bits and channel offset in lower 16 bits
 * for sparse O(1) lookup in the channel xarray.
 *
 * Return: Composite 32-bit xarray key
 */
#define FDICE_TC_CHAN_KEY(tc, idx)  (((u32)(tc) << 16) | (u16)(idx))

/* -----------------------------------------------------------------------
 * Channel Function Declarations
 * ----------------------------------------------------------------------- */

/* Single channel lifecycle */
int fdice_channel_create(struct fdice_channel *ch, struct fdice_dev *fdev,
			 struct fdice_params *params, u16 idx);
void fdice_channel_destroy(struct fdice_channel *ch);
int fdice_channel_enable(struct fdice_channel *ch);
void fdice_channel_disable(struct fdice_channel *ch);

/* Bulk channel management */
int fdice_alloc_channels(struct fdice_dev *fdev, struct fdice_params *params);
void fdice_free_channels(struct fdice_dev *fdev);
int fdice_enable_channels(struct fdice_dev *fdev);
void fdice_disable_channels(struct fdice_dev *fdev);

/* TC management */
int fdice_tc_mgr_init(struct fdice_tc_mgr *mgr);
void fdice_tc_mgr_cleanup(struct fdice_tc_mgr *mgr);
int fdice_tc_validate(struct fdice_params *params);

#endif /* _FDICE_CHANNEL_H_ */
