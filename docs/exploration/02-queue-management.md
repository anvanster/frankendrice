<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 02: Queue Management -- mlx5 vs ice

## Overview

This document compares the queue management architectures of mlx5 (ConnectX)
and ice (E810) drivers, mapping their abstraction layers, structural
differences, and identifying patterns worth adopting for FrankenDrICE.

**Key files examined:**

- mlx5: `core/lib/eq.h`, `core/eq.c`, `core/wq.h`, `core/wq.c`, `core/en.h`, `core/en_txrx.c`
- ice: `ice.h`, `ice_txrx.h`, `ice_txrx.c`, `ice_controlq.h`, `ice_lib.c`, `ice_base.c`
- kernel: `include/linux/mlx5/cq.h`

---

## 1. mlx5 Queue Hierarchy: EQ -> CQ -> WQ

mlx5 implements a three-level hardware queue hierarchy that mirrors the
ConnectX hardware architecture. Each level is a distinct abstraction with
clean separation of concerns.

### 1.1 Event Queues (EQ) -- Hardware Interrupt Layer

**Structs:** `mlx5_eq`, `mlx5_eq_async`, `mlx5_eq_comp`, `mlx5_eq_table`

The EQ is the interrupt delivery mechanism. Each EQ maps to one MSI-X vector.
There are two specialized EQ types:

- **`mlx5_eq_async`** -- Handles asynchronous events (port changes, errors,
  CQ errors, WQ catastrophic errors, SRQ events). Single instance. Uses a
  notifier block and spinlock for concurrency with resiliency flows.

- **`mlx5_eq_comp`** -- Handles completion events. Multiple instances (one per
  CPU/channel). Contains a tasklet context (`mlx5_eq_tasklet`) for deferred
  CQ processing.

**Key design:** Each EQ owns a `mlx5_cq_table` (radix tree) that maps CQ
numbers to `mlx5_core_cq` objects. When an EQE arrives:

```
IRQ -> mlx5_eq_comp_int()
  -> next_eqe_sw(eq)           // read EQE from ring buffer
  -> extract cqn from EQE
  -> mlx5_eq_cq_get(eq, cqn)  // radix tree lookup
  -> cq->comp(cq, eqe)        // callback into driver (mlx5e_completion_event)
  -> napi_schedule()           // trigger NAPI poll
```

The `mlx5_eq_table` manages all EQs for a device, tracking async EQs,
completion EQs, IRQ table references, and CPU affinity via `used_cpus` mask.

### 1.2 Completion Queues (CQ) -- Event Notification Layer

**Structs:** `mlx5_core_cq` (kernel), `mlx5e_cq` (ethernet), `mlx5_cqwq` (work queue)

CQs sit between EQs and WQs, providing typed completion notifications.

**`mlx5_core_cq`** (include/linux/mlx5/cq.h) -- The hardware-facing CQ:
- `cqn` -- Hardware CQ number
- `set_ci_db` / `arm_db` -- Doorbell registers for consumer index and arming
- `comp()` callback -- Called from EQ interrupt handler
- `event()` callback -- Called for CQ error events
- `arm_sn` -- Arm sequence number for interrupt coalescing
- `tasklet_ctx` -- Deferred processing support with its own comp callback

**`mlx5e_cq`** (en.h) -- The ethernet driver's CQ wrapper:
- `wq` (`mlx5_cqwq`) -- The CQ work queue for reading CQEs
- `event_ctr` -- Tracks completion events
- `napi` -- Pointer to associated NAPI struct
- `mcq` (`mlx5_core_cq`) -- Embedded core CQ

**`mlx5_cqwq`** (wq.h) -- CQ ring buffer abstraction:
- `fbc` (`mlx5_frag_buf_ctrl`) -- Fragment buffer control for DMA
- `db` -- Doorbell pointer
- `cc` -- Consumer counter
- Supports both standard CQE polling and enhanced completion mode
- Owner bit / validity iteration count for hardware synchronization

**CQ-to-EQ binding:** `mlx5_eq_add_cq()` inserts a CQ into an EQ's radix
tree. Each CQ is associated with exactly one EQ (and thus one MSI-X vector).

