// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_QUEUE_H_
#define _FDICE_QUEUE_H_

/**
 * DOC: FrankenDrICE Queue Structure Definitions
 *
 * Three-level queue hierarchy: EQ -> CQ -> WQ (work queue).
 * EQs route interrupts, CQs provide completion notification with
 * arm/disarm semantics, and WQs are the typed descriptor rings where
 * packets are posted (TX) and received (RX).
 *
 * Cache-line grouping follows the read_mostly / read_write / cold
 * pattern for hot-path performance.
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md
 */

#include <linux/types.h>
#include <linux/netdevice.h>
#include <linux/skbuff.h>
#include <linux/dim.h>
#include <net/page_pool/types.h>
#include <net/xdp.h>

/* Forward declarations */
struct fdice_channel;
struct fdice_eq;
struct fdice_cq;
struct fdice_rxq;
struct fdice_txq;
struct fdice_dev;
struct fdice_priv;
struct fdice_params;

/* -----------------------------------------------------------------------
 * Queue State Machine
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_queue_state - Unified lifecycle state for all queue types
 * @FDICE_QS_UNINIT:     No resources allocated
 * @FDICE_QS_ALLOCATED:  DMA buffers and ring memory allocated
 * @FDICE_QS_CONFIGURED: CQ registered, doorbell mapped, context written via AQ
 * @FDICE_QS_ACTIVE:     Interrupts enabled, buffers posted, queue running
 * @FDICE_QS_DISABLED:   DMA buffers retained, interrupts off, drain complete
 * @FDICE_QS_DESTROYED:  All resources freed
 *
 * Transitions: UNINIT->ALLOCATED->CONFIGURED->ACTIVE->DISABLED->DESTROYED
 */
enum fdice_queue_state {
	FDICE_QS_UNINIT = 0,
	FDICE_QS_ALLOCATED,
	FDICE_QS_CONFIGURED,
	FDICE_QS_ACTIVE,
	FDICE_QS_DISABLED,
	FDICE_QS_DESTROYED,
};

/* -----------------------------------------------------------------------
 * DMA Fragment Buffer Management
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_frag_buf - DMA-coherent fragment buffer
 * @buf: Array of virtual addresses for each page-sized fragment
 * @dma: Array of DMA addresses for each fragment
 * @size: Total buffer size in bytes
 * @npages: Number of page-sized fragments
 *
 * Provides NUMA-aware coherent DMA allocation using page-sized fragments
 * rather than a single contiguous allocation. This avoids high-order page
 * allocation failures for large rings.
 */
struct fdice_frag_buf {
	void			**buf;
	dma_addr_t		*dma;
	int			size;
	int			npages;
};

/**
 * struct fdice_frag_buf_ctrl - Fragment buffer control for ring indexing
 * @frag_buf: Underlying fragment buffer
 * @frag_sz_shift: log2(fragment size) for index-to-fragment calculation
 * @strides_per_frag: Number of WQE strides fitting in one fragment
 * @frag_mask: Mask for offset within a fragment (frag_size - 1)
 * @log_stride: log2(stride size) for efficient multiplication
 *
 * Translates ring indices to virtual and DMA addresses within the
 * fragmented DMA buffer using shift-based arithmetic.
 */
struct fdice_frag_buf_ctrl {
	struct fdice_frag_buf	frag_buf;
	u32			frag_sz_shift;
	u32			strides_per_frag;
	u32			frag_mask;
	u8			log_stride;
};

/* -----------------------------------------------------------------------
 * Cyclic Work Queue (Ring Buffer)
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_wq_cyc - Cyclic (circular) work queue descriptor ring
 * @fbc: Fragment buffer control for DMA address translation
 * @db: Doorbell pointer for hardware notification
 * @sz: Ring size in entries (power of 2)
 * @mask: Ring index mask (sz - 1)
 * @stride: WQE size in bytes
 * @pc: Producer counter (advances on post)
 * @cc: Consumer counter (advances on completion)
 *
 * Standard ring buffer with power-of-2 size used for TX send queues,
 * RX queues, and control queues. Producer/consumer counters are free-
 * running; mask is applied to derive ring indices.
 */
struct fdice_wq_cyc {
	struct fdice_frag_buf_ctrl fbc;
	__be32			*db;
	u32			sz;
	u32			mask;
	u16			stride;
	u16			pc;
	u16			cc;
};

