// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_queue.c - Queue allocation, lifecycle, and DMA management
 *
 * Phase 3.3 implementation: work queue ring allocation, completion queue
 * creation, TX/RX queue lifecycle with proper state machine transitions.
 *
 * Design rationale:
 *   - Three-level EQ/CQ/WQ hierarchy (mlx5-inspired) layered above E810
 *     native descriptor rings. CQs are embedded in TXQ/RXQ for cache
 *     locality; WQs use cyclic (power-of-2) rings with fragment buffer
 *     control for DMA management.
 *   - Page pool per RXQ with NUMA pinning and fragmentation bias for
 *     efficient small-packet memory utilization.
 *   - Typed DMA FIFO for TX tracks DMA mappings separately from buffer
 *     descriptors, enabling cache-friendly bulk completion.
 *   - Unified state machine: UNINIT -> ALLOCATED -> CONFIGURED -> ACTIVE
 *     -> DISABLED -> DESTROYED, with goto-chain cleanup at each stage.
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/dma-mapping.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/io.h>
#include <linux/xarray.h>
#include <linux/log2.h>
#include <net/page_pool/helpers.h>

#include "fdice_dev.h"
#include "fdice_queue.h"
#include "fdice_eq.h"
#include "fdice_channel.h"

/* ---- E810 Queue Register Offsets ---- */

/* TX queue tail register */
#define FDICE_REG_QTX_TAIL(q)		(0x000E0000 + ((q) * 0x4))
/* RX queue tail register */
#define FDICE_REG_QRX_TAIL(q)		(0x00200000 + ((q) * 0x4))

/* Queue enable/disable registers */
#define FDICE_REG_QTX_ENA(q)		(0x00100000 + ((q) * 0x4))
#define FDICE_REG_QRX_ENA(q)		(0x00120000 + ((q) * 0x4))
#define FDICE_REG_QUEUE_ENA_REQ		BIT(0)
#define FDICE_REG_QUEUE_ENA_STAT	BIT(2)

/* Default DMA FIFO depth (must be power of 2) */
#define FDICE_DMA_FIFO_SZ		1024
#define FDICE_DMA_FIFO_MASK		(FDICE_DMA_FIFO_SZ - 1)

/* Page pool fragmentation bias */
#define FDICE_PAGECNT_BIAS		(PAGE_SIZE / 64)

/* Queue enable/disable polling */
#define FDICE_QUEUE_WAIT_RETRIES	100
#define FDICE_QUEUE_WAIT_US		10

/* ---- Fragment Buffer Allocation ---- */

/**
 * fdice_frag_buf_alloc_node - Allocate a fragmented DMA buffer
 * @buf: fragment buffer structure to initialize
 * @size: total buffer size in bytes
 * @node: NUMA node for allocation
 *
 * Allocates DMA-coherent memory using page-sized fragments rather than
 * a single contiguous allocation. This avoids high-order page allocation
 * failures for large rings while maintaining DMA-coherent guarantees.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_frag_buf_alloc_node(struct fdice_frag_buf *buf, int size, int node)
{
	int npages;
	int i;

	npages = DIV_ROUND_UP(size, PAGE_SIZE);

	buf->buf = kvcalloc_node(npages, sizeof(*buf->buf), GFP_KERNEL, node);
	if (!buf->buf)
		return -ENOMEM;

	buf->dma = kvcalloc_node(npages, sizeof(*buf->dma), GFP_KERNEL, node);
	if (!buf->dma)
		goto err_free_buf;

	for (i = 0; i < npages; i++) {
		buf->buf[i] = dma_alloc_coherent(NULL, PAGE_SIZE,
						 &buf->dma[i],
						 GFP_KERNEL | __GFP_ZERO);
		if (!buf->buf[i])
			goto err_free_pages;
	}

	buf->size = size;
	buf->npages = npages;
	return 0;

err_free_pages:
	while (i > 0) {
		i--;
		dma_free_coherent(NULL, PAGE_SIZE, buf->buf[i], buf->dma[i]);
	}
	kvfree(buf->dma);
	buf->dma = NULL;
err_free_buf:
	kvfree(buf->buf);
	buf->buf = NULL;
	return -ENOMEM;
}

/**
 * fdice_frag_buf_free - Free a fragmented DMA buffer
 * @buf: fragment buffer to free
 *
 * Frees all DMA-coherent page fragments and the tracking arrays.
 * Safe to call on a zeroed or already-freed buffer.
 */