### 1.3 Work Queues (WQ) -- Data Transport Layer

**Structs:** `mlx5_wq_cyc`, `mlx5_wq_ll`, `mlx5_wq_qp`, `mlx5_wq_ctrl`, `mlx5_wq_param`

WQs are the actual descriptor rings where packets are posted and completed.
mlx5 provides multiple WQ types:

- **`mlx5_wq_cyc`** -- Cyclic (circular) work queue. Used for SQs and
  regular RQs. Tracks `wqe_ctr` (producer), `cur_sz` (fill level), uses
  `fbc` for DMA buffer management. Operations: push, pop, get_wqe, ctr2ix.

- **`mlx5_wq_ll`** -- Linked-list work queue. Used for MPWQE (multi-packet
  WQE) RQs and SRQs. Has `head` and `tail_next` pointers for O(1) push/pop
  with linked WQE entries.

- **`mlx5_wq_qp`** -- QP work queue, a composite of two `mlx5_wq_cyc`
  instances (one for RQ, one for SQ). Used by RDMA queue pairs.

- **`mlx5_wq_ctrl`** -- Control structure managing the underlying DMA buffer
  (`mlx5_frag_buf`) and doorbell (`mlx5_db`).

### 1.4 Ethernet Queue Composition (en.h)

At the ethernet driver level, these primitives compose into higher-level
structures:

**`mlx5e_txqsq`** (Transmit Queue / Send Queue):
- Contains `mlx5_wq_cyc wq` -- The actual WQ ring
- Contains `mlx5e_cq cq` -- Embedded CQ for TX completions
- Producer/consumer counters (`pc`/`cc`)
- DMA FIFO for tracking DMA mappings
- SKB FIFO for tracking socket buffers
- References to `mlx5e_channel` and `mlx5e_priv`
- Supports MPWQE (multi-packet WQE) mode

**`mlx5e_rq`** (Receive Queue):
- Union of two WQ modes:
  - `wqe` mode: `mlx5_wq_cyc` with per-WQE fragment tracking
  - `mpwqe` mode: `mlx5_wq_ll` with UMR (User Memory Region) for
    multi-packet WQE / SHAMPO (header-data split) support
- Contains `mlx5e_cq cq` -- Embedded CQ for RX completions
- Contains `mlx5e_cq_decomp cqd` -- CQE decompression state
- Function pointers for RX processing: `handle_rx_cqe`, `post_wqes`, `dealloc_wqe`
- DIM (Dynamic Interrupt Moderation) support

**`mlx5e_channel`** -- The per-CPU grouping:
- `rq` -- One receive queue
- `sq[MLX5_MAX_NUM_TC]` -- Array of send queues (one per traffic class)
- `rq_xdpsq` -- XDP send queue for the RQ
- `icosq` -- Internal control operations SQ
- `async_icosq` -- Async ICOSQ (for kTLS, etc.)
- `napi` -- NAPI struct for this channel
- `xskrq` / `xsksq` -- AF_XDP zero-copy queues
- `qos_sqs` -- QoS send queues (RCU-protected, dynamically sized)

### 1.5 mlx5 NAPI Poll Flow

`mlx5e_napi_poll()` demonstrates the full hierarchy in action:

```
mlx5e_napi_poll(napi, budget)
  1. Poll TX CQs:  for each TC: mlx5e_poll_tx_cq(&sq[i].cq, budget)
  2. Poll QoS TX CQs: for each qos_sq: mlx5e_poll_tx_cq(&sq->cq, budget)
  3. Poll XDP CQs: mlx5e_poll_xdpsq_cq(&xdpsq->cq)
  4. Poll RX CQs:  mlx5e_poll_rx_cq(&rq->cq, budget)
  5. Poll ICOSQ CQ: mlx5e_poll_ico_cq(&icosq.cq)
  6. Post RX WQEs: rq->post_wqes(rq)
  7. If not busy: arm all CQs via mlx5e_cq_arm()
```