/* -----------------------------------------------------------------------
 * Completion Queue
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_cqwq - Completion queue work queue ring
 * @fbc: Fragment buffer control for CQE access
 * @cc: Consumer counter
 * @sz: Ring size in CQEs
 * @mask: Ring index mask (sz - 1)
 * @log_cqe_sz: log2(CQE size) for address calculation
 *
 * Ring buffer for reading completion entries. Uses a consumer counter
 * and owner-bit protocol for hardware synchronization.
 */
struct fdice_cqwq {
	struct fdice_frag_buf_ctrl fbc;
	u32			cc;
	u32			sz;
	u32			mask;
	u8			log_cqe_sz;
};

/**
 * struct fdice_cq - Completion queue with arm/disarm semantics
 * @wq: CQ ring buffer for reading completion entries
 * @comp_handler: Callback invoked on completion (NAPI schedule or RDMA CQ)
 * @eq: Parent event queue for interrupt routing
 * @napi: Associated NAPI struct (ethernet CQs; NULL for RDMA)
 * @cqn: CQ number used as xarray key in parent EQ
 * @event_ctr: Completion event counter for coalescing
 * @state: Arm/disarm state
 * @arm_db: Arm doorbell register pointer
 * @ci_db: Consumer index doorbell register pointer
 *
 * Wraps E810 descriptor completion with arm/disarm semantics and
 * callback dispatch. Shared interface enables ethernet/RDMA coexistence
 * on the same EQ.
 */
struct fdice_cq {
	struct fdice_cqwq	wq;
	void			(*comp_handler)(struct fdice_cq *cq);
	struct fdice_eq		*eq;
	struct napi_struct	*napi;
	u32			cqn;
	u16			event_ctr;
	u8			state;
	__be32			*arm_db;
	__be32			*ci_db;
};

/* -----------------------------------------------------------------------
 * TX DMA Tracking
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_dma_type - DMA mapping type for TX unmap tracking
 * @FDICE_DMA_MAP_SINGLE: dma_map_single() linear data
 * @FDICE_DMA_MAP_PAGE: dma_map_page() page fragment
 * @FDICE_DMA_MAP_PERSIST: Persistent mapping (RDMA redirect, XDP fwd)
 */
enum fdice_dma_type {
	FDICE_DMA_MAP_SINGLE = 0,
	FDICE_DMA_MAP_PAGE,
	FDICE_DMA_MAP_PERSIST,
};

/**
 * struct fdice_sq_dma - TX DMA tracking entry
 * @addr: DMA bus address
 * @size: Mapped region size in bytes
 * @type: Mapping type from &enum fdice_dma_type
 *
 * Stored in a separate circular FIFO from TX buffer descriptors for
 * cache-friendly bulk unmapping at completion time.
 */
struct fdice_sq_dma {
	dma_addr_t		addr;
	u32			size;
	u8			type;
};

/* -----------------------------------------------------------------------
 * TX Buffer Tracking
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_txb_type - TX buffer payload type
 * @FDICE_TXB_SKB: Normal network stack sk_buff
 * @FDICE_TXB_XDP: XDP frame from xdp_do_redirect
 * @FDICE_TXB_XSK: XSK (AF_XDP) zero-copy buffer
 */
enum fdice_txb_type {
	FDICE_TXB_SKB = 0,
	FDICE_TXB_XDP,
	FDICE_TXB_XSK,
};

/**
 * struct fdice_tx_buf - TX buffer tracking entry
 * @skb: sk_buff pointer (when type == FDICE_TXB_SKB)
 * @xdpf: XDP frame pointer (when type == FDICE_TXB_XDP)
 * @num_dma: Number of DMA FIFO entries for this WQE
 * @num_wqebbs: WQE basic blocks consumed
 * @type: Buffer type from &enum fdice_txb_type
 *
 * Indexed by WQ consumer counter. Records the buffer pointer and DMA
 * FIFO entry count for efficient bulk completion processing.
 */
struct fdice_tx_buf {
	union {
		struct sk_buff	*skb;
		struct xdp_frame *xdpf;
	};
	u16			num_dma;
	u16			num_wqebbs;
	u8			type;
};