void fdice_frag_buf_free(struct fdice_frag_buf *buf)
{
	int i;

	if (!buf->buf)
		return;

	for (i = 0; i < buf->npages; i++) {
		if (buf->buf[i])
			dma_free_coherent(NULL, PAGE_SIZE,
					  buf->buf[i], buf->dma[i]);
	}

	kvfree(buf->dma);
	kvfree(buf->buf);
	buf->buf = NULL;
	buf->dma = NULL;
	buf->npages = 0;
	buf->size = 0;
}

/* ---- Fragment Buffer Control Init ---- */

/**
 * fdice_fbc_init - Initialize fragment buffer control for ring indexing
 * @fbc: fragment buffer control to initialize
 * @log_stride: log2 of WQE stride size
 *
 * Computes the shift-based arithmetic parameters for translating ring
 * indices to virtual and DMA addresses within the fragmented buffer.
 */
static void fdice_fbc_init(struct fdice_frag_buf_ctrl *fbc, u8 log_stride)
{
	fbc->log_stride = log_stride;
	fbc->frag_sz_shift = PAGE_SHIFT;
	fbc->strides_per_frag = PAGE_SIZE >> log_stride;
	fbc->frag_mask = PAGE_SIZE - 1;
}

/* ---- Work Queue (Cyclic Ring) Operations ---- */

/**
 * fdice_wq_cyc_create - Allocate a cyclic work queue ring
 * @wq: work queue structure to initialize
 * @log_sz: log2 of ring size in entries
 * @stride: WQE size in bytes
 * @numa_node: NUMA node for memory allocation
 *
 * Allocates the underlying fragment buffer for the descriptor ring with
 * NUMA-aware placement. Ring size must be a power of 2 (specified as log2).
 * Producer and consumer counters are initialized to zero.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_wq_cyc_create(struct fdice_wq_cyc *wq, u32 log_sz, u16 stride,
			int numa_node)
{
	u32 sz = 1 << log_sz;
	int ring_bytes;
	int err;

	memset(wq, 0, sizeof(*wq));

	ring_bytes = sz * stride;
	err = fdice_frag_buf_alloc_node(&wq->fbc.frag_buf, ring_bytes,
					numa_node);
	if (err)
		return err;

	fdice_fbc_init(&wq->fbc, ilog2(stride));

	wq->sz = sz;
	wq->mask = sz - 1;
	wq->stride = stride;
	wq->pc = 0;
	wq->cc = 0;
	wq->db = NULL;

	return 0;
}

/**
 * fdice_wq_cyc_destroy - Free a cyclic work queue ring
 * @wq: work queue to destroy
 *
 * Frees the underlying fragment buffer and resets the work queue state.
 * Safe to call on a zeroed or already-destroyed wq.
 */
void fdice_wq_cyc_destroy(struct fdice_wq_cyc *wq)
{
	fdice_frag_buf_free(&wq->fbc.frag_buf);
	wq->sz = 0;
	wq->mask = 0;
	wq->pc = 0;
	wq->cc = 0;
}

/* ---- Completion Queue Operations ---- */

