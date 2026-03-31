// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE NAPI Poll and TX/RX Data Path
 *
 * Implements the hot-path data plane for E810: NAPI poll loop,
 * TX completion processing via CQ, RX descriptor parsing with flex
 * descriptor metadata extraction, SKB construction, and TX submission
 * with DMA FIFO tracking.
 *
 * Architecture: mlx5-inspired EQ->CQ->WQ abstraction over E810-native
 * flex descriptors. The channel embeds TX and RX queues directly
 * for cache locality (no linked-list pointer chasing).
 *
 * Key design differences from reference drivers:
 *   - Typed DMA FIFO for TX (mlx5 pattern) instead of inline unmap metadata
 *   - Separated flex descriptor parsing into a standalone extractor
 *   - Page pool with explicit NUMA pinning and fragmentation bias
 *   - CQ arm/disarm semantics for interrupt coalescing integration
 *   - Bulk RX refill with configurable threshold
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md
 */

#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/skbuff.h>
#include <linux/dma-mapping.h>
#include <linux/prefetch.h>
#include <linux/cache.h>
#include <linux/dim.h>
#include <net/busy_poll.h>
#include <net/page_pool/helpers.h>

#include "fdice_txrx.h"
#include "fdice_queue.h"
#include "fdice_channel.h"
#include "fdice_dev.h"

/* -----------------------------------------------------------------------
 * E810 Flex RX Descriptor Bit Definitions
 * Hardware constants from E810 datasheet.
 * ----------------------------------------------------------------------- */

/* Flex descriptor status_error0 bit masks */
#define FDICE_RXD_STAT0_DD		BIT(FDICE_RX_FLEX_DESC_STATUS0_DD_S)
#define FDICE_RXD_STAT0_EOF		BIT(FDICE_RX_FLEX_DESC_STATUS0_EOF_S)
#define FDICE_RXD_STAT0_L3L4P		BIT(FDICE_RX_FLEX_DESC_STATUS0_L3L4P_S)
#define FDICE_RXD_STAT0_XSUM_IPE	BIT(FDICE_RX_FLEX_DESC_STATUS0_XSUM_IPE_S)
#define FDICE_RXD_STAT0_XSUM_L4E	BIT(FDICE_RX_FLEX_DESC_STATUS0_XSUM_L4E_S)
#define FDICE_RXD_STAT0_XSUM_EIPE	BIT(FDICE_RX_FLEX_DESC_STATUS0_XSUM_EIPE_S)
#define FDICE_RXD_STAT0_XSUM_EUDPE	BIT(FDICE_RX_FLEX_DESC_STATUS0_XSUM_EUDPE_S)
#define FDICE_RXD_STAT0_RXE		BIT(FDICE_RX_FLEX_DESC_STATUS0_RXE_S)
#define FDICE_RXD_STAT0_RSS_VALID	BIT(FDICE_RX_FLEX_DESC_STATUS0_RSS_VALID_S)
#define FDICE_RXD_STAT0_L2TAG1P	BIT(FDICE_RX_FLEX_DESC_STATUS0_L2TAG1P_S)

/* Flex descriptor status_error1 bit masks */
#define FDICE_RXD_STAT1_L2TAG2P	BIT(11)

/* Packet length field mask (14-bit) */
#define FDICE_RXD_PKT_LEN_MASK		GENMASK(13, 0)

/* Maximum data per single TX descriptor (E810 datasheet: 16KB - 1) */
#define FDICE_MAX_DATA_PER_TXD		(16 * 1024 - 1)

/* Gap descriptors needed for cache-line safety */
#define FDICE_TX_DESC_SAFETY_GAP	4

/* TX wake threshold: wake the queue when this many descriptors free */
#define FDICE_TX_WAKE_THRESHOLD		(FDICE_TX_DESC_SAFETY_GAP * 2 + 4)

/* -----------------------------------------------------------------------
 * Parsed RX Descriptor Metadata
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_rx_parsed - Extracted flex descriptor metadata
 * @ptype: 10-bit packet type
 * @pkt_len: packet length in bytes (14-bit)
 * @rss_hash: RSS hash value
 * @vlan_tci: inner VLAN TCI (from L2TAG1)
 * @vlan_tci_outer: outer VLAN TCI (from L2TAG2)
 * @ts_low: low 8 bits of hardware timestamp
 * @ts_high: high 32 bits of hardware timestamp
 * @hash_valid: RSS hash is valid
 * @vlan_valid: inner VLAN tag is present
 * @vlan_outer_valid: outer VLAN tag is present
 * @l3l4p: L3/L4 parsing was performed
 * @ip_err: IP checksum error
 * @l4_err: L4 checksum error
 * @eip_err: outer IP checksum error (tunneled)
 * @eudp_err: outer UDP checksum error (tunneled)
 * @rx_err: general receive error
 *
 * Extracted in a single pass over the descriptor for cache efficiency,
 * then applied to the skb in a separate step.
 */