/* -----------------------------------------------------------------------
 * RX Buffer Management
 * ----------------------------------------------------------------------- */

/* RX buffer flags */
#define FDICE_RXB_MAPPED	BIT(0)	/* DMA mapping is valid */
#define FDICE_RXB_HDR_SPLIT	BIT(1)	/* Buffer from header page pool */
#define FDICE_RXB_XDP		BIT(2)	/* Buffer in XDP processing */
#define FDICE_RXB_XSK		BIT(3)	/* Buffer from XSK buff pool */

/**
 * struct fdice_rx_buf - RX buffer descriptor
 * @netmem: Page pool netmem reference
 * @dma: Cached DMA address (avoids re-deriving in hot path)
 * @offset: Byte offset within the page
 * @truesize: Allocated buffer size for memory accounting
 * @flags: FDICE_RXB_* flags
 *
 * Replaces both mlx5's wqe_frag_info/frag_page indirection chain
 * (simpler) and ice's libeth_fqe (richer, with DMA address caching).
 */
struct fdice_rx_buf {
	netmem_ref		netmem;
	dma_addr_t		dma;
	u32			offset;
	u16			truesize;
	u8			flags;
};

/* -----------------------------------------------------------------------
 * Per-Queue Statistics
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_txq_stats - TX queue statistics
 * @packets: Total packets transmitted
 * @bytes: Total bytes transmitted
 * @tso_packets: TSO/GSO segmented packets
 * @tso_bytes: TSO/GSO segmented bytes
 * @linearize: Times skb linearization was required
 * @csum_partial: Hardware checksum offload count
 * @stopped: Times queue was stopped (full)
 * @wake: Times queue was woken after stop
 * @dropped: Packets dropped due to errors
 */
struct fdice_txq_stats {
	u64			packets;
	u64			bytes;
	u64			tso_packets;
	u64			tso_bytes;
	u64			linearize;
	u64			csum_partial;
	u64			stopped;
	u64			wake;
	u64			dropped;
};

/**
 * struct fdice_rxq_stats - RX queue statistics
 * @packets: Total packets received
 * @bytes: Total bytes received
 * @alloc_fail: Page pool allocation failures
 * @csum_complete: Hardware checksum verified packets
 * @csum_none: Packets with no hardware checksum
 * @csum_err: Packets with checksum errors
 * @xdp_drop: Packets dropped by XDP program
 * @xdp_tx: Packets transmitted by XDP program
 * @xdp_redirect: Packets redirected by XDP program
 * @lro_packets: LRO/GRO coalesced packets
 * @lro_bytes: LRO/GRO coalesced bytes
 */
struct fdice_rxq_stats {
	u64			packets;
	u64			bytes;
	u64			alloc_fail;
	u64			csum_complete;
	u64			csum_none;
	u64			csum_err;
	u64			xdp_drop;
	u64			xdp_tx;
	u64			xdp_redirect;
	u64			lro_packets;
	u64			lro_bytes;
};

/* -----------------------------------------------------------------------
 * Transmit Queue
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_txq - Transmit queue with cache-line-grouped layout
 *
 * Fields are grouped by access pattern:
 * - read_mostly: accessed every packet, rarely modified (ring ptrs, netdev)
 * - read_write: modified every packet (producer/consumer counters)
 * - cold: setup/teardown/stats only
 */
struct fdice_txq {
	__cacheline_group_begin_aligned(read_mostly);
	struct fdice_wq_cyc	wq;
	struct fdice_cq		cq;
	void __iomem		*tail_reg;
	struct fdice_channel	*channel;
	struct net_device	*netdev;
	__cacheline_group_end_aligned(read_mostly);

	__cacheline_group_begin_aligned(read_write);
	u32			pc;
	u32			cc;
	__cacheline_group_end_aligned(read_write);

	__cacheline_group_begin_aligned(cold);
	struct fdice_tx_buf	*tx_buf;
	struct fdice_sq_dma	*dma_fifo;
	u32			dma_fifo_pc;
	u32			dma_fifo_cc;
	u32			dma_fifo_mask;
	struct dim		dim;
	u8			tc;
	enum fdice_queue_state	state;
	struct fdice_txq_stats	stats;
	__cacheline_group_end_aligned(cold);
};