/**
 * fdice_cq_alloc - Allocate CQ ring memory
 * @cq: completion queue to initialize
 * @nent: number of CQ entries (power of 2)
 * @numa_node: NUMA node for allocation
 *
 * Allocates the CQ work queue ring buffer for reading completion entries.
 * CQ registration with a parent EQ is done separately via fdice_eq_add_cq().
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_cq_alloc(struct fdice_cq *cq, u32 nent, int numa_node)
{
	u8 log_cqe_sz = 6;	/* 64-byte CQEs */
	int ring_bytes;
	int err;

	memset(cq, 0, sizeof(*cq));

	ring_bytes = nent << log_cqe_sz;
	err = fdice_frag_buf_alloc_node(&cq->wq.fbc.frag_buf, ring_bytes,
					numa_node);
	if (err)
		return err;

	fdice_fbc_init(&cq->wq.fbc, log_cqe_sz);

	cq->wq.sz = nent;
	cq->wq.mask = nent - 1;
	cq->wq.cc = 0;
	cq->wq.log_cqe_sz = log_cqe_sz;

	cq->state = 0;
	cq->event_ctr = 0;

	return 0;
}

/**
 * fdice_cq_free - Free CQ ring memory
 * @cq: completion queue to free
 *
 * Frees the CQ ring buffer. The CQ must be removed from its parent EQ
 * (via fdice_eq_del_cq) before calling this.
 */
void fdice_cq_free(struct fdice_cq *cq)
{
	fdice_frag_buf_free(&cq->wq.fbc.frag_buf);
	cq->wq.sz = 0;
}

/**
 * fdice_cq_arm - Arm a CQ to generate interrupts through its parent EQ
 * @cq: completion queue to arm
 *
 * Sets the arm bit so that new completions generate an interrupt through
 * the parent EQ's MSI-X vector. Called at the end of a NAPI poll cycle.
 */
void fdice_cq_arm(struct fdice_cq *cq)
{
	if (cq->arm_db)
		WRITE_ONCE(*cq->arm_db, cpu_to_be32(cq->event_ctr));
	cq->state = 1; /* armed */
}

/**
 * fdice_cq_disarm - Disarm a CQ to suppress interrupts
 * @cq: completion queue to disarm
 *
 * Clears the arm state. Used when entering NAPI busy-poll mode or
 * during queue teardown.
 */
void fdice_cq_disarm(struct fdice_cq *cq)
{
	cq->state = 0; /* disarmed */
}

/* ---- TX Queue Operations ---- */

/**
 * fdice_txq_create - Allocate and initialize a transmit queue
 * @txq: transmit queue structure to initialize
 * @ch: parent channel providing CPU/NUMA/NAPI context
 * @params: channel parameters (ring sizes, coalescing)
 * @tc: traffic class index for this queue
 *
 * Allocates the TX descriptor ring (WQ), completion queue (CQ), DMA
 * tracking FIFO, and buffer tracking array. All resources are NUMA-pinned
 * to the channel's node for optimal data-path performance.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_txq_create(struct fdice_txq *txq, struct fdice_channel *ch,
		     struct fdice_params *params, u8 tc)
{
	int numa_node = ch->numa_node;
	u32 log_sz;
	int err;

	memset(txq, 0, sizeof(*txq));
	txq->channel = ch;
	txq->netdev = ch->priv->netdev;
	txq->tc = tc;
	txq->state = FDICE_QS_UNINIT;

	/* Convert ring size to log2 */
	log_sz = ilog2(params->tx_ring_size);

	/* Allocate TX descriptor ring via fragment buffer */
	err = fdice_wq_cyc_create(&txq->wq, log_sz,
				  sizeof(u64) * 2, /* 16-byte TX desc */
				  numa_node);
	if (err)
		return err;

	/* Allocate CQ for TX completions */
	err = fdice_cq_alloc(&txq->cq, params->tx_ring_size, numa_node);
	if (err)
		goto err_free_wq;

	txq->cq.comp_handler = NULL; /* set during enable */
	txq->cq.napi = &ch->napi;

	/* Allocate DMA tracking FIFO */
	txq->dma_fifo = kvzalloc_node(FDICE_DMA_FIFO_SZ *
				      sizeof(struct fdice_sq_dma),
				      GFP_KERNEL, numa_node);
	if (!txq->dma_fifo) {
		err = -ENOMEM;
		goto err_free_cq;
	}
	txq->dma_fifo_pc = 0;
	txq->dma_fifo_cc = 0;
	txq->dma_fifo_mask = FDICE_DMA_FIFO_MASK;

	/* Allocate TX buffer tracking array */
	txq->tx_buf = kvzalloc_node(params->tx_ring_size *
				    sizeof(struct fdice_tx_buf),
				    GFP_KERNEL, numa_node);
	if (!txq->tx_buf) {
		err = -ENOMEM;
		goto err_free_dma_fifo;
	}

	txq->pc = 0;
	txq->cc = 0;
	txq->state = FDICE_QS_ALLOCATED;

	return 0;