Each CQ is polled independently, consuming CQEs that reference WQEs. After
polling, CQs are re-armed to generate new completion events via their EQ.

---

## 2. ice Queue Model: Flat Ring-Based Architecture

ice uses a simpler, flatter queue model typical of Intel NICs, built around
descriptor rings directly mapped to hardware queues.

### 2.1 Interrupt Vectors (ice_q_vector)

**Struct:** `ice_q_vector`

The q_vector is the interrupt-level grouping, analogous to mlx5's EQ but
combining interrupt delivery with queue ownership:

- `vsi` -- Back-pointer to the VSI (Virtual Station Interface)
- `v_idx` / `reg_idx` -- Vector indices (software and hardware)
- `num_ring_rx` / `num_ring_tx` -- Ring counts
- `napi` -- NAPI struct (directly embedded, not in a sub-channel)
- `rx` / `tx` -- `ice_ring_container` structs holding linked lists of rings
- `irq` -- MSI-X mapping
- `intrl` -- Interrupt rate limiting (usecs)
- `wb_on_itr` -- Write-back on ITR optimization flag

**Key difference from mlx5:** The q_vector directly owns rings. There is no
intermediate CQ/EQ indirection -- the MSI-X vector fires, NAPI polls the
descriptor rings directly.

### 2.2 Descriptor Rings (ice_tx_ring, ice_rx_ring)

**Structs:** `ice_tx_ring`, `ice_rx_ring`, `ice_ring_container`, `ice_ring_stats`

These are the core data path structures, designed with cache-line-aware
layout (`__cacheline_group_begin/end_aligned`):

**`ice_rx_ring`:**
- `desc` -- Descriptor ring memory (DMA)
- `pp` -- Page pool for buffer allocation
- `q_vector` -- Back-reference to interrupt vector
- `tail` -- MMIO tail register
- `rx_fqes` / `xdp_buf` -- Buffer tracking (union for regular vs XDP)
- `count` -- Descriptor count
- `next_to_use` / `next_to_clean` -- Producer/consumer indices
- `ring_stats` -- Per-ring statistics
- `next` -- Linked list pointer (multiple rings per q_vector)
- `vsi` -- Back-reference to VSI
- `q_index` / `reg_idx` -- Queue indices (software/hardware)
- `dcb_tc` -- Traffic class assignment

**`ice_tx_ring`:**
- Similar layout to RX with TX-specific fields
- `tx_buf` -- TX buffer tracking array
- `tx_lock` -- Spinlock for TX
- `txq_teid` -- Scheduler TEID for the TX queue
- `q_handle` -- Per-TC queue handle
- `quanta_prof_id` -- Quanta profile for scheduling
- `tstamp_ring` -- Optional timestamp ring

**`ice_ring_container`:**
- Union of `rx_ring` / `tx_ring` head pointers
- `dim` -- Dynamic Interrupt Moderation data
- `itr_idx` / `itr_setting` / `itr_mode` -- ITR configuration
- `type` -- Container type (RX or TX)

Rings within a container are linked via `->next` pointers:
```c
#define ice_for_each_rx_ring(pos, head) \
    for (pos = (head).rx_ring; pos; pos = pos->next)
```

### 2.3 VSI (Virtual Station Interface) -- ice_vsi

**Struct:** `ice_vsi`

The VSI is the top-level queue management entity, owning all queues for a
virtual function or port:

- `q_vectors` -- Array of q_vector pointers
- `tx_rings` / `rx_rings` -- Arrays of ring pointers
- `txq_map` / `rxq_map` -- Queue-to-hardware index mapping
- `alloc_txq` / `alloc_rxq` / `num_txq` / `num_rxq` -- Queue counts
- `num_q_vectors` -- Interrupt vector count
- `tc_cfg` -- Traffic class configuration
- `xdp_rings` -- XDP ring array
- `port_info` -- Physical port info

### 2.4 Control Queues (ice_ctl_q_info, ice_ctl_q_ring)

Separate from the data path, ice has admin/control queues for firmware
communication:

