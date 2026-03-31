<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 09: DMA and Memory Management

Comparison of buffer management, page pool usage, and DMA mapping strategies
between mlx5 and ice drivers, informing the fdice_ memory subsystem design.

## mlx5 DMA and Buffer Management

### RX Buffer Architecture

mlx5 supports two distinct RX queue modes, each with its own buffer strategy:

**Regular RQ (Cyclic WQ mode - `mlx5_wq_cyc`)**:
- Each WQE contains up to `MLX5E_MAX_RX_FRAGS` (4) data segments
- Buffers tracked via `mlx5e_wqe_frag_info` which holds a `mlx5e_frag_page` pointer and offset
- Page allocation goes through `mlx5e_page_alloc_fragmented()` which calls `page_pool_dev_alloc_netmems()`
- Pages are pre-fragmented with `page_pool_fragment_netmem()` using `MLX5E_PAGECNT_BIAS_MAX` (PAGE_SIZE / 64) bias
- DMA addresses retrieved via `page_pool_get_dma_addr_netmem()` -- no explicit driver-side dma_map calls for RX
- Allocation unit tracking via `union mlx5e_alloc_units` (flexible array of `mlx5e_frag_page`, `page`, or `xdp_buff`)
- Refill uses `mlx5e_refill_rx_wqes()` with chunked bulk allocation sized to page pool cache refill unit

**Striding RQ (MPWQE mode - `mlx5_wq_ll`)**:
- Each WQE maps a large contiguous region via multiple MTT (Memory Translation Table) entries
- `mlx5e_mpw_info` tracks per-WQE state: consumed strides, skip_release bitmap, allocation units
- Pages allocated in bulk per WQE (`pages_per_wqe` pages, each via `mlx5e_page_alloc_fragmented`)
- DMA addresses written as MTT entries in a UMR (User Memory Registration) WQE posted to ICOSQ
- Hardware MTT-based remapping allows scatter pages to appear contiguous to the NIC
- `mlx5e_shampo_hd` provides header-data split with dedicated `hd_page_pool` and `mlx5e_dma_info` array

**RX Page Pool Configuration** (from `en_main.c`):
- `PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV` -- page pool handles DMA mapping and sync
- Order-0 pages only (`pp_params.order = 0`)
- Pool size matches queue depth
- NUMA-aware allocation (`pp_params.nid = node`)
- NAPI and netdev attached for recycling
- DMA direction from `rq->buff.map_dir`
- Optional `PP_FLAG_ALLOW_UNREADABLE_NETMEM` for SHAMPO header split
- Separate `hd_page_pool` for header-data split (defaults to main pool if no split)

### TX DMA Management

mlx5 TX uses an explicit DMA FIFO model:
- `mlx5e_sq_dma` structure tracks each DMA mapping: address, size, and type enum (`MLX5E_DMA_MAP_SINGLE` or `MLX5E_DMA_MAP_PAGE`)
- DMA FIFO (`sq->db.dma_fifo`) is a circular buffer indexed by `dma_fifo_pc` (producer) / `dma_fifo_cc` (consumer)
- `mlx5e_dma_push_single()` / `mlx5e_dma_push_netmem()` record mappings at TX time
- `mlx5e_tx_dma_unmap()` dispatches to `dma_unmap_single()` or `netmem_dma_unmap_page_attrs()` based on type
- Completion path walks the DMA FIFO via `mlx5e_tx_wi_dma_unmap()`, processing `wi->num_dma` entries per WQE
- Error path rolls back via `mlx5e_dma_unmap_wqe_err()`

### Control Queue and WQ DMA

- `mlx5_frag_buf_alloc_node()` in `alloc.c`: NUMA-aware coherent DMA allocation for WQ/CQ buffers
- Allocates page-sized fragments via `dma_alloc_coherent()` with explicit NUMA node pinning
- Temporarily sets device NUMA node via `set_dev_node()` under mutex for NUMA-local allocation
- Fragment alignment verified against page_shift; misaligned allocations are freed and reported
- Doorbell pages allocated via `mlx5_db_pgdir` with bitmap-tracked sub-page allocation