struct fdice_rx_parsed {
	u16		ptype;
	u16		pkt_len;
	u32		rss_hash;
	u16		vlan_tci;
	u16		vlan_tci_outer;
	u8		ts_low;
	u32		ts_high;
	bool		hash_valid;
	bool		vlan_valid;
	bool		vlan_outer_valid;
	bool		l3l4p;
	bool		ip_err;
	bool		l4_err;
	bool		eip_err;
	bool		eudp_err;
	bool		rx_err;
};

/* -----------------------------------------------------------------------
 * TX DMA FIFO Operations
 * ----------------------------------------------------------------------- */

/**
 * fdice_tx_dma_push - Record a DMA mapping in the TX DMA FIFO
 * @txq: transmit queue
 * @addr: DMA address to record
 * @size: mapping size in bytes
 * @type: mapping type (FDICE_DMA_MAP_SINGLE or FDICE_DMA_MAP_PAGE)
 *
 * Appends a DMA tracking entry to the TX queue's circular DMA FIFO
 * at the current producer position.
 */
void fdice_tx_dma_push(struct fdice_txq *txq, dma_addr_t addr,
		       u32 size, u8 type)
{
	u32 idx = txq->dma_fifo_pc & txq->dma_fifo_mask;

	txq->dma_fifo[idx].addr = addr;
	txq->dma_fifo[idx].size = size;
	txq->dma_fifo[idx].type = type;
	txq->dma_fifo_pc++;
}

/**
 * fdice_tx_dma_unmap - Bulk unmap DMA entries from the TX DMA FIFO
 * @txq: transmit queue
 * @count: number of DMA FIFO entries to unmap
 *
 * Walks @count entries from the consumer position, unmaps each
 * according to its type, and advances the consumer counter.
 */
void fdice_tx_dma_unmap(struct fdice_txq *txq, u16 count)
{
	struct device *dev = txq->netdev->dev.parent;
	u16 i;

	for (i = 0; i < count; i++) {
		u32 idx = txq->dma_fifo_cc & txq->dma_fifo_mask;
		struct fdice_sq_dma *entry = &txq->dma_fifo[idx];

		switch (entry->type) {
		case FDICE_DMA_MAP_SINGLE:
			dma_unmap_single(dev, entry->addr, entry->size,
					 DMA_TO_DEVICE);
			break;
		case FDICE_DMA_MAP_PAGE:
			dma_unmap_page(dev, entry->addr, entry->size,
				       DMA_TO_DEVICE);
			break;
		case FDICE_DMA_MAP_PERSIST:
			/* Persistent mappings are not unmapped per-packet */
			break;
		}

		entry->type = 0;
		txq->dma_fifo_cc++;
	}
}

/**
 * fdice_tx_dma_rollback - Roll back DMA FIFO entries on error
 * @txq: transmit queue
 * @count: number of entries to roll back from current producer position
 *
 * Undoes @count DMA mappings walking backwards from the producer.
 * Used when TX descriptor posting fails partway through an skb.
 */
void fdice_tx_dma_rollback(struct fdice_txq *txq, u16 count)
{
	struct device *dev = txq->netdev->dev.parent;
	u16 i;

	for (i = 0; i < count; i++) {
		u32 idx;

		txq->dma_fifo_pc--;
		idx = txq->dma_fifo_pc & txq->dma_fifo_mask;

		switch (txq->dma_fifo[idx].type) {
		case FDICE_DMA_MAP_SINGLE:
			dma_unmap_single(dev, txq->dma_fifo[idx].addr,
					 txq->dma_fifo[idx].size,
					 DMA_TO_DEVICE);
			break;
		case FDICE_DMA_MAP_PAGE:
			dma_unmap_page(dev, txq->dma_fifo[idx].addr,
				       txq->dma_fifo[idx].size,
				       DMA_TO_DEVICE);
			break;
		default:
			break;
		}

		txq->dma_fifo[idx].type = 0;
	}
}

/* -----------------------------------------------------------------------
 * TX Completion Processing
 * ----------------------------------------------------------------------- */