err_free_dma_fifo:
	kvfree(txq->dma_fifo);
	txq->dma_fifo = NULL;
err_free_cq:
	fdice_cq_free(&txq->cq);
err_free_wq:
	fdice_wq_cyc_destroy(&txq->wq);
	return err;
}

/**
 * fdice_txq_destroy - Free all TX queue resources
 * @txq: transmit queue to destroy
 *
 * Releases buffer tracking array, DMA FIFO, completion queue, and
 * descriptor ring in reverse allocation order. Frees any remaining
 * SKBs or XDP frames before releasing the buffer array.
 */
void fdice_txq_destroy(struct fdice_txq *txq)
{
	u32 i;

	if (txq->state == FDICE_QS_UNINIT || txq->state == FDICE_QS_DESTROYED)
		return;

	/* Free any remaining TX buffers */
	if (txq->tx_buf) {
		for (i = 0; i < txq->wq.sz; i++) {
			struct fdice_tx_buf *buf = &txq->tx_buf[i];

			if (buf->type == FDICE_TXB_SKB && buf->skb) {
				dev_kfree_skb_any(buf->skb);
				buf->skb = NULL;
			} else if (buf->type == FDICE_TXB_XDP && buf->xdpf) {
				xdp_return_frame(buf->xdpf);
				buf->xdpf = NULL;
			}
		}
		kvfree(txq->tx_buf);
		txq->tx_buf = NULL;
	}

	/* Unmap any remaining DMA entries */
	if (txq->dma_fifo) {
		fdice_tx_dma_unmap(txq, txq->dma_fifo_pc - txq->dma_fifo_cc);
		kvfree(txq->dma_fifo);
		txq->dma_fifo = NULL;
	}

	fdice_cq_free(&txq->cq);
	fdice_wq_cyc_destroy(&txq->wq);

	txq->state = FDICE_QS_DESTROYED;
}

/**
 * fdice_txq_enable - Configure and activate a TX queue in hardware
 * @txq: transmit queue to enable
 *
 * Maps the tail register, enables the queue in hardware, and polls for
 * hardware confirmation. Sets the queue state to ACTIVE on success.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_txq_enable(struct fdice_txq *txq)
{
	struct fdice_channel *ch = txq->channel;
	struct fdice_dev *fdev = ch->priv->fdev;
	struct fdice_hw *hw = &fdev->hw;
	u16 qidx;
	u32 reg_val;
	int i;

	if (txq->state != FDICE_QS_ALLOCATED &&
	    txq->state != FDICE_QS_CONFIGURED)
		return -EINVAL;

	/* Compute global queue index: tc * num_channels + channel_idx */
	qidx = (u16)(txq->tc * ch->priv->num_channels + ch->idx);

	/* Map tail register */
	txq->tail_reg = hw->hw_addr + FDICE_REG_QTX_TAIL(qidx);

	/* Enable the TX queue in hardware */
	writel(FDICE_REG_QUEUE_ENA_REQ,
	       hw->hw_addr + FDICE_REG_QTX_ENA(qidx));

	/* Poll for hardware confirmation */
	for (i = 0; i < FDICE_QUEUE_WAIT_RETRIES; i++) {
		reg_val = readl(hw->hw_addr + FDICE_REG_QTX_ENA(qidx));
		if (reg_val & FDICE_REG_QUEUE_ENA_STAT)
			break;
		udelay(FDICE_QUEUE_WAIT_US);
	}

	if (!(reg_val & FDICE_REG_QUEUE_ENA_STAT)) {
		dev_err(&hw->pdev->dev, "TX queue %u enable timed out\n",
			qidx);
		return -ETIMEDOUT;
	}

	/* Write initial tail pointer */
	writel(0, txq->tail_reg);

	txq->state = FDICE_QS_ACTIVE;
	return 0;
}

