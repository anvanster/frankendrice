<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Architecture 02: Queue Architecture

## Overview

FrankenDrICE adopts a three-level queue hierarchy inspired by mlx5's
EQ -> CQ -> WQ model, layered above E810-native descriptor rings. This
provides the abstraction depth needed for RDMA/ethernet coexistence,
flexible interrupt routing, and clean completion processing -- while the
underlying hardware interface remains E810 Admin Queue commands and
descriptor ring DMA.

**Design principle:** Everything below the AQ interface uses E810-native
descriptor formats. Everything above uses mlx5-inspired abstractions that
decouple software queue management from hardware specifics.

**Key exploration references:**
- Exploration 02: Queue Management (mlx5 vs ice comparison)
- Exploration 09: DMA and Memory Management
- Exploration 14: ADQ (Application Device Queues)
- Exploration 10: DCB/QoS (TC integration)

---

## 1. Queue Hierarchy

FrankenDrICE implements three distinct queue abstraction levels. Each level
has a single responsibility and clean interfaces to adjacent levels.

```
                    +-----------+
                    | MSI-X Vec |   (hardware interrupt)
                    +-----+-----+
                          |
                    +-----v-----+
                    | fdice_eq  |   Level 1: Event Queue (interrupt routing)
                    +-----+-----+
                          |
              +-----------+-----------+
              |           |           |
        +-----v---+ +----v----+ +----v----+
        | fdice_cq| |fdice_cq | |fdice_cq |  Level 2: Completion Queues
        +-----+---+ +----+----+ +----+----+
              |           |           |
        +-----v---+ +----v----+ +----v----+
        | fdice_wq| |fdice_wq | |fdice_wq |  Level 3: Work Queues (descriptor rings)
        +---------+ +---------+ +---------+
```

### 1.1 Event Queues (fdice_eq) -- Interrupt Routing Layer

Each `fdice_eq` maps to exactly one MSI-X vector and provides interrupt
routing for one or more completion queues. Unlike mlx5 where EQs are
hardware objects, fdice_eq is a software abstraction over E810's interrupt
vector mechanism that gains us:

- Dynamic CQ-to-interrupt remapping without hardware reconfiguration
- Shared interrupt infrastructure between ethernet and RDMA consumers
- A clean place to implement interrupt coalescing policy

**CQ-to-EQ mapping:** Each EQ owns an xarray (`eq->cq_table`) that maps
CQ numbers to `fdice_cq` pointers. When an interrupt fires:

```
MSI-X IRQ -> fdice_eq_irq_handler(eq)
  -> xa_load(&eq->cq_table, cqn)     // O(1) lookup
  -> cq->comp_handler(cq)            // callback into consumer
  -> napi_schedule()                  // for ethernet CQs
```

The xarray replaces mlx5's radix tree with a more modern kernel data
structure that provides better RCU semantics and simpler error handling.

**EQ types:**

- **Completion EQs:** One per channel/NAPI context. Services data-path CQs
  for TX completions, RX completions, and XDP completions within that channel.
- **Async EQ:** Single instance for error events, port change notifications,
  Admin Queue completions, and RDMA async events. Uses a workqueue rather
  than NAPI for deferred processing.

**EQ table:** `fdice_eq_table` manages all EQs for the device, tracks the
MSI-X vector pool, and provides allocation/release APIs for both ethernet
and RDMA consumers.

### 1.2 Completion Queues (fdice_cq) -- Event Notification Layer

Each `fdice_cq` provides typed completion notifications for one or more
work queues. The CQ is a software abstraction that wraps E810's descriptor
completion mechanism with arm/disarm semantics and callback dispatch.

**Arm/disarm mechanism:** CQs support an arm bit that controls whether
completions generate an interrupt through the parent EQ. This enables:

- Busy-polling: disarm CQ, poll directly, re-arm when idle
- NAPI integration: disarm during poll, re-arm at end of NAPI cycle
- Interrupt coalescing: arm with delay parameters

**Completion callback:** Each CQ has a `comp_handler` function pointer set
at creation time. For ethernet CQs this triggers NAPI scheduling. For RDMA
CQs this invokes the ib_cq completion callback. This shared interface is
what enables ethernet/RDMA coexistence on the same EQ.

**CQ work queue (cqwq):** The CQ contains an embedded ring buffer
(`fdice_cqwq`) for reading completion entries. This ring uses a consumer
counter and owner-bit protocol for hardware synchronization, following the
mlx5 pattern but operating over E810 descriptor completion data.