## ice DMA and Buffer Management

### RX Buffer Architecture via libeth

ice delegates RX buffer management to the `libeth` common library layer:

**libeth Fill Queue (FQ) abstraction**:
- `struct libeth_fq`: wraps a `page_pool`, `libeth_fqe` array, truesize, count
- `struct libeth_fqe`: per-buffer descriptor with `netmem_ref`, offset, truesize
- Three buffer types via `enum libeth_fqe_type`: `LIBETH_FQE_MTU` (full frame), `LIBETH_FQE_SHORT` (small), `LIBETH_FQE_HDR` (header split)
- `libeth_rx_fq_create()` creates the page pool and allocates the FQE array
- `libeth_rx_alloc()` is an inline that calls `page_pool_dev_alloc_netmem()` with offset support, returns DMA address directly

**ice RX ring integration** (`ice_rxq_pp_create` in `ice_base.c`):
- Creates primary FQ with `LIBIE_MAX_RX_BUF_LEN` buffer size
- Optionally creates secondary header FQ (`LIBETH_FQE_HDR` type) when header split enabled
- Page pool reference stored in `ring->pp`, FQEs in `ring->rx_fqes`
- XDP rxq info attached to page pool for proper memory model registration

**Buffer allocation in hot path** (`ice_alloc_rx_bufs` in `ice_txrx.c`):
- Iterates from `next_to_use`, calling `libeth_rx_alloc()` for each descriptor
- Writes DMA address directly to `rx_desc->read.pkt_addr`
- For header split, also allocates from `hdr_fq` and writes to `rx_desc->read.hdr_addr`
- On allocation failure, recycles the already-allocated body buffer via `libeth_rx_recycle_slow()`

**ice RX ring struct** (`ice_txrx.h`):
- `struct page_pool *pp` in read_mostly cacheline group
- `struct page_pool *hdr_pp` for header split
- `dma_addr_t dma` in cold group -- this is the descriptor ring DMA address, not per-buffer
- Cache-line-grouped layout: read_mostly (pp, desc, netdev, q_vector, tail), read_write (next_to_use/clean, xdp state), cold (vsi, dma, config)

### TX DMA Management

ice TX uses the standard kernel DMA unmap macros:
- `struct ice_tx_buf` contains `DEFINE_DMA_UNMAP_LEN(len)` and `DEFINE_DMA_UNMAP_ADDR(dma)`
- `ice_tx_map()` calls `dma_map_single()` for skb head data, `skb_frag_dma_map()` for fragments
- DMA address and length recorded via `dma_unmap_addr_set()` / `dma_unmap_len_set()`
- Handles data chunks larger than `ICE_MAX_DATA_PER_TXD` by splitting across multiple descriptors
- PCIe read request alignment optimization: `max_data += -dma & (ICE_MAX_READ_REQ_SIZE - 1)`
- Typed TX buffers via `ice_tx_buf_type` enum: SKB, FRAG, XDP, XSK, DUMMY

### Control Queue DMA

- `ice_alloc_ctrlq_sq_ring()` / `ice_alloc_ctrlq_rq_ring()`: `dmam_alloc_coherent()` for descriptor rings (device-managed, auto-freed)
- `ice_alloc_rq_bufs()`: per-entry `dmam_alloc_coherent()` buffers for Admin RQ receive
- Uses `struct ice_dma_mem` (va, pa, size) for coherent buffer tracking
- Device-managed allocation (`dmam_*`) ensures cleanup on driver unload even if explicit free is missed

## Page Pool Usage Comparison