/**
 * fdice_txq_disable - Disable a TX queue in hardware
 * @txq: transmit queue to disable
 *
 * Disables the TX queue and waits for hardware drain confirmation.
 */
void fdice_txq_disable(struct fdice_txq *txq)
{
	struct fdice_channel *ch = txq->channel;
	struct fdice_dev *fdev = ch->priv->fdev;
	struct fdice_hw *hw = &fdev->hw;
	u16 qidx;
	u32 reg_val;
	int i;

	if (txq->state != FDICE_QS_ACTIVE)
		return;

	qidx = (u16)(txq->tc * ch->priv->num_channels + ch->idx);

	/* Request queue disable */
	writel(0, hw->hw_addr + FDICE_REG_QTX_ENA(qidx));

	/* Wait for hardware drain */
	for (i = 0; i < FDICE_QUEUE_WAIT_RETRIES; i++) {
		reg_val = readl(hw->hw_addr + FDICE_REG_QTX_ENA(qidx));
		if (!(reg_val & FDICE_REG_QUEUE_ENA_STAT))
			break;
		udelay(FDICE_QUEUE_WAIT_US);
	}

	if (reg_val & FDICE_REG_QUEUE_ENA_STAT)
		dev_warn(&hw->pdev->dev,
			 "TX queue %u disable timed out\n", qidx);

	txq->state = FDICE_QS_DISABLED;
}

/* ---- RX Queue Operations ---- */

/**
 * fdice_create_page_pool - Create a NUMA-aware page pool for RX buffers
 * @dev: PCI device for DMA mapping
 * @netdev: network device
 * @napi: NAPI instance for page pool integration
 * @pool_size: number of pages in the pool
 * @numa_node: preferred NUMA node
 *
 * Creates a page pool with PP_FLAG_DMA_MAP and PP_FLAG_DMA_SYNC_DEV for
 * hardware DMA mapping. NUMA pinning ensures buffer memory is local to
 * the CPU processing packets on this queue.
 *
 * Return: pointer to page_pool on success, ERR_PTR on failure.
 */
static struct page_pool *
fdice_create_page_pool(struct device *dev, struct net_device *netdev,
		       struct napi_struct *napi, u32 pool_size, int numa_node)
{
	struct page_pool_params pp_params = {
		.flags		= PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
		.order		= 0,
		.pool_size	= pool_size,
		.nid		= numa_node,
		.dev		= dev,
		.dma_dir	= DMA_FROM_DEVICE,
		.netdev		= netdev,
		.napi		= napi,
	};

	return page_pool_create(&pp_params);
}