**Sharing model:** A single CQ can service completions from multiple WQs.
In the common ethernet case each TXQ and RXQ embeds its own CQ for cache
locality. For RDMA queue pairs, the CQ is a shared resource referenced by
multiple QPs.

### 1.3 Work Queues (fdice_wq) -- Descriptor Ring Layer

Work queues are the typed descriptor rings where packets are posted (TX)
and received (RX). FrankenDrICE provides two WQ variants through a common
interface:

**`fdice_wq_cyc` -- Cyclic (circular) work queue:**
- Standard ring buffer with power-of-2 size
- Producer counter (`pc`) and consumer counter (`cc`)
- Used for TX send queues, RX queues, and control queues
- Fragment buffer control (`fdice_frag_buf_ctrl`) for DMA management
- Doorbell pointer for hardware notification

**`fdice_wq_ll` -- Linked-list work queue:**
- WQEs linked via next pointers for O(1) push/pop
- Head and tail tracking with free list
- Used for shared receive queues (SRQ) in RDMA path
- Supports variable-size WQE posting

**`fdice_wq_ctrl` -- Common control structure:**
- Owns the underlying DMA buffer (`fdice_frag_buf`)
- Manages doorbell allocation
- Provides NUMA-aware coherent DMA allocation using page-sized fragments
  (adopted from mlx5's `mlx5_frag_buf_alloc_node` pattern)

**`fdice_wq_param` -- Creation parameters:**
- Ring size (log2)
- WQE stride (log2)
- WQ type (cyclic or linked-list)
- NUMA node affinity
- DMA direction

Both WQ types share a common base that provides index-to-pointer
translation, fill-level queries, and DMA address resolution. The type
abstraction enables reuse across data queues, control queues, and XDP
queues without duplicating ring management logic.

---

## 2. Channel Model

### 2.1 struct fdice_channel

The channel is the per-NAPI grouping of all queues serviced by a single
interrupt vector. This follows mlx5's `mlx5e_channel` pattern with embedded
(not pointed-to) queue structs for cache locality.

```c
struct fdice_channel {
    /* --- cacheline 0: hot path, read-mostly --- */
    struct fdice_rxq        rxq;
    struct napi_struct       napi;
    int                      cpu;           /* bound CPU */
    int                      numa_node;

    /* --- TX queues, one per traffic class --- */
    struct fdice_txq         txq[FDICE_MAX_TC];
    struct fdice_xdp_sq      xdp_sq;        /* XDP transmit */

    /* --- control --- */
    struct fdice_icosq       icosq;         /* internal control ops */
    struct fdice_eq         *eq;            /* parent event queue */
    struct fdice_params     *params;        /* channel parameters */

    /* --- identity --- */
    u16                      idx;           /* channel index */
    struct fdice_priv       *priv;          /* back-pointer */
};
```

**Design rationale vs ice:** ice's `ice_q_vector` uses linked lists of
rings (`ice_for_each_rx_ring`), requiring pointer chasing in the hot path.
By embedding queues directly in the channel struct, fdice_ ensures that
the RXQ, its CQ, and the NAPI struct are in adjacent cache lines. The
TXQ array is indexed by traffic class, providing O(1) TC-to-queue mapping
without linked-list traversal.

### 2.2 Parameter Hierarchy

Channel configuration uses a layered parameter model that separates
hardware capabilities from runtime configuration:

```
fdice_hw_params          (device capabilities, read from firmware)
  -> fdice_profile       (performance profile: default, low-latency, throughput)
    -> fdice_params      (runtime parameters: ring sizes, coalescing, features)
      -> fdice_channel   (per-channel instantiation of params)
```

**`fdice_params`** holds all tunable queue parameters:
- Ring sizes (RX count, TX count per TC)
- Interrupt coalescing settings
- RX buffer configuration (MTU, header split, page pool params)
- Feature flags (XDP, XSK, timestamping)
- TC count and per-TC queue counts

Parameter changes follow the mlx5 `safe_switch_params` pattern: build a
new params struct, validate it, swap atomically with the old one, then
rebuild affected channels. This avoids partial-update states that plague
ice's incremental reconfiguration paths.

### 2.3 Cache-Line-Aware Layout

All channel sub-structures use explicit cache-line grouping adopted from
ice's `__cacheline_group_begin/end_aligned` pattern:

- **read_mostly:** Descriptor ring pointers, page pool, netdev, NAPI,
  tail register -- fields accessed on every packet but rarely modified
- **read_write:** Producer/consumer indices, budget counters, XDP state --
  fields modified on every packet
- **cold:** Statistics, configuration, back-pointers -- fields accessed
  only during setup or ethtool queries

---

## 3. Queue Lifecycle

All queue types follow a unified state machine with well-defined transitions
and resource cleanup at each stage.

```
  UNINIT ──create──> ALLOCATED ──configure──> CONFIGURED
                                                  │
                                              enable
                                                  │
  DESTROYED <──destroy── DISABLED <──disable── ACTIVE
```

### 3.1 State Definitions

| State | Resources Held | Hardware State |
|-------|---------------|----------------|
| `FDICE_QS_UNINIT` | None | No hardware resources |
| `FDICE_QS_ALLOCATED` | DMA buffers, ring memory | Descriptor ring mapped |
| `FDICE_QS_CONFIGURED` | + CQ registered, doorbell mapped | Queue context written via AQ |
| `FDICE_QS_ACTIVE` | + interrupts enabled, buffers posted | Queue enabled in hardware |
| `FDICE_QS_DISABLED` | DMA buffers retained, interrupts off | Queue disabled, drain complete |
| `FDICE_QS_DESTROYED` | None (all freed) | Hardware context released |

### 3.2 Transition Operations

**Create (UNINIT -> ALLOCATED):**
1. Allocate DMA-coherent ring memory via `fdice_frag_buf_alloc_node()`
   with NUMA pinning to the channel's node
2. Allocate doorbell page
3. Initialize producer/consumer counters to zero
4. For RX: create page pool, pre-allocate buffer descriptors
5. For TX: allocate DMA FIFO and SKB tracking array

**Configure (ALLOCATED -> CONFIGURED):**
1. Write queue context to hardware via Admin Queue command
2. Register CQ in parent EQ's cq_table (xarray insert)
3. Map doorbell to hardware register
4. Set queue parameters (ring size, buffer size, descriptor format)

**Enable (CONFIGURED -> ACTIVE):**
1. Post initial RX buffers (for RX queues)
2. Arm CQ to generate interrupts
3. Enable NAPI
4. Write queue tail register to kick hardware

**Disable (ACTIVE -> DISABLED):**
1. Disable NAPI
2. Disarm CQ
3. Issue queue disable via AQ command
4. Wait for hardware drain (poll for completion or timeout)
5. Process any remaining completions

**Destroy (DISABLED -> DESTROYED):**
1. Remove CQ from EQ's cq_table
2. Free remaining RX buffers, return pages to page pool
3. Unmap and free TX DMA FIFO entries
4. Free doorbell page
5. Free DMA-coherent ring memory
6. Destroy page pool

### 3.3 Integration with Init/Teardown Layers

Channel bring-up and tear-down sequence:

```
fdice_open()
  -> fdice_alloc_channels()        // create all channels
    -> fdice_channel_create()      // per-channel: alloc + configure
      -> fdice_rxq_create()
      -> fdice_txq_create() x num_tc
      -> fdice_xdp_sq_create()
      -> fdice_icosq_create()
  -> fdice_enable_channels()       // enable all channels
    -> fdice_channel_enable()      // per-channel: enable + arm
      -> fdice_rxq_enable()
      -> fdice_txq_enable() x num_tc
      -> napi_enable()

fdice_close()
  -> fdice_disable_channels()      // disable all channels
    -> napi_disable()
    -> fdice_rxq_disable()
    -> fdice_txq_disable() x num_tc
  -> fdice_free_channels()         // destroy all channels
    -> fdice_channel_destroy()     // per-channel cleanup
```

Error handling uses reverse-order teardown: if `fdice_txq_create()` fails
for TC N, all previously created TXQs (TC 0..N-1) and the RXQ are
destroyed before returning the error. This prevents resource leaks on
partial initialization.

---

## 4. RX Data Path

### 4.1 Page Pool Integration

Each `fdice_rxq` owns a dedicated `page_pool` configured for NUMA-local
allocation and hardware DMA mapping:

```c
struct page_pool_params pp_params = {
    .flags      = PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV,
    .order      = 0,                    /* order-0 pages only */
    .pool_size  = ring_size,
    .nid        = channel->numa_node,   /* NUMA-aware (mlx5 pattern) */
    .dev        = &pdev->dev,
    .dma_dir    = DMA_FROM_DEVICE,
    .netdev     = netdev,
    .napi       = &channel->napi,
};
```

**NUMA awareness:** Unlike ice which passes `NUMA_NO_NODE` through libeth,
fdice_ always pins page pool allocation to the channel's NUMA node. This
ensures RX buffer memory is local to the CPU processing the packets.

**Page fragmentation:** Following mlx5's pattern, fdice_ uses
`page_pool_fragment_netmem()` with a configurable bias
(`FDICE_PAGECNT_BIAS`, default PAGE_SIZE/64) to share each page across
multiple RX buffers. This provides better memory utilization than the
default page_pool offset model for small-packet workloads.

**Header-data split:** When enabled, a second page pool (`hdr_page_pool`)
with smaller buffer size is created for header buffers. E810 hardware
supports DTYPE-based header split natively.

### 4.2 RX Buffer Management

```c
struct fdice_rx_buf {
    netmem_ref              netmem;     /* page pool netmem reference */
    dma_addr_t              dma;        /* cached DMA address */
    u32                     offset;     /* offset within page */
    u16                     truesize;   /* allocated size */
    u8                      flags;      /* FDICE_RXB_* flags */
};
```

This structure replaces both mlx5's `mlx5e_wqe_frag_info`/`mlx5e_frag_page`
chain (simpler, no separate indirection) and ice's `libeth_fqe` (richer,
with explicit DMA address caching to avoid re-deriving it in the hot path).