| Aspect | mlx5 | ice |
|--------|------|-----|
| Page pool creation | Direct `page_pool_create()` in `en_main.c` | Via `libeth_rx_fq_create()` abstraction |
| Pool per queue | One primary + optional SHAMPO header pool per RQ | One primary + optional header pool per ring |
| DMA mapping | `PP_FLAG_DMA_MAP \| PP_FLAG_DMA_SYNC_DEV` | Delegated to libeth/page_pool |
| Page order | Order-0 only | Order-0 only (`LIBETH_RX_PAGE_ORDER = 0`) |
| Page fragmentation | Manual `page_pool_fragment_netmem()` with bias of PAGE_SIZE/64 | Handled by `page_pool_dev_alloc_netmem()` with offset |
| NUMA awareness | Explicit `pp_params.nid = node` | `NUMA_NO_NODE` in libeth_fq config |
| DMA sync | `dma_sync_single_for_cpu()` explicit in RX path | Handled by page pool infrastructure |
| Allocation API | `page_pool_dev_alloc_netmems()` (returns netmem) | `page_pool_dev_alloc_netmem()` (returns netmem + offset) |
| Recycling | `page_pool_unref_netmem()` + `page_pool_put_unrefed_netmem()` | `libeth_rx_recycle_slow()` |
| XDP integration | `MEM_TYPE_PAGE_POOL` via `xdp_rxq_info_reg_mem_model` | `xdp_rxq_info_attach_page_pool()` |
| XSK integration | Separate `MEM_TYPE_XSK_BUFF_POOL` path | Same separate XSK path |

## Key Differences in Mapping Strategies

### RX Path

1. **Abstraction level**: mlx5 directly manages page pool interactions with custom fragmentation bias. ice uses the libeth abstraction layer that encapsulates page pool details, providing a simpler API at the cost of less control.

2. **MPWQE/UMR mapping**: mlx5's striding RQ mode uses hardware Memory Translation Tables (MTT) posted via UMR WQEs on the ICOSQ. This allows scatter pages to appear as a single contiguous buffer to the NIC, enabling large receive buffers without contiguous physical memory. ice has no equivalent -- each descriptor points to a single DMA address.

3. **Fragmentation strategy**: mlx5 pre-fragments pages with a high bias count (PAGE_SIZE/64), allowing many small buffers per page with efficient reference counting. ice/libeth uses page_pool's built-in offset-based sharing, which is simpler but less tunable.

4. **Header-data split**: Both support it. mlx5 uses a dedicated `hd_page_pool` with `mlx5e_shampo_hd` structure containing an array of `mlx5e_dma_info`. ice uses a second `libeth_fq` with `LIBETH_FQE_HDR` type.

### TX Path

1. **DMA tracking**: mlx5 uses a dedicated DMA FIFO (`mlx5e_sq_dma` ring) separate from the WQ, with typed entries (SINGLE vs PAGE). ice uses the standard kernel `DEFINE_DMA_UNMAP_ADDR/LEN` macros embedded directly in `ice_tx_buf`.

2. **Unmap dispatch**: mlx5 has an explicit switch on DMA type in `mlx5e_tx_dma_unmap()`. ice relies on the kernel DMA unmap helpers invoked per-buffer during completion.

3. **PCIe optimization**: ice explicitly aligns TX data chunks to PCIe Max Read Request Size boundaries. mlx5 relies on hardware scatter-gather and MPWQE for TX efficiency.

### Control Path

1. **Allocation model**: mlx5 uses explicit `dma_alloc_coherent()` with NUMA node pinning and manual lifecycle. ice uses `dmam_alloc_coherent()` (device-managed) for automatic cleanup on driver removal.

2. **Buffer structure**: mlx5 uses `mlx5_frag_buf` with page-sized fragments for WQ/CQ buffers, checked for alignment. ice uses `ice_dma_mem` (va/pa/size triplet) for control queue buffers.

## Recommendations for fdice_ Memory Management

### RX Buffer Strategy

1. **ADOPT mlx5-style page pool with direct control**: Create `fdice_page_pool` wrapper that directly uses `page_pool_create()` with explicit flags (`PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV`). Avoid the libeth abstraction -- it adds indirection without benefit for a new driver that can design its own buffer management from scratch.