/* -----------------------------------------------------------------------
 * Receive Queue
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_rxq - Receive queue with cache-line-grouped layout
 *
 * Fields are grouped by access pattern:
 * - read_mostly: ring pointers, page pool, tail register
 * - read_write: producer/consumer indices, XDP state
 * - cold: buffer array, stats, configuration, function pointers
 */
struct fdice_rxq {
	__cacheline_group_begin_aligned(read_mostly);
	struct fdice_wq_cyc	wq;
	struct fdice_cq		cq;
	struct page_pool	*page_pool;
	struct page_pool	*hdr_page_pool;
	void __iomem		*tail_reg;
	struct fdice_channel	*channel;
	struct net_device	*netdev;
	__cacheline_group_end_aligned(read_mostly);

	__cacheline_group_begin_aligned(read_write);
	u16			next_to_use;
	u16			next_to_clean;
	struct xdp_rxq_info	xdp_rxq;
	__cacheline_group_end_aligned(read_write);

	__cacheline_group_begin_aligned(cold);
	struct fdice_rx_buf	*rx_buf;
	struct dim		dim;
	u16			count;
	u16			buf_size;
	enum fdice_queue_state	state;
	struct fdice_rxq_stats	stats;
	void			(*handle_rx_cqe)(struct fdice_rxq *rxq,
						 struct fdice_cq *cq);
	void			(*post_wqes)(struct fdice_rxq *rxq);
	__cacheline_group_end_aligned(cold);
};

/* -----------------------------------------------------------------------
 * Queue Function Declarations
 * ----------------------------------------------------------------------- */

/* Fragment buffer allocation */
int fdice_frag_buf_alloc_node(struct fdice_frag_buf *buf, int size, int node);
void fdice_frag_buf_free(struct fdice_frag_buf *buf);

/* Work queue lifecycle */
int fdice_wq_cyc_create(struct fdice_wq_cyc *wq, u32 log_sz, u16 stride,
			 int numa_node);
void fdice_wq_cyc_destroy(struct fdice_wq_cyc *wq);

/* CQ lifecycle */
int fdice_cq_alloc(struct fdice_cq *cq, u32 nent, int numa_node);
void fdice_cq_free(struct fdice_cq *cq);
void fdice_cq_arm(struct fdice_cq *cq);
void fdice_cq_disarm(struct fdice_cq *cq);

/* TX queue lifecycle */
int fdice_txq_create(struct fdice_txq *txq, struct fdice_channel *ch,
		     struct fdice_params *params, u8 tc);
void fdice_txq_destroy(struct fdice_txq *txq);
int fdice_txq_enable(struct fdice_txq *txq);
void fdice_txq_disable(struct fdice_txq *txq);

/* RX queue lifecycle */
int fdice_rxq_create(struct fdice_rxq *rxq, struct fdice_channel *ch,
		     struct fdice_params *params);
void fdice_rxq_destroy(struct fdice_rxq *rxq);
int fdice_rxq_enable(struct fdice_rxq *rxq);
void fdice_rxq_disable(struct fdice_rxq *rxq);

/* TX DMA FIFO operations */
void fdice_tx_dma_push(struct fdice_txq *txq, dma_addr_t addr,
		       u32 size, u8 type);
void fdice_tx_dma_unmap(struct fdice_txq *txq, u16 count);
void fdice_tx_dma_rollback(struct fdice_txq *txq, u16 count);

/* RX buffer operations */
int fdice_rxq_alloc_bufs(struct fdice_rxq *rxq, u16 count);
void fdice_rxq_post_wqes(struct fdice_rxq *rxq);

/* Page pool lifecycle */
int fdice_page_pool_create(struct fdice_rxq *rxq, int numa_node,
			   struct device *dev, struct net_device *netdev,
			   struct napi_struct *napi);
void fdice_page_pool_destroy(struct fdice_rxq *rxq);
int fdice_page_pool_create_hdr(struct fdice_rxq *rxq, int numa_node,
			       struct device *dev, struct net_device *netdev,
			       struct napi_struct *napi, u16 hdr_buf_size);
void fdice_page_pool_destroy_hdr(struct fdice_rxq *rxq);
int fdice_rx_alloc_bufs(struct fdice_rxq *rxq, u16 count);

#endif /* _FDICE_QUEUE_H_ */