- `ice_ctl_q_info` -- Contains SQ (send) and RQ (receive) control queue rings
- `ice_ctl_q_ring` -- Descriptor ring with `next_to_use`/`next_to_clean`
- Three types: Admin (AQ), Mailbox (MBX), Sideband (SB)
- Protected by mutexes (`sq_lock`, `rq_lock`)

### 2.5 ice NAPI Poll Flow

`ice_napi_poll()` is simpler than mlx5's:

```
ice_napi_poll(napi, budget)
  1. For each TX ring in q_vector->tx:
     - ice_clean_tx_irq(tx_ring, budget) or ice_xmit_zc()
  2. For each RX ring in q_vector->rx:
     - ice_clean_rx_irq(rx_ring, budget) or ice_clean_rx_irq_zc()
  3. If complete: ice_net_dim() + ice_enable_interrupt()
     If not: ice_set_wb_on_itr() for partial completions
```

No CQ abstraction -- the NAPI handler reads descriptors directly from the
hardware rings.

---

## 3. Key Structural Differences

| Aspect | mlx5 | ice |
|--------|------|-----|
| **Queue levels** | 3 (EQ -> CQ -> WQ) | 1 (descriptor rings) |
| **Interrupt routing** | EQ radix tree maps CQN to CQ | q_vector directly owns ring linked lists |
| **Completion model** | Separate CQ with CQEs referencing WQEs | Descriptor DD (done) bits in-place |
| **Ring types** | Cyclic (`wq_cyc`), linked-list (`wq_ll`), QP composite | Single ring type per direction |
| **Per-CPU grouping** | `mlx5e_channel` (rich, many queue types) | `ice_q_vector` (simpler, ring containers) |
| **CQ/ring ownership** | CQ embedded in SQ/RQ struct, WQ also embedded | Rings linked via `->next` in containers |
| **Multi-TC support** | `sq[MLX5_MAX_NUM_TC]` array in channel | Separate rings with `dcb_tc` tag |
| **NAPI location** | In `mlx5e_channel` | In `ice_q_vector` |
| **Interrupt coalescing** | CQ arm/disarm mechanism | ITR registers + WB-on-ITR |
| **CQE compression** | `mlx5e_cq_decomp` mini-CQE arrays | N/A (no CQ abstraction) |
| **DIM support** | Per-RQ and per-SQ DIM | Per-container DIM |
| **Control operations** | Dedicated ICOSQ (sync + async) | Separate control queue subsystem |
| **RDMA integration** | `mlx5_wq_qp` composite, shared EQ/CQ infra | Separate irdma driver with own queues |
| **Hardware doorbell** | Per-WQ and per-CQ doorbells | Per-ring tail register |
| **Queue virtualization** | Through channel abstraction | Through VSI abstraction |

### 3.1 Abstraction Depth

mlx5's three-level hierarchy provides:
- **Flexibility:** One EQ can service many CQs; one CQ can track multiple WQs
- **Sharing:** RDMA and ethernet share the same EQ/CQ infrastructure
- **Efficiency:** CQE compression reduces PCIe traffic
- **Isolation:** EQ-level interrupt routing enables fine-grained CPU affinity

ice's flat model provides:
- **Simplicity:** Direct mapping from interrupt to ring, fewer indirections
- **Lower latency:** No radix tree lookup in the interrupt path
- **Predictability:** 1:1 mapping between interrupt and queue group

### 3.2 Composition Patterns

mlx5 uses **embedding** -- each SQ/RQ contains its own CQ and WQ as struct
members. This provides strong locality (the CQ data is in the same cache
lines as the SQ/RQ data) and clear ownership.

ice uses **linked lists** -- rings are linked via `->next` pointers within
ring containers. This allows flexible N:1 mapping of rings to interrupt
vectors but requires pointer chasing.

### 3.3 Control Path Separation

mlx5 has dedicated internal control operation SQs (`icosq`, `async_icosq`)
that share the same CQ/WQ infrastructure as data path queues. This allows
control operations (UMR posts, kTLS offload) to be processed in the same
NAPI context.