2. **ADOPT mlx5-style page fragmentation bias**: Use `page_pool_fragment_netmem()` with configurable bias for efficient page sharing. This gives better memory utilization than the default page_pool offset model.

3. **ADOPT NUMA-aware pool creation**: Always pass the channel's NUMA node to `pp_params.nid` as mlx5 does, rather than `NUMA_NO_NODE` like ice.

4. **DESIGN fdice_rx_buf abstraction**: Create a unified buffer descriptor:
   - `struct fdice_rx_buf { netmem_ref netmem; dma_addr_t dma; u32 offset; u16 truesize; u8 flags; }`
   - Replaces both mlx5's `mlx5e_wqe_frag_info`/`mlx5e_frag_page` and ice's `libeth_fqe`
   - Simpler than mlx5 (no separate frag_page indirection), richer than ice (explicit DMA addr cache)

5. **SUPPORT header-data split from day one**: Use a separate `hd_page_pool` for header buffers, similar to both drivers. E810 hardware supports DTYPE-based header split natively.

### TX DMA Strategy

6. **ADOPT mlx5-style typed DMA FIFO**: The separate DMA tracking FIFO (`fdice_sq_dma`) with typed entries provides cleaner separation of concerns than ice's inline `DEFINE_DMA_UNMAP_*` macros. It also makes the completion path simpler and more cache-friendly.

7. **IMPROVE with combined unmap tracking**: Merge the type enum into a flags field within the DMA entry to support future DMA mapping modes (e.g., persistent mappings for RDMA, XDP redirect DMA).

### Control Queue DMA

8. **ADOPT ice-style device-managed allocation for control queues**: Use `dmam_alloc_coherent()` for Admin Queue descriptor rings and buffers. This provides safety-net cleanup and is appropriate for control-path allocations that persist for the driver lifetime.

9. **ADOPT mlx5-style fragmented allocation for data-path WQ/CQ**: Use page-sized coherent DMA fragments with NUMA pinning for work queue and completion queue buffers, similar to `mlx5_frag_buf_alloc_node()`. This avoids large contiguous allocation requirements.

### General Principles

10. **Page pool for all RX data-path DMA**: Let page_pool handle DMA mapping for all receive buffers. Never call `dma_map_page()` directly in the RX hot path.

11. **Explicit DMA sync in RX completion**: Follow mlx5's pattern of explicit `dma_sync_single_for_cpu()` calls with the correct offset and length at CQE processing time.

12. **Cache-line-aware struct layout**: Follow ice's `__cacheline_group_begin/end_aligned` pattern for all hot-path structures, grouping DMA addresses and buffer pointers in the read_mostly section.

13. **XDP/XSK support in buffer design**: Design the buffer abstraction to cleanly switch between page_pool (normal), XSK buff pool (AF_XDP), and XDP redirect paths from the start. Both reference drivers show this as a union-based design.

### Source Files Referenced

- mlx5 RX buffers: `core/en_rx.c` (alloc/free/refill), `core/en.h` (structs)
- mlx5 TX DMA: `core/en/txrx.h` (DMA FIFO ops, unmap), `core/en_tx.c` (mapping)
- mlx5 page pool: `core/en_main.c:958-995` (creation), `core/en_rx.c:277-337` (fragmented alloc)
- mlx5 WQ DMA: `core/alloc.c` (frag_buf coherent alloc)
- ice RX buffers: `ice_base.c:607-652` (pp_create), `ice_txrx.c:835-904` (alloc_rx_bufs)
- ice TX DMA: `ice_txrx.c:1397-1476` (ice_tx_map), `ice_txrx.h:97-118` (ice_tx_buf)
- ice control DMA: `ice_controlq.c:90-189` (coherent alloc)
- libeth layer: `include/net/libeth/rx.h` (FQ/FQE structs, libeth_rx_alloc)