**Buffer flags:**
- `FDICE_RXB_MAPPED` -- DMA mapping is valid
- `FDICE_RXB_HDR_SPLIT` -- buffer is from header page pool
- `FDICE_RXB_XDP` -- buffer is in XDP processing
- `FDICE_RXB_XSK` -- buffer is from XSK buff pool

**Allocation path (bulk):**
```
fdice_rxq_alloc_bufs(rxq, count)
  -> page_pool_dev_alloc_netmems()    // bulk allocation
  -> page_pool_fragment_netmem()      // apply fragmentation bias
  -> page_pool_get_dma_addr_netmem()  // retrieve DMA address
  -> write descriptor with DMA addr   // post to hardware ring
  -> writel(tail, rxq->tail_reg)      // notify hardware
```

### 4.3 Flex Descriptor Parsing

E810 supports flexible RX descriptors (`ice_32b_rx_flex_desc`) that provide
richer metadata than legacy descriptors. fdice_ parses flex descriptors in
the CQ completion handler:

```
fdice_poll_rx_cq(cq, budget)
  for each completed descriptor:
    -> fdice_rx_flex_parse(desc, &parsed)   // extract metadata
       - ptype (packet type)
       - rx_hash (RSS hash value + type)
       - vlan (ID + priority, if stripped)
       - checksum status
       - timestamp (if PTP enabled)
       - flow_id (for flow steering)
    -> fdice_rxq_build_skb(rxq, buf, &parsed)
       - dma_sync_single_for_cpu() with exact offset+length
       - build_skb() or napi_build_skb() from buffer
       - apply parsed metadata to skb fields
    -> napi_gro_receive() or xdp_do_redirect()
```