/**
 * fdice_txq_desc_unused - Calculate unused TX descriptors
 * @txq: transmit queue
 *
 * Return: number of unused descriptors in the ring.
 */
static inline u32 fdice_txq_desc_unused(const struct fdice_txq *txq)
{
	return txq->wq.sz - (txq->pc - txq->cc);
}

/**
 * fdice_poll_tx_cq - Process TX completions for a single TXQ
 * @txq: transmit queue to process
 * @budget: NAPI budget (passed to napi_consume_skb for batch free)
 *
 * Walks completed WQEs from the consumer counter to the hardware-
 * reported completion point. For each completed entry, unmaps the
 * associated DMA FIFO entries and frees the skb or XDP frame.
 * Wakes the netdev TX queue if it was stopped and space is available.
 *
 * Return: number of completions processed.
 */
int fdice_poll_tx_cq(struct fdice_txq *txq, int budget)
{
	struct netdev_queue *ndq = netdev_get_tx_queue(txq->netdev,
						       txq->tc);
	u32 cons = txq->cc;
	u32 ring_mask = txq->wq.mask;
	u64 total_bytes = 0;
	u32 total_pkts = 0;
	int work = 0;

	while (work < budget) {
		struct fdice_tx_buf *buf;
		u64 cmd_type;
		u32 idx;

		idx = cons & ring_mask;
		buf = &txq->tx_buf[idx];

		/* Skip empty slots (already completed or never posted) */
		if (buf->type == FDICE_TXB_SKB && !buf->skb)
			break;
		if (buf->type != FDICE_TXB_SKB && buf->type != FDICE_TXB_XDP)
			break;

		/* Check the EOP descriptor's DTYPE for DESC_DONE.
		 * Only the last descriptor of a multi-descriptor packet
		 * has the RS (Report Status) bit, so we check that one.
		 */
		if (buf->num_wqebbs > 0) {
			u32 eop_idx = (cons + buf->num_wqebbs - 1) & ring_mask;
			void *eop_va = fdice_wq_cyc_get(&txq->wq, eop_idx);
			struct fdice_tx_desc *eop_desc = eop_va;

			cmd_type = le64_to_cpu(eop_desc->cmd_type_offset_bsz);
			if ((cmd_type & FDICE_TXD_QW1_DTYPE_M) !=
			    FDICE_TXD_DTYPE_DESC_DONE)
				break;
		}

		/* Ordering: read descriptor data only after confirming done */
		smp_rmb();

		/* Unmap all DMA entries for this WQE */
		fdice_tx_dma_unmap(txq, buf->num_dma);

		/* Free the buffer */
		switch (buf->type) {
		case FDICE_TXB_SKB:
			total_bytes += buf->skb->len;
			total_pkts++;
			napi_consume_skb(buf->skb, budget);
			break;
		case FDICE_TXB_XDP:
			total_bytes += buf->xdpf->len;
			total_pkts++;
			xdp_return_frame_rx_napi(buf->xdpf);
			break;
		default:
			break;
		}

		buf->skb = NULL;
		buf->num_dma = 0;
		cons += buf->num_wqebbs ? buf->num_wqebbs : 1;
		work++;
	}

	txq->cc = cons;

	/* Update BQL accounting */
	netdev_tx_completed_queue(ndq, total_pkts, total_bytes);

	/* Update per-queue statistics */
	txq->stats.packets += total_pkts;
	txq->stats.bytes += total_bytes;

	/* Wake the TX queue if it was stopped and space is now available */
	if (unlikely(total_pkts && netif_tx_queue_stopped(ndq) &&
		     fdice_txq_desc_unused(txq) >= FDICE_TX_WAKE_THRESHOLD)) {
		/* Ensure cc update visible before checking stopped state */
		smp_mb();
		if (netif_tx_queue_stopped(ndq)) {
			netif_tx_wake_queue(ndq);
			txq->stats.wake++;
		}
	}

	return work;
}

/* -----------------------------------------------------------------------
 * RX Flex Descriptor Parsing
 * ----------------------------------------------------------------------- */

/**
 * fdice_rx_flex_parse - Extract metadata from E810 flex RX descriptor
 * @desc: pointer to the 32-byte flex descriptor writeback
 * @parsed: output structure to fill with parsed metadata
 *
 * Decodes the E810 NIC-profile flex descriptor (RXDID 2) writeback
 * fields into a driver-internal representation. All descriptor reads
 * happen in one pass before touching any SKB memory for cache locality.
 */
