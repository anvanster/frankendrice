// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE Page Pool Management
 *
 * Creates and manages per-RXQ page pools with NUMA-aware allocation,
 * hardware DMA mapping, and page fragmentation for efficient memory
 * reuse. Each receive queue owns a dedicated page pool that provides
 * pre-mapped buffers for the E810 RX descriptor ring.
 *
 * Key design decisions:
 *   - NUMA pinning to channel's node (unlike ice which uses NUMA_NO_NODE)
 *   - Configurable fragmentation bias for small-packet workloads
 *   - Netdev/NAPI association for page pool tracking and pp_recycle stats
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md, Section 4.1
 */

#include <linux/netdevice.h>
#include <linux/dma-mapping.h>
#include <linux/slab.h>
#include <net/page_pool/helpers.h>

#include "fdice_txrx.h"
#include "fdice_queue.h"
#include "fdice_channel.h"
#include "fdice_dev.h"

/**
 * FDICE_PAGECNT_BIAS - Page fragmentation bias for RX buffers
 *
 * Controls how many RX buffers share a single page via the page pool
 * fragmentation API. Higher values improve memory utilization for
 * small-packet workloads at the cost of delayed page recycling.
 *
 * PAGE_SIZE / 64 is a conservative default. For jumbo frames or
 * order-0 pages with large buf_size, this naturally resolves to 1.
 *
 * Adopted from mlx5's page fragmentation pattern.
 */
#define FDICE_PAGECNT_BIAS	(PAGE_SIZE / 64)

/**
 * fdice_page_pool_create - Create a page pool for an RX queue
 * @rxq: receive queue that will own the page pool
 * @numa_node: NUMA node to pin allocations to
 * @dev: PCI device for DMA mapping
 * @netdev: network device for page pool stats association
 * @napi: NAPI instance for page pool recycling context
 *
 * Allocates and configures a kernel page_pool with DMA mapping and
 * CPU sync flags enabled. The pool is pinned to @numa_node to ensure
 * RX buffer memory is local to the CPU processing packets.
 *
 * Pool size is set to the RX ring count to pre-warm with enough
 * pages for a full ring fill, avoiding slow-start allocation pressure.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_page_pool_create(struct fdice_rxq *rxq, int numa_node,
			   struct device *dev, struct net_device *netdev,
			   struct napi_struct *napi)
{
	struct page_pool_params pp_params = {};
	struct page_pool *pool;

	pp_params.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV;
	pp_params.order = 0;
	pp_params.pool_size = rxq->count;
	pp_params.nid = numa_node;
	pp_params.dev = dev;
	pp_params.dma_dir = DMA_FROM_DEVICE;
	pp_params.netdev = netdev;
	pp_params.napi = napi;
	pp_params.max_len = rxq->buf_size;
	pp_params.offset = 0;

	pool = page_pool_create(&pp_params);
	if (IS_ERR(pool))
		return PTR_ERR(pool);

	rxq->page_pool = pool;

	return 0;
}

/**
 * fdice_page_pool_destroy - Destroy and free an RX queue's page pool
 * @rxq: receive queue owning the page pool
 *
 * Releases the page pool and all cached pages. Must be called only
 * after the RX ring has been fully drained and all RX buffers returned.
 *
 * Safe to call with rxq->page_pool == NULL.
 */
void fdice_page_pool_destroy(struct fdice_rxq *rxq)
{
	if (!rxq->page_pool)
		return;

	page_pool_destroy(rxq->page_pool);
	rxq->page_pool = NULL;
}

/**
 * fdice_page_pool_create_hdr - Create a header-split page pool
 * @rxq: receive queue that will own the header page pool
 * @numa_node: NUMA node for allocation pinning
 * @dev: PCI device for DMA mapping
 * @netdev: network device for stats
 * @napi: NAPI instance for recycling context
 * @hdr_buf_size: header buffer size in bytes
 *
 * Creates a second page pool for header buffers when E810 hardware
 * header-data split is enabled. Uses a smaller buffer size than the
 * main data page pool.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_page_pool_create_hdr(struct fdice_rxq *rxq, int numa_node,
			       struct device *dev, struct net_device *netdev,
			       struct napi_struct *napi, u16 hdr_buf_size)
{
	struct page_pool_params pp_params = {};
	struct page_pool *pool;

	pp_params.flags = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV;
	pp_params.order = 0;
	pp_params.pool_size = rxq->count;
	pp_params.nid = numa_node;
	pp_params.dev = dev;
	pp_params.dma_dir = DMA_FROM_DEVICE;
	pp_params.netdev = netdev;
	pp_params.napi = napi;
	pp_params.max_len = hdr_buf_size;
	pp_params.offset = 0;

	pool = page_pool_create(&pp_params);
	if (IS_ERR(pool))
		return PTR_ERR(pool);

	rxq->hdr_page_pool = pool;

	return 0;
}

/**
 * fdice_page_pool_destroy_hdr - Destroy the header-split page pool
 * @rxq: receive queue owning the header page pool
 *
 * Safe to call with rxq->hdr_page_pool == NULL.
 */
void fdice_page_pool_destroy_hdr(struct fdice_rxq *rxq)
{
	if (!rxq->hdr_page_pool)
		return;

	page_pool_destroy(rxq->hdr_page_pool);
	rxq->hdr_page_pool = NULL;
}

/**
 * fdice_rx_alloc_bufs - Bulk allocate RX buffers from the page pool
 * @rxq: receive queue to allocate buffers for
 * @count: number of buffers to allocate
 *
 * Performs bulk page allocation from the RX queue's page pool.
 * Each buffer is configured with DMA address, offset, and truesize.
 * Applies fragmentation bias for small-packet optimization when
 * FDICE_PAGECNT_BIAS > 1.
 *
 * Called during ring initialization and by fdice_rxq_post_wqes
 * during runtime refill.
 *
 * Return: number of buffers successfully allocated (may be less
 *         than @count if the pool is depleted).
 */
int fdice_rx_alloc_bufs(struct fdice_rxq *rxq, u16 count)
{
	struct page_pool *pool = rxq->page_pool;
	u16 ntu = rxq->next_to_use;
	u16 ring_mask = rxq->count - 1;
	u16 allocated = 0;
	u16 i;

	for (i = 0; i < count; i++) {
		u16 idx = ntu & ring_mask;
		struct fdice_rx_buf *buf = &rxq->rx_buf[idx];
		netmem_ref netmem;
		dma_addr_t dma;

		netmem = page_pool_dev_alloc_netmems(pool);
		if (unlikely(!netmem))
			break;

		/*
		 * Apply fragmentation bias to share pages across multiple
		 * RX buffers. This mlx5-inspired optimization improves
		 * memory utilization for small-packet workloads.
		 */
		if (FDICE_PAGECNT_BIAS > 1)
			page_pool_fragment_netmem(netmem, FDICE_PAGECNT_BIAS);

		dma = page_pool_get_dma_addr_netmem(netmem);

		buf->netmem = netmem;
		buf->dma = dma;
		buf->offset = 0;
		buf->truesize = rxq->buf_size;
		buf->flags = FDICE_RXB_MAPPED;

		ntu++;
		allocated++;
	}

	return allocated;
}