**Flex descriptor profiles:** fdice_ configures the E810 flex descriptor
profile at queue creation time to select which metadata fields appear in
the descriptor. This avoids runtime field-presence checks in the hot path.

### 4.4 RX Refill Strategy

Buffer refill uses chunked allocation sized to the page pool cache refill
unit (adopted from mlx5's `mlx5e_refill_rx_wqes` pattern):

```
fdice_rxq_post_wqes(rxq)
  -> calculate refill count = wq_cyc_missing(&rxq->wq)
  -> if refill_count < FDICE_RX_REFILL_THRESHOLD: return
  -> fdice_rxq_alloc_bufs(rxq, refill_count)
  -> doorbell write to advance tail
```

The refill threshold prevents excessive small allocations while ensuring
the hardware never runs out of posted buffers under normal load.

---

## 5. TX Data Path

### 5.1 Typed DMA FIFO

Following mlx5's pattern, each `fdice_txq` maintains a separate DMA
tracking FIFO rather than embedding DMA metadata in TX buffer descriptors
(as ice does with `DEFINE_DMA_UNMAP_ADDR/LEN`).

```c
struct fdice_sq_dma {
    dma_addr_t              addr;
    u32                     size;
    u8                      type;       /* FDICE_DMA_MAP_SINGLE | _PAGE | _PERSIST */
};

struct fdice_txq {
    /* ... */
    struct fdice_sq_dma    *dma_fifo;   /* circular DMA tracking buffer */
    u32                     dma_fifo_pc; /* DMA FIFO producer counter */
    u32                     dma_fifo_cc; /* DMA FIFO consumer counter */
    u32                     dma_fifo_mask;
    /* ... */
};
```

**Advantages over ice's inline approach:**
- Cleaner separation: TX buffer tracking and DMA tracking are independent
- Cache-friendly completion: DMA unmap walks a contiguous array rather
  than chasing per-descriptor pointers
- Type extensibility: the type field supports future persistent DMA
  mappings for RDMA redirect and XDP forwarding without restructuring

**DMA operations:**
```
fdice_tx_dma_push(txq, addr, size, type)  // record at TX post time
fdice_tx_dma_unmap(txq, count)            // bulk unmap at completion
fdice_tx_dma_rollback(txq, count)         // error path cleanup
```

### 5.2 TX Buffer Tracking

```c
struct fdice_tx_buf {
    union {
        struct sk_buff     *skb;        /* network stack buffer */
        struct xdp_frame   *xdpf;       /* XDP frame */
    };
    u16                     num_dma;     /* DMA entries for this WQE */
    u16                     num_wqebbs;  /* WQE basic blocks consumed */
    u8                      type;        /* FDICE_TXB_SKB | _XDP | _XSK */
};
```

The TX buffer array is indexed by WQ consumer counter. Each entry records
the buffer pointer and the number of DMA FIFO entries associated with it,
enabling efficient bulk completion.

### 5.3 TX Completion Processing

```
fdice_poll_tx_cq(cq, budget)
  -> read CQ consumer counter from hardware
  -> for each completed WQE (cc to new_cc):
     -> tx_buf = &txq->tx_buf[cc & mask]
     -> fdice_tx_dma_unmap(txq, tx_buf->num_dma)
     -> if (tx_buf->type == FDICE_TXB_SKB)
          napi_consume_skb(tx_buf->skb, budget)
     -> else if (tx_buf->type == FDICE_TXB_XDP)
          xdp_return_frame_rx_napi(tx_buf->xdpf)
     -> advance consumer counter
  -> if (netif_tx_queue_stopped && space_available)
       netif_tx_wake_queue()
```

**PCIe alignment optimization:** Adopted from ice, TX descriptor data
chunks are aligned to PCIe Max Read Request Size boundaries to maximize
bus efficiency:
```c
max_data += -dma & (FDICE_MAX_READ_REQ_SIZE - 1);
```

---

## 6. ADQ/TC Integration

### 6.1 Traffic Class Architecture

FrankenDrICE implements ADQ (Application Device Queues) through a unified
TC management layer that improves on ice's approach in three key ways:

1. **xarray-based channel management** instead of linked lists
2. **ADQ/switchdev coexistence** instead of mutual exclusion
3. **Integrated BW management** instead of tearing down devlink rate trees

### 6.2 Per-TC Channel Groups

Each traffic class beyond TC0 owns a dedicated group of channels:

```c
struct fdice_tc_info {
    u16                     qoffset;     /* first queue index */
    u16                     qcount;      /* queue count */
    u8                      netdev_tc;   /* kernel TC number */
    u64                     max_bw;      /* max bandwidth (Kbps) */
    u64                     min_bw;      /* guaranteed bandwidth (Kbps) */
};

struct fdice_tc_mgr {
    struct fdice_tc_info    tc[FDICE_MAX_TC];
    u8                      num_tc;
    u16                     ena_tc;      /* TC enable bitmap */
    struct xarray           channel_xa;  /* TC channels by index */
    struct rhashtable       flower_ht;   /* TC flower filters, O(1) lookup */
    struct mutex            lock;
};
```

**TC0** always uses the main PF channel array. **TC1..TC_MAX** each get
a channel group allocated from the device's queue pool, with each group
containing its own set of `fdice_channel` instances.

### 6.3 xarray Channel Management

Channels are managed via xarray keyed by a composite index encoding the
TC number and channel offset within that TC:

```c
#define FDICE_TC_CHAN_KEY(tc, idx)  (((u32)(tc) << 16) | (idx))
```

This replaces ice's `list_head`-based channel list with O(1) lookup,
RCU-safe iteration, and clean insertion/removal semantics. It also
naturally supports sparse allocation when not all TCs have the same
channel count.

### 6.4 TC Flower Steering

TC flower filters steer matching traffic to specific TC channel groups.
Filter management uses rhashtable for O(1) cookie-based lookup, improving
over ice's hlist linear scan:

```
ndo_setup_tc(TC_SETUP_BLOCK)
  -> fdice_setup_tc_cls_flower()
    -> FLOW_CLS_REPLACE:
       -> fdice_tc_parse_flower()      // extract match keys
       -> fdice_tc_validate_filter()   // check against capabilities
       -> fdice_flow_add_rule()        // program via AQ switch recipe
       -> rhashtable_insert()          // track by cookie
    -> FLOW_CLS_DESTROY:
       -> rhashtable_remove()
       -> fdice_flow_del_rule()
```

**Shared filter infrastructure:** The same `fdice_tc_flower_fltr` structure
and programming path works for both ADQ mode (advanced switch rules) and
switchdev mode (FDB flow table entries). The filter's `dest_type` field
selects the programming path:

```c
enum fdice_fltr_dest {
    FDICE_FLTR_DEST_TC_CHANNEL,   /* ADQ: steer to TC channel VSI */
    FDICE_FLTR_DEST_ESWITCH_REP,  /* switchdev: forward to repr */
    FDICE_FLTR_DEST_DROP,         /* drop action */
};
```

### 6.5 ADQ/Switchdev Coexistence

Unlike ice where ADQ and switchdev are mutually exclusive, fdice_ designs
for coexistence from the start:

- In **legacy mode**, TC flower filters program E810 advanced switch recipes
  to steer traffic to TC channel VSIs
- In **switchdev mode**, TC flower filters on representors go through the
  eswitch FDB path, while ADQ filters on the PF uplink use the same
  advanced switch recipe path as legacy mode
- The `fdice_tc_mgr` operates independently of eswitch state; only the
  filter programming backend changes

This avoids the 12+ `ice_is_adq_active()` guard checks scattered through
ice's codebase and enables richer deployment scenarios.

### 6.6 BW Management Integration

ADQ per-channel bandwidth limits integrate into the TX scheduler tree
rather than replacing it:

```
fdice_sched_root
  ├── fdice_sched_node (TC0, default)
  │     ├── channel[0] queues
  │     └── channel[1] queues
  ├── fdice_sched_node (TC1, ADQ, min=1Gbps, max=5Gbps)
  │     └── channel[2] queues
  └── fdice_sched_node (TC2, ADQ, min=500Mbps, max=2Gbps)
        └── channel[3] queues
```

ADQ channels appear as nodes in the same scheduling hierarchy used by
devlink rate objects and DCB ETS configuration. This avoids ice's pattern
of tearing down the devlink rate tree when ADQ is configured.

### 6.7 Validation

TC configuration validation is extracted into a reusable function:

```
fdice_tc_validate(params)
  -> check queue counts (power-of-2 preferred for RSS)
  -> check contiguity of queue offsets
  -> check BW limits (min >= FDICE_MIN_BW_LIMIT, sum <= link speed)
  -> check TC count <= FDICE_MAX_TC
  -> check VSI availability for channel creation
  -> return structured error (not bare -EINVAL)
```

This validator is callable from mqprio setup, ethtool, devlink, and
netlink paths, eliminating duplicated validation logic.

---

## 7. Interrupt Management

### 7.1 EQ Allocation

The `fdice_eq_table` manages the device's MSI-X vector pool:

```c
struct fdice_eq_table {
    struct fdice_eq        *comp_eqs;    /* completion EQ array */
    u16                     num_comp_eqs; /* one per channel */
    struct fdice_eq         async_eq;     /* single async EQ */
    struct fdice_irq_pool  *irq_pool;    /* MSI-X vector pool */
    cpumask_var_t           used_cpus;    /* CPU affinity tracking */
};
```

EQ count matches channel count (one completion EQ per channel) plus one
async EQ for error/control events. The IRQ pool manages MSI-X vector
allocation with support for dynamic expansion when RDMA requests
additional vectors.

### 7.2 MSI-X Vector Assignment

Vector assignment follows a NUMA-aware strategy:

1. Query available MSI-X vectors from PCI subsystem
2. Reserve one vector for async EQ
3. Allocate remaining vectors to completion EQs
4. Assign each completion EQ to a CPU, preferring:
   a. CPUs local to the device's NUMA node
   b. Even distribution across NUMA nodes if vectors > local CPUs
   c. Respect user-configured IRQ affinity masks

**RDMA vector sharing:** The EQ abstraction allows RDMA CQs to register on
existing completion EQs or request dedicated vectors from the IRQ pool.
This is managed through `fdice_eq_alloc_irq()` / `fdice_eq_release_irq()`
APIs exported to the RDMA auxiliary driver.

### 7.3 IRQ Affinity

IRQ affinity is configured at EQ creation and can be updated at runtime:

```
fdice_eq_set_affinity(eq, cpu)
  -> irq_set_affinity_hint(eq->irq, cpumask_of(cpu))
  -> eq->cpu = cpu
  -> update eq_table->used_cpus mask
```

The channel's NUMA node is derived from its EQ's CPU affinity, ensuring
page pool allocations and buffer processing are NUMA-local.

### 7.4 Interrupt Coalescing (DIM Integration)

FrankenDrICE uses the kernel's Dynamic Interrupt Moderation (DIM)
framework for adaptive coalescing, with per-direction DIM instances:

```c
struct fdice_rxq {
    /* ... */
    struct dim              dim;         /* RX interrupt moderation */
    /* ... */
};

struct fdice_txq {
    /* ... */
    struct dim              dim;         /* TX interrupt moderation */
    /* ... */
};
```

**DIM integration in NAPI poll:**
```
fdice_napi_poll(napi, budget)
  -> poll TX CQs (all TCs)
  -> poll RX CQ
  -> if work_done < budget:
     -> fdice_rx_dim_update(&rxq->dim, packets, bytes)
     -> fdice_tx_dim_update(&txq->dim, packets, bytes)
     -> arm all CQs
     -> napi_complete_done()
  -> else:
     -> continue polling (CQs remain disarmed)
```

**Coalescing parameters** are written to E810 ITR (Interrupt Throttle Rate)
registers. DIM callbacks translate the selected profile (lowest, low,
medium, high) into ITR register values:

```
fdice_dim_work(work)
  -> dim_calc_net_dim(&dim)
  -> map profile to ITR value
  -> writel(itr_val, eq->itr_reg)
```

**Static coalescing** is also supported via ethtool for users who prefer
fixed interrupt rates. When static mode is selected, DIM is disabled and
the user-specified ITR values are written directly.

---

## 8. Key Structs

### fdice_eq -- Event Queue

```c
struct fdice_eq {
    struct xarray           cq_table;    /* CQN -> fdice_cq mapping */
    int                     irq;         /* MSI-X IRQ number */
    int                     cpu;         /* assigned CPU */
    u16                     idx;         /* EQ index */
    u8                      type;        /* FDICE_EQ_COMP | FDICE_EQ_ASYNC */
    void __iomem           *itr_reg;     /* ITR register for coalescing */
    struct fdice_eq_table  *table;       /* parent table */
};
```

### fdice_cq -- Completion Queue

```c
struct fdice_cq {
    struct fdice_cqwq       wq;          /* CQ ring buffer */
    void                  (*comp_handler)(struct fdice_cq *cq);
    struct fdice_eq        *eq;          /* parent EQ */
    struct napi_struct     *napi;        /* associated NAPI (ethernet) */
    u32                     cqn;         /* CQ number (xarray key) */
    u16                     event_ctr;   /* completion event counter */
    u8                      state;       /* arm/disarm state */
    __be32                 *arm_db;      /* arm doorbell */
    __be32                 *ci_db;       /* consumer index doorbell */
};
```

### fdice_wq -- Work Queue (cyclic variant)

```c
struct fdice_wq_cyc {
    struct fdice_frag_buf_ctrl fbc;      /* fragment buffer control */
    __be32                 *db;          /* doorbell pointer */
    u32                     sz;          /* ring size (entries) */
    u32                     mask;        /* sz - 1 */
    u16                     stride;      /* WQE size in bytes */
    u16                     pc;          /* producer counter */
    u16                     cc;          /* consumer counter */
};
```

### fdice_channel -- Per-NAPI Queue Group

```c
struct fdice_channel {
    struct napi_struct       napi;
    struct fdice_rxq         rxq;
    struct fdice_txq         txq[FDICE_MAX_TC];
    struct fdice_xdp_sq      xdp_sq;
    struct fdice_icosq       icosq;
    struct fdice_eq         *eq;
    struct fdice_params     *params;
    int                      cpu;
    int                      numa_node;
    u16                      idx;
    struct fdice_priv       *priv;
};
```

### fdice_txq -- Transmit Queue

```c
struct fdice_txq {
    __cacheline_group_begin_aligned(read_mostly);
    struct fdice_wq_cyc      wq;         /* TX descriptor ring */
    struct fdice_cq          cq;         /* TX completion queue */
    void __iomem            *tail_reg;   /* hardware tail register */
    struct fdice_channel    *channel;
    struct net_device       *netdev;
    __cacheline_group_end_aligned(read_mostly);

    __cacheline_group_begin_aligned(read_write);
    u32                      pc;         /* producer counter */
    u32                      cc;         /* consumer counter */
    __cacheline_group_end_aligned(read_write);

    __cacheline_group_begin_aligned(cold);
    struct fdice_tx_buf     *tx_buf;     /* buffer tracking array */
    struct fdice_sq_dma     *dma_fifo;   /* DMA tracking FIFO */
    u32                      dma_fifo_pc;
    u32                      dma_fifo_cc;
    u32                      dma_fifo_mask;
    struct dim               dim;        /* interrupt moderation */
    u8                       tc;         /* traffic class */
    struct fdice_txq_stats   stats;
    __cacheline_group_end_aligned(cold);
};
```

### fdice_rxq -- Receive Queue

```c
struct fdice_rxq {
    __cacheline_group_begin_aligned(read_mostly);
    struct fdice_wq_cyc      wq;         /* RX descriptor ring */
    struct fdice_cq          cq;         /* RX completion queue */
    struct page_pool        *page_pool;
    struct page_pool        *hdr_page_pool; /* header split pool */
    void __iomem            *tail_reg;
    struct fdice_channel    *channel;
    struct net_device       *netdev;
    __cacheline_group_end_aligned(read_mostly);

    __cacheline_group_begin_aligned(read_write);
    u16                      next_to_use;
    u16                      next_to_clean;
    struct xdp_rxq_info      xdp_rxq;
    __cacheline_group_end_aligned(read_write);

    __cacheline_group_begin_aligned(cold);
    struct fdice_rx_buf     *rx_buf;     /* buffer descriptor array */
    struct dim               dim;        /* interrupt moderation */
    u16                      count;      /* ring size */
    u16                      buf_size;   /* per-buffer size */
    struct fdice_rxq_stats   stats;
    void                   (*handle_rx_cqe)(struct fdice_rxq *, struct fdice_cq *);
    void                   (*post_wqes)(struct fdice_rxq *);
    __cacheline_group_end_aligned(cold);
};
```

### fdice_rx_buf -- RX Buffer Descriptor

```c
struct fdice_rx_buf {
    netmem_ref               netmem;
    dma_addr_t               dma;
    u32                      offset;
    u16                      truesize;
    u8                       flags;      /* FDICE_RXB_* */
};
```

---

## Design Decisions Summary

| Decision | Rationale | Reference |
|----------|-----------|-----------|
| 3-level EQ/CQ/WQ hierarchy | RDMA/ethernet sharing, flexible interrupt routing | Exploration 02 sec 4.4 |
| Embedded CQ in TXQ/RXQ | Cache locality, clear ownership | Exploration 02 sec 4.1 |
| Channel-based queue grouping | Eliminates pointer chasing in hot path | Exploration 02 sec 4.2 |
| xarray for CQ-to-EQ mapping | RCU-safe, O(1) lookup, modern kernel API | Improvement over mlx5 radix tree |
| Page pool with NUMA pinning | Local memory for RX buffers | Exploration 09 sec Recommendations |
| Fragmentation bias on page pool | Better memory utilization | Exploration 09 mlx5 pattern |
| Typed DMA FIFO for TX | Clean separation, cache-friendly completion | Exploration 09 sec TX DMA |
| Unified queue state machine | Predictable lifecycle, proper cleanup | Exploration 02 sec 4.7 |
| xarray channel management for ADQ | O(1) lookup, sparse allocation | Exploration 14 sec Recommendations |
| ADQ/switchdev coexistence | Removes artificial limitation | Exploration 14 sec 4 |
| rhashtable for flower filters | O(1) cookie lookup | Exploration 14 sec 3 |
| DIM per-direction | Adaptive coalescing for both RX and TX | Exploration 02 sec 3 |
| Cache-line-grouped struct layout | Hot path performance | Exploration 02 sec 4.5 |