static void fdice_rx_flex_parse(const union fdice_rx_flex_desc *desc,
				struct fdice_rx_parsed *parsed)
{
	u16 stat0 = le16_to_cpu(desc->wb.status_error0);
	u16 stat1 = le16_to_cpu(desc->wb.status_error1);

	/* Packet type: 10-bit field in ptype_flex_flags0 */
	parsed->ptype = le16_to_cpu(desc->wb.ptype_flex_flags0) &
			FDICE_RX_FLEX_DESC_PTYPE_M;

	/* Packet length: 14-bit field */
	parsed->pkt_len = le16_to_cpu(desc->wb.pkt_len) &
			  FDICE_RXD_PKT_LEN_MASK;

	/* RSS hash: flex_meta0/flex_meta1 form a 32-bit hash in NIC profile */
	if (stat0 & FDICE_RXD_STAT0_RSS_VALID) {
		parsed->rss_hash =
			(u32)le16_to_cpu(desc->wb.flex_meta0) |
			((u32)le16_to_cpu(desc->wb.flex_meta1) << 16);
		parsed->hash_valid = true;
	} else {
		parsed->rss_hash = 0;
		parsed->hash_valid = false;
	}

	/* L2TAG1 (inner VLAN): valid if L2TAG1P bit set */
	if (stat0 & FDICE_RXD_STAT0_L2TAG1P) {
		parsed->vlan_tci = le16_to_cpu(desc->wb.l2tag1);
		parsed->vlan_valid = true;
	} else {
		parsed->vlan_tci = 0;
		parsed->vlan_valid = false;
	}

	/* L2TAG2 (outer VLAN): valid if L2TAG2P bit set in status_error1 */
	if (stat1 & FDICE_RXD_STAT1_L2TAG2P) {
		parsed->vlan_tci_outer = le16_to_cpu(desc->wb.l2tag2_1st);
		parsed->vlan_outer_valid = true;
	} else {
		parsed->vlan_tci_outer = 0;
		parsed->vlan_outer_valid = false;
	}

	/* Checksum offload status bits */
	parsed->l3l4p = !!(stat0 & FDICE_RXD_STAT0_L3L4P);
	parsed->ip_err = !!(stat0 & FDICE_RXD_STAT0_XSUM_IPE);
	parsed->l4_err = !!(stat0 & FDICE_RXD_STAT0_XSUM_L4E);
	parsed->eip_err = !!(stat0 & FDICE_RXD_STAT0_XSUM_EIPE);
	parsed->eudp_err = !!(stat0 & FDICE_RXD_STAT0_XSUM_EUDPE);
	parsed->rx_err = !!(stat0 & FDICE_RXD_STAT0_RXE);

	/* Hardware timestamp: low byte + high 32 bits from flex_ts */
	parsed->ts_low = desc->wb.time_stamp_low;
	parsed->ts_high = le32_to_cpu(desc->wb.flex_ts.ts_high);
}

/* -----------------------------------------------------------------------
 * RX SKB Construction
 * ----------------------------------------------------------------------- */

/**
 * fdice_rx_apply_csum - Apply checksum offload status to an skb
 * @parsed: parsed descriptor metadata
 * @skb: socket buffer to annotate
 */
static void fdice_rx_apply_csum(const struct fdice_rx_parsed *parsed,
				struct sk_buff *skb)
{
	skb_checksum_none_assert(skb);

	if (!parsed->l3l4p)
		return;

	if (parsed->ip_err || parsed->l4_err ||
	    parsed->eip_err || parsed->eudp_err)
		return;

	skb->ip_summed = CHECKSUM_UNNECESSARY;
}

/**
 * fdice_rx_apply_hash - Propagate RSS hash to an skb
 * @parsed: parsed descriptor metadata
 * @skb: socket buffer to annotate
 */
static void fdice_rx_apply_hash(const struct fdice_rx_parsed *parsed,
				struct sk_buff *skb)
{
	if (!parsed->hash_valid)
		return;

	/*
	 * Determine hash type from ptype. TCP/UDP/SCTP (L4 protocols)
	 * get PKT_HASH_TYPE_L4; everything else gets L3.
	 * Bits [5:4] of ptype indicate L4 protocol presence in E810.
	 */
	skb_set_hash(skb, parsed->rss_hash,
		     (parsed->ptype & 0x30) ? PKT_HASH_TYPE_L4 :
					      PKT_HASH_TYPE_L3);
}

/**
 * fdice_rx_apply_vlan - Insert VLAN tag into skb acceleration fields
 * @parsed: parsed descriptor metadata
 * @skb: socket buffer to annotate
 */