/**
 * fdice_rxq_create - Allocate and initialize a receive queue
 * @rxq: receive queue structure to initialize
 * @ch: parent channel providing CPU/NUMA/NAPI context
 * @params: channel parameters (ring sizes, buffer config)
 *
 * Allocates the RX descriptor ring (WQ), completion queue (CQ), page pool
 * with NUMA-aware DMA mapping, and RX buffer descriptor array.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_rxq_create(struct fdice_rxq *rxq, struct fdice_channel *ch,
		     struct fdice_params *params)
{
	struct device *dev = ch->priv->netdev->dev.parent;
	int numa_node = ch->numa_node;
	u32 log_sz;
	int err;

	memset(rxq, 0, sizeof(*rxq));
	rxq->channel = ch;
	rxq->netdev = ch->priv->netdev;
	rxq->count = params->rx_ring_size;
	rxq->buf_size = params->rx_buf_size;
	rxq->state = FDICE_QS_UNINIT;

	log_sz = ilog2(params->rx_ring_size);

	/* Allocate RX descriptor ring via fragment buffer */
	err = fdice_wq_cyc_create(&rxq->wq, log_sz,
				  32, /* 32-byte flex RX desc */
				  numa_node);
	if (err)
		return err;

	/* Allocate CQ for RX completions */
	err = fdice_cq_alloc(&rxq->cq, params->rx_ring_size, numa_node);
	if (err)
		goto err_free_wq;

	rxq->cq.comp_handler = NULL; /* set during enable */
	rxq->cq.napi = &ch->napi;

	/* Create NUMA-aware page pool */
	rxq->page_pool = fdice_create_page_pool(dev, ch->priv->netdev,
						&ch->napi,
						params->rx_ring_size,
						numa_node);
	if (IS_ERR(rxq->page_pool)) {
		err = PTR_ERR(rxq->page_pool);
		rxq->page_pool = NULL;
		goto err_free_cq;
	}

	/* Header-split page pool (if enabled) */
	if (params->hdr_split_enabled) {
		rxq->hdr_page_pool = fdice_create_page_pool(
			dev, ch->priv->netdev, &ch->napi,
			params->rx_ring_size, numa_node);
		if (IS_ERR(rxq->hdr_page_pool)) {
			err = PTR_ERR(rxq->hdr_page_pool);
			rxq->hdr_page_pool = NULL;
			goto err_free_pp;
		}
	}

	/* Allocate RX buffer descriptor array */
	rxq->rx_buf = kvzalloc_node(params->rx_ring_size *
				    sizeof(struct fdice_rx_buf),
				    GFP_KERNEL, numa_node);
	if (!rxq->rx_buf) {
		err = -ENOMEM;
		goto err_free_hdr_pp;
	}

	rxq->next_to_use = 0;
	rxq->next_to_clean = 0;
	rxq->state = FDICE_QS_ALLOCATED;

	return 0;

err_free_hdr_pp:
	if (rxq->hdr_page_pool) {
		page_pool_destroy(rxq->hdr_page_pool);
		rxq->hdr_page_pool = NULL;
	}
err_free_pp:
	page_pool_destroy(rxq->page_pool);
	rxq->page_pool = NULL;
err_free_cq:
	fdice_cq_free(&rxq->cq);
err_free_wq:
	fdice_wq_cyc_destroy(&rxq->wq);
	return err;
}

/**
 * fdice_rxq_destroy - Free all RX queue resources
 * @rxq: receive queue to destroy
 *
 * Returns all outstanding RX buffers to the page pool, destroys the
 * page pool, frees the CQ, and releases the descriptor ring.
 */
void fdice_rxq_destroy(struct fdice_rxq *rxq)
{
	u32 i;

	if (rxq->state == FDICE_QS_UNINIT || rxq->state == FDICE_QS_DESTROYED)
		return;

	/* Return all RX buffers to the page pool */
	if (rxq->rx_buf) {
		for (i = 0; i < rxq->count; i++) {
			struct fdice_rx_buf *buf = &rxq->rx_buf[i];

			if (buf->flags & FDICE_RXB_MAPPED)
				page_pool_put_full_page(rxq->page_pool,
							netmem_to_page(buf->netmem),
							false);
			buf->flags = 0;
		}
		kvfree(rxq->rx_buf);
		rxq->rx_buf = NULL;
	}

	if (rxq->hdr_page_pool) {
		page_pool_destroy(rxq->hdr_page_pool);
		rxq->hdr_page_pool = NULL;
	}

	if (rxq->page_pool) {
		page_pool_destroy(rxq->page_pool);
		rxq->page_pool = NULL;
	}

	fdice_cq_free(&rxq->cq);
	fdice_wq_cyc_destroy(&rxq->wq);

	rxq->state = FDICE_QS_DESTROYED;
}