ice separates control queues (`ice_ctl_q_info`) entirely from the data path,
using a different descriptor format and mutex-based synchronization. The
Admin Queue is the hardware boundary for all control operations.

---

## 4. Patterns Worth Adopting for fdice_

### 4.1 ADOPT: mlx5-Style Embedded CQ in Queue Structs

The mlx5 pattern of embedding a CQ directly in each SQ/RQ struct provides
excellent cache locality and clear ownership semantics. For fdice_, define:

```
struct fdice_cq { ... };       // Completion queue abstraction

struct fdice_txq {
    struct fdice_cq cq;        // Embedded, not pointed-to
    /* ... ring data ... */
};

struct fdice_rxq {
    struct fdice_cq cq;        // Embedded, not pointed-to
    /* ... ring data ... */
};
```

**Rationale:** E810 does not have hardware CQs like ConnectX, but a software
CQ abstraction layer would let us unify completion handling, enable future
CQE-style batching optimizations, and provide a clean boundary between
"hardware wrote a descriptor" and "software processed the completion."

### 4.2 ADOPT: mlx5-Style Channel Grouping

The `mlx5e_channel` pattern of grouping all queues for one NAPI context into
a single struct is superior to ice's linked-list approach:

```
struct fdice_channel {
    struct fdice_rxq   rxq;
    struct fdice_txq   txq[FDICE_MAX_TC];
    struct fdice_xdpsq xdp_sq;
    struct napi_struct napi;
    int cpu;
    /* ... */
};
```

**Rationale:** This gives better cache locality, eliminates pointer chasing
in the hot path, and makes per-CPU resource management straightforward.

### 4.3 ADOPT: Typed WQ Abstractions

mlx5's separation of `mlx5_wq_cyc` (cyclic) and `mlx5_wq_ll` (linked-list)
is worth preserving as fdice_ WQ primitives. Even though E810 uses cyclic
rings for data queues, having the abstraction enables:

- Clean separation of ring buffer management from queue semantics
- Reuse for different queue types (data, control, XDP)
- Testability of ring logic in isolation

### 4.4 ADOPT: Separate EQ-Like Interrupt Routing Table

While E810 does not have hardware EQs, building an interrupt routing
abstraction above the raw MSI-X vectors would:

- Enable dynamic CQ-to-vector remapping
- Support RDMA/ethernet interrupt sharing (critical for irdma integration)
- Provide a clean abstraction for interrupt coalescing

### 4.5 KEEP FROM ICE: Cache-Line-Aware Struct Layout

ice's explicit `__cacheline_group_begin/end_aligned` annotations with
`read_mostly`, `read_write`, and `cold` groupings is excellent practice that
mlx5 does less consistently. Adopt this for all fdice_ data path structs.

### 4.6 KEEP FROM ICE: Admin Queue as Hardware Boundary

The E810 Admin Queue is the hardware control interface. Keep ice's clean
separation of control queues (`ice_ctl_q_info`) from data path queues,
but implement the control queue using fdice_ WQ primitives rather than
a separate ring abstraction.

### 4.7 IMPROVE: Unified Queue Lifecycle

Both drivers have ad-hoc queue lifecycle management. fdice_ should define
a clear state machine for all queue types:

```
UNINITIALIZED -> ALLOCATED -> CREATED -> ENABLED -> ACTIVE -> DISABLED -> DESTROYED
```

with well-defined transitions, error handling, and resource cleanup at each
stage.

---

## 5. Summary

mlx5's three-level queue hierarchy (EQ -> CQ -> WQ) reflects ConnectX
hardware and provides flexibility for multi-protocol (ethernet + RDMA)
operation. ice's flat ring model reflects E810's simpler hardware but
sacrifices abstraction.

For FrankenDrICE, the strategy is:
- **Below the AQ interface:** Use E810-native descriptor rings (ice-informed)
- **Above the AQ interface:** Layer mlx5-inspired abstractions (channel
  grouping, embedded CQ, typed WQ) to gain flexibility, cache locality,
  and future RDMA readiness
- **At the boundary:** Clean interrupt routing abstraction that can serve
  both ethernet and RDMA consumers