static void fdice_rx_apply_vlan(const struct fdice_rx_parsed *parsed,
				struct sk_buff *skb)
{
	if (!parsed->vlan_valid)
		return;

	__vlan_hwaccel_put_tag(skb, htons(ETH_P_8021Q), parsed->vlan_tci);
}

/**
 * fdice_rxq_build_skb - Build an skb from a completed RX buffer
 * @rxq: receive queue
 * @rx_buf: the RX buffer containing received data
 * @parsed: pre-parsed flex descriptor metadata
 *
 * Performs DMA sync, constructs skb from page pool buffer, applies
 * all parsed metadata, and prepares the skb for stack delivery.
 *
 * Return: pointer to completed skb, or NULL on failure.
 */
static struct sk_buff *fdice_rxq_build_skb(struct fdice_rxq *rxq,
					   struct fdice_rx_buf *rx_buf,
					   const struct fdice_rx_parsed *parsed)
{
	unsigned int len = parsed->pkt_len;
	struct sk_buff *skb;
	void *va;

	/* Sync the DMA region that hardware wrote into */
	page_pool_dma_sync_for_cpu(rxq->page_pool,
				   netmem_to_page(rx_buf->netmem),
				   rx_buf->offset, len);

	va = page_address(netmem_to_page(rx_buf->netmem)) + rx_buf->offset;

	/* Prefetch first cache line of packet data */
	prefetch(va);

	/* Use napi_build_skb for NAPI context allocation efficiency */
	skb = napi_build_skb(va, rx_buf->truesize);
	if (unlikely(!skb))
		return NULL;

	/* Register page with page pool for recycling on skb free */
	skb_mark_for_recycle(skb);

	skb_put(skb, len);
	skb->protocol = eth_type_trans(skb, rxq->netdev);

	/* Apply all offload metadata */
	fdice_rx_apply_csum(parsed, skb);
	fdice_rx_apply_hash(parsed, skb);
	fdice_rx_apply_vlan(parsed, skb);

	/* Record receive queue for RFS/RPS */
	skb_record_rx_queue(skb, rxq->channel->idx);

	return skb;
}

/* -----------------------------------------------------------------------
 * RX Ring Refill
 * ----------------------------------------------------------------------- */

/**
 * fdice_rxq_alloc_bufs - Allocate RX buffers from the page pool
 * @rxq: receive queue
 * @count: number of buffers to allocate
 *
 * Return: number of buffers successfully allocated.
 */
int fdice_rxq_alloc_bufs(struct fdice_rxq *rxq, u16 count)
{
	struct page_pool *pool = rxq->page_pool;
	u16 allocated = 0;
	u16 i;

	for (i = 0; i < count; i++) {
		u16 idx = (rxq->next_to_use + allocated) & (rxq->count - 1);
		struct fdice_rx_buf *buf = &rxq->rx_buf[idx];
		netmem_ref netmem;
		dma_addr_t dma;

		netmem = page_pool_dev_alloc_netmems(pool);
		if (unlikely(!netmem))
			break;

		dma = page_pool_get_dma_addr_netmem(netmem);

		buf->netmem = netmem;
		buf->dma = dma;
		buf->offset = 0;
		buf->truesize = rxq->buf_size;
		buf->flags = FDICE_RXB_MAPPED;

		allocated++;
	}

	return allocated;
}

/**
 * fdice_rxq_post_wqes - Bulk refill RX ring from page pool
 * @rxq: receive queue to refill
 *
 * Calculates how many descriptors need refilling. If the count
 * exceeds the refill threshold, allocates pages and posts new
 * descriptors. Writes the tail register to notify hardware.
 */