/**
 * fdice_rxq_enable - Configure and activate an RX queue in hardware
 * @rxq: receive queue to enable
 *
 * Posts initial RX buffers, maps the tail register, enables the queue
 * in hardware, and writes the tail pointer to begin receiving.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_rxq_enable(struct fdice_rxq *rxq)
{
	struct fdice_channel *ch = rxq->channel;
	struct fdice_dev *fdev = ch->priv->fdev;
	struct fdice_hw *hw = &fdev->hw;
	u16 qidx = ch->idx;
	u32 reg_val;
	int posted, i;

	if (rxq->state != FDICE_QS_ALLOCATED &&
	    rxq->state != FDICE_QS_CONFIGURED)
		return -EINVAL;

	/* Map tail register */
	rxq->tail_reg = hw->hw_addr + FDICE_REG_QRX_TAIL(qidx);

	/* Post initial buffers */
	posted = fdice_rxq_alloc_bufs(rxq, rxq->count - 1);
	if (posted <= 0) {
		dev_err(&hw->pdev->dev,
			"RXQ %u: failed to allocate initial buffers\n", qidx);
		return -ENOMEM;
	}
	rxq->next_to_use = posted;

	/* Enable the RX queue in hardware */
	writel(FDICE_REG_QUEUE_ENA_REQ,
	       hw->hw_addr + FDICE_REG_QRX_ENA(qidx));

	/* Poll for hardware confirmation */
	for (i = 0; i < FDICE_QUEUE_WAIT_RETRIES; i++) {
		reg_val = readl(hw->hw_addr + FDICE_REG_QRX_ENA(qidx));
		if (reg_val & FDICE_REG_QUEUE_ENA_STAT)
			break;
		udelay(FDICE_QUEUE_WAIT_US);
	}

	if (!(reg_val & FDICE_REG_QUEUE_ENA_STAT)) {
		dev_err(&hw->pdev->dev, "RX queue %u enable timed out\n",
			qidx);
		return -ETIMEDOUT;
	}

	/* Advance tail to let hardware know about posted buffers */
	writel(rxq->next_to_use & rxq->wq.mask, rxq->tail_reg);

	rxq->state = FDICE_QS_ACTIVE;
	return 0;
}

/**
 * fdice_rxq_disable - Disable an RX queue in hardware
 * @rxq: receive queue to disable
 *
 * Disables the RX queue and waits for hardware drain.
 */
void fdice_rxq_disable(struct fdice_rxq *rxq)
{
	struct fdice_channel *ch = rxq->channel;
	struct fdice_dev *fdev = ch->priv->fdev;
	struct fdice_hw *hw = &fdev->hw;
	u16 qidx = ch->idx;
	u32 reg_val;
	int i;

	if (rxq->state != FDICE_QS_ACTIVE)
		return;

	/* Request queue disable */
	writel(0, hw->hw_addr + FDICE_REG_QRX_ENA(qidx));

	/* Wait for hardware drain */
	for (i = 0; i < FDICE_QUEUE_WAIT_RETRIES; i++) {
		reg_val = readl(hw->hw_addr + FDICE_REG_QRX_ENA(qidx));
		if (!(reg_val & FDICE_REG_QUEUE_ENA_STAT))
			break;
		udelay(FDICE_QUEUE_WAIT_US);
	}

	if (reg_val & FDICE_REG_QUEUE_ENA_STAT)
		dev_warn(&hw->pdev->dev,
			 "RX queue %u disable timed out\n", qidx);

	rxq->state = FDICE_QS_DISABLED;
}