void fdice_rxq_post_wqes(struct fdice_rxq *rxq)
{
	u16 ntu = rxq->next_to_use;
	u16 ntc = rxq->next_to_clean;
	u16 ring_sz = rxq->count;
	u16 missing = ring_sz - ((ntu - ntc) & (ring_sz - 1));
	u16 posted = 0;
	u16 i;

	if (missing < FDICE_RX_REFILL_THRESHOLD)
		return;

	for (i = 0; i < missing; i++) {
		u16 idx = ntu & (ring_sz - 1);
		struct fdice_rx_buf *buf = &rxq->rx_buf[idx];
		union fdice_rx_flex_desc *desc;
		netmem_ref netmem;
		dma_addr_t dma;

		netmem = page_pool_dev_alloc_netmems(rxq->page_pool);
		if (unlikely(!netmem))
			break;

		dma = page_pool_get_dma_addr_netmem(netmem);

		buf->netmem = netmem;
		buf->dma = dma;
		buf->offset = 0;
		buf->truesize = rxq->buf_size;
		buf->flags = FDICE_RXB_MAPPED;

		/* Write the RX descriptor in read format.
		 * Bit 0 of hdr_addr is DD; cleared = available to HW.
		 */
		desc = fdice_wq_cyc_get(&rxq->wq, idx);
		desc->read.pkt_addr = cpu_to_le64(dma);
		desc->read.hdr_addr = 0;

		ntu++;
		posted++;
	}

	if (likely(posted)) {
		rxq->next_to_use = ntu;
		/* Ensure descriptor writes visible before tail update */
		wmb();
		writel(ntu & (ring_sz - 1), rxq->tail_reg);
	}
}

/* -----------------------------------------------------------------------
 * RX Completion Processing
 * ----------------------------------------------------------------------- */

/**
 * fdice_poll_rx_cq - Process RX completions for a receive queue
 * @rxq: receive queue to poll
 * @budget: maximum packets to process
 *
 * Iterates over completed flex descriptors. For each: checks DD bit,
 * enforces memory ordering, parses metadata, builds skb, and delivers
 * via napi_gro_receive. Triggers bulk refill when done.
 *
 * Return: number of packets processed (0..budget).
 */
int fdice_poll_rx_cq(struct fdice_rxq *rxq, int budget)
{
	u16 ntc = rxq->next_to_clean;
	u16 ring_mask = rxq->count - 1;
	u32 total_pkts = 0;
	u64 total_bytes = 0;

	while (likely(total_pkts < (unsigned int)budget)) {
		union fdice_rx_flex_desc *desc;
		struct fdice_rx_buf *rx_buf;
		struct fdice_rx_parsed parsed;
		struct sk_buff *skb;
		u16 stat0;
		u16 idx;

		idx = ntc & ring_mask;
		desc = fdice_wq_cyc_get(&rxq->wq, idx);

		/* Check DD (Descriptor Done) bit */
		stat0 = le16_to_cpu(desc->wb.status_error0);
		if (!(stat0 & FDICE_RXD_STAT0_DD))
			break;

		/* All descriptor reads must happen after observing DD */
		dma_rmb();

		/* Prefetch next descriptor for the next iteration */
		prefetch(fdice_wq_cyc_get(&rxq->wq, (ntc + 1) & ring_mask));

		/* Check for receive errors -- drop errored frames */
		if (unlikely(stat0 & FDICE_RXD_STAT0_RXE)) {
			rx_buf = &rxq->rx_buf[idx];
			page_pool_put_full_netmem(rxq->page_pool,
						  rx_buf->netmem, false);
			rx_buf->netmem = 0;
			rxq->stats.csum_err++;
			ntc++;
			continue;
		}

		/* Parse all metadata from flex descriptor in one pass */
		fdice_rx_flex_parse(desc, &parsed);

		rx_buf = &rxq->rx_buf[idx];

		/* Build skb from buffer with parsed metadata */
		skb = fdice_rxq_build_skb(rxq, rx_buf, &parsed);
		if (unlikely(!skb)) {
			page_pool_put_full_netmem(rxq->page_pool,
						  rx_buf->netmem, false);
			rx_buf->netmem = 0;
			rxq->stats.alloc_fail++;
			ntc++;
			continue;
		}

		/* Buffer ownership transferred to skb */
		rx_buf->netmem = 0;

		/* Prefetch skb->users for napi_gro_receive fast path */
		prefetchw(&skb->users);

		total_bytes += skb->len;
		total_pkts++;

		/* Deliver to network stack via GRO */
		napi_gro_receive(rxq->cq.napi, skb);

		ntc++;
	}

	rxq->next_to_clean = ntc;

	/* Update per-queue statistics */
	rxq->stats.packets += total_pkts;
	rxq->stats.bytes += total_bytes;

	/* Bulk refill the RX ring */
	fdice_rxq_post_wqes(rxq);

	return total_pkts;
}

/* -----------------------------------------------------------------------
 * NAPI Poll
 * ----------------------------------------------------------------------- */

/**
 * fdice_napi_poll - Main NAPI poll function for a channel
 * @napi: NAPI context
 * @budget: maximum RX packets to process this poll cycle
 *
 * Called by NAPI when the channel's interrupt fires or during busy-poll.
 * Processes all TX CQs first (one per TC), then polls the RX CQ.
 *
 * Completion logic:
 *   - If work remains, keeps polling (returns budget)
 *   - If all done, calls napi_complete_done, submits DIM sample,
 *     and re-arms the CQ interrupt via fdice_cq_arm
 *
 * Return: number of RX packets processed, clamped to budget.
 */
int fdice_napi_poll(struct napi_struct *napi, int budget)
{
	struct fdice_channel *ch = container_of(napi, struct fdice_channel,
						napi);
	u8 num_tc = ch->params ? ch->params->num_tc : 1;
	bool tx_complete = true;
	int rx_work = 0;
	int tc;

	/*
	 * Phase 1: Clean all TX CQs -- one per traffic class.
	 * TX completions don't count against the NAPI RX budget.
	 */
	for (tc = 0; tc < num_tc; tc++) {
		if (fdice_poll_tx_cq(&ch->txq[tc], budget) == 0)
			tx_complete = false;
	}

	/*
	 * Phase 2: Process RX CQ if budget allows.
	 * Budget of 0 = netpoll context, only clean TX.
	 */
	if (likely(budget > 0))
		rx_work = fdice_poll_rx_cq(&ch->rxq, budget);

	/* Phase 3: Determine completion status */
	if (!tx_complete || rx_work >= budget)
		return budget;

	/* All work done -- exit NAPI poll mode */
	if (napi_complete_done(napi, rx_work)) {
		/*
		 * Phase 4: DIM (Dynamic Interrupt Moderation) sample.
		 * DIM uses recent packet/byte counts to tune coalescing.
		 */
		struct dim_sample dim_sample = {};

		dim_update_sample(ch->rxq.cq.event_ctr++,
				  ch->rxq.stats.packets,
				  ch->rxq.stats.bytes,
				  &dim_sample);
		net_dim(&ch->rxq.dim, &dim_sample);

		/*
		 * Phase 5: Re-arm the completion queue interrupts.
		 * This enables the next interrupt for this channel.
		 */
		fdice_cq_arm(&ch->rxq.cq);
	}

	return min_t(int, rx_work, budget - 1);
}

/* -----------------------------------------------------------------------
 * TX Submission (ndo_start_xmit helper)
 * ----------------------------------------------------------------------- */

/**
 * fdice_txd_count - Calculate descriptors needed for a data buffer
 * @size: buffer size in bytes
 *
 * Return: number of TX descriptors needed.
 */
static inline u32 fdice_txd_count(unsigned int size)
{
	return DIV_ROUND_UP(size, FDICE_MAX_DATA_PER_TXD);
}

/**
 * fdice_xmit_desc_count - Count total descriptors needed for an skb
 * @skb: socket buffer to transmit
 *
 * Return: total descriptor count for head + all fragments.
 */
static unsigned int fdice_xmit_desc_count(const struct sk_buff *skb)
{
	unsigned int count = fdice_txd_count(skb_headlen(skb));
	unsigned int i;

	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++)
		count += fdice_txd_count(skb_frag_size(
			&skb_shinfo(skb)->frags[i]));

	return count;
}

/**
 * fdice_maybe_stop_tx - Check if the TX ring has enough space
 * @txq: transmit queue
 * @ndq: netdev TX queue
 * @size: descriptors needed
 *
 * Return: 0 if space available, -EBUSY if the queue was stopped.
 */
static int fdice_maybe_stop_tx(struct fdice_txq *txq,
			       struct netdev_queue *ndq,
			       unsigned int size)
{
	if (likely(fdice_txq_desc_unused(txq) >= size))
		return 0;

	netif_tx_stop_queue(ndq);

	/* Re-check after stopping in case completions raced */
	smp_mb();
	if (likely(fdice_txq_desc_unused(txq) < size))
		return -EBUSY;

	netif_tx_wake_queue(ndq);
	return 0;
}

/**
 * fdice_xmit_frame - Prepare and submit TX descriptors for an skb
 * @skb: socket buffer to transmit
 * @txq: transmit queue
 *
 * Main TX entry point called from ndo_start_xmit context. Linearizes
 * the skb if needed, maps all data via DMA, writes TX descriptors
 * with proper offload encoding, and rings the hardware doorbell.
 *
 * Return: NETDEV_TX_OK on success, NETDEV_TX_BUSY if the ring is full.
 */
netdev_tx_t fdice_xmit_frame(struct sk_buff *skb, struct fdice_txq *txq)
{
	unsigned int desc_count = fdice_xmit_desc_count(skb);
	struct device *dev = txq->netdev->dev.parent;
	struct netdev_queue *ndq;
	u32 ring_mask = txq->wq.mask;
	struct fdice_tx_buf *first_buf;
	dma_addr_t dma;
	u32 prod;
	u16 dma_entries = 0;
	unsigned int i;

	ndq = netdev_get_tx_queue(txq->netdev, txq->tc);

	/* Check if linearization is needed */
	if (desc_count > FDICE_TX_DESC_SAFETY_GAP &&
	    skb_shinfo(skb)->nr_frags > 0) {
		if (__skb_linearize(skb)) {
			dev_kfree_skb_any(skb);
			txq->stats.dropped++;
			return NETDEV_TX_OK;
		}
		desc_count = fdice_txd_count(skb->len);
		txq->stats.linearize++;
	}

	/* Ensure enough descriptors: data + safety gap */
	if (fdice_maybe_stop_tx(txq, ndq,
				desc_count + FDICE_TX_DESC_SAFETY_GAP)) {
		txq->stats.stopped++;
		return NETDEV_TX_BUSY;
	}

	/* Prefetch BQL state */
	netdev_txq_bql_enqueue_prefetchw(ndq);

	prod = txq->pc;

	/* Record first TX buffer entry for completion tracking */
	first_buf = &txq->tx_buf[prod & ring_mask];
	first_buf->skb = skb;
	first_buf->type = FDICE_TXB_SKB;
	first_buf->num_dma = 0;
	first_buf->num_wqebbs = desc_count;

	/* Map the linear head data */
	dma = dma_map_single(dev, skb->data, skb_headlen(skb),
			     DMA_TO_DEVICE);
	if (dma_mapping_error(dev, dma))
		goto err_dma;

	fdice_tx_dma_push(txq, dma, skb_headlen(skb), FDICE_DMA_MAP_SINGLE);
	dma_entries++;

	/* Write head descriptor. Only the last descriptor gets EOP+RS. */
	{
		struct fdice_tx_desc *desc;
		u16 cmd = 0;

		if (skb_shinfo(skb)->nr_frags == 0)
			cmd = FDICE_TX_DESC_CMD_EOP | FDICE_TX_DESC_CMD_RS;

		desc = fdice_wq_cyc_get(&txq->wq, prod & ring_mask);
		desc->buf_addr = cpu_to_le64(dma);
		desc->cmd_type_offset_bsz = fdice_tx_desc_encode(
			FDICE_TXD_DTYPE_DATA, cmd, 0, skb_headlen(skb));
	}
	prod++;

	/* Map and post each skb fragment */
	for (i = 0; i < skb_shinfo(skb)->nr_frags; i++) {
		skb_frag_t *frag = &skb_shinfo(skb)->frags[i];
		struct fdice_tx_desc *desc;
		unsigned int frag_size = skb_frag_size(frag);
		u16 cmd = 0;

		dma = skb_frag_dma_map(dev, frag, 0, frag_size,
				       DMA_TO_DEVICE);
		if (dma_mapping_error(dev, dma))
			goto err_dma_frag;

		fdice_tx_dma_push(txq, dma, frag_size, FDICE_DMA_MAP_PAGE);
		dma_entries++;

		/* Last fragment gets EOP + RS */
		if (i == skb_shinfo(skb)->nr_frags - 1)
			cmd = FDICE_TX_DESC_CMD_EOP | FDICE_TX_DESC_CMD_RS;

		desc = fdice_wq_cyc_get(&txq->wq, prod & ring_mask);
		desc->buf_addr = cpu_to_le64(dma);
		desc->cmd_type_offset_bsz = fdice_tx_desc_encode(
			FDICE_TXD_DTYPE_DATA, cmd, 0, frag_size);
		prod++;
	}

	first_buf->num_dma = dma_entries;

	/* BQL enqueue accounting */
	netdev_tx_sent_queue(ndq, skb->len);

	txq->pc = prod;

	/* Ensure descriptor writes globally visible before doorbell */
	wmb();

	/* Ring the TX doorbell */
	writel(prod & ring_mask, txq->tail_reg);

	return NETDEV_TX_OK;

err_dma_frag:
	fdice_tx_dma_rollback(txq, dma_entries);
	first_buf->skb = NULL;
	dev_kfree_skb_any(skb);
	txq->stats.dropped++;
	return NETDEV_TX_OK;

err_dma:
	first_buf->skb = NULL;
	dev_kfree_skb_any(skb);
	txq->stats.dropped++;
	return NETDEV_TX_OK;
}
