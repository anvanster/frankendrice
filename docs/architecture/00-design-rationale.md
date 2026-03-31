<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# FrankenDrICE Design Rationale

This document explains why each major architectural pattern was chosen for the
fdice_ driver and how it improves on the ice and mlx5 reference implementations.
Every decision traces back to findings from the exploration phase.

---

## 1. Design Philosophy

### The "Study, Learn, Rewrite" Principle

FrankenDrICE is not a fork of ice, nor a port of mlx5. It is a clean-sheet E810
driver that studies both architectures, understands why they made their choices,
and rewrites everything under the `fdice_` namespace with deliberate improvements.

The reference drivers serve distinct roles:

- **ice** teaches us what the E810 hardware requires. Its Admin Queue command
  sequences, descriptor formats, DDP package handling, and register-level
  interactions define the hardware contract. We study ice to understand the
  firmware protocol, then rewrite the implementation with better error handling,
  cleaner abstractions, and improved lifecycle management.

- **mlx5** teaches us what a well-architected modern NIC driver looks like. Its
  layered initialization, auxiliary device model, health monitoring, flow steering
  tree, and devlink integration represent the state of the art in Linux network
  driver design. We adopt these software patterns above the hardware boundary.

- **irdma** teaches us the correct RDMA integration model for E810 hardware,
  where separate MMIO register spaces and the CQP make loose coupling natural.

### The Admin Queue Hardware Boundary

The E810 Admin Queue (AQ) command interface is the definitive hardware boundary
in the fdice_ architecture:

**Below the AQ interface** -- everything is dictated by E810 firmware:
- The 32-byte `libie_aq_desc` descriptor format is fixed
- All ~100 AQ opcodes and their parameter structs are firmware protocol constants
- Ring buffer mechanics (head/tail register protocol, DD/CMP flags) are hardware
- DDP package format, signing verification, and download sequences are fixed
- Control queue types (Admin, Mailbox, Sideband) match hardware expectations

**Above the AQ interface** -- everything is software architecture:
- Init/teardown lifecycle and layer separation
- Queue management abstractions (channels, CQ wrappers, typed WQs)
- Flow steering organization (tree vs flat)
- Health monitoring and automated recovery
- Devlink integration depth and reporter granularity
- RDMA coupling model and auxiliary device management
- Eswitch mode management and representor framework

This boundary is not arbitrary. It reflects a hardware truth: the E810 firmware
only sees AQ descriptors and DMA buffers. Everything the driver builds above
that interface is invisible to firmware and can be redesigned freely.

### Why mlx5 Patterns Above, ice Patterns Below

mlx5 was designed for ConnectX hardware that is fundamentally more capable than
E810 in its control plane (firmware mailbox slots, hardware flow tables, EQ/CQ
hierarchy). However, mlx5's software architecture -- how it organizes
subsystems, manages lifecycles, and integrates with kernel frameworks -- is
superior regardless of hardware. These patterns are pure software engineering
and translate directly.

ice was written for E810 and knows its hardware intimately. Its AQ command
sequences, capability discovery, DDP loading, and register interactions are
the authoritative reference for E810 firmware communication. But ice's software
architecture carries legacy design choices (flat init, flag-gated cleanup,
monolithic probe) that we can improve upon.

The fdice_ strategy: **mlx5 software architecture above the AQ, ice hardware
knowledge below it**.

---

## 2. Pattern Adoption Table

| Subsystem | Pattern Source | What We Adopt | What We Improve | Exploration Ref |
|-----------|---------------|---------------|-----------------|-----------------|
| **Init/Teardown** | mlx5 (primary) | 4-layer hierarchy: mdev_init / function_setup / init_once / load | Table-driven subsystem registration for guaranteed init/cleanup symmetry; state-bit gating for partial init safety | Q1, Q3 |
| **PCI Probe** | mlx5 + ice | Thin probe delegating to layers (mlx5); kdump FLR handling (ice) | Devlink-first allocation with lock held through init; recovery mode detection from ice | Q1 |
| **Queue Management** | mlx5 (primary) | Embedded CQ in SQ/RQ; channel-based per-CPU grouping; typed WQ abstractions | Unified queue lifecycle state machine (UNINITIALIZED through DESTROYED); cache-line-aware layout from ice | Q2 |
| **Interrupt Routing** | mlx5 | EQ-like interrupt routing table above MSI-X vectors | Software abstraction enabling RDMA/ethernet interrupt sharing without hardware EQs | Q2 |
| **Flow Steering** | mlx5 (tree) + ice (recipes) | fs_node generic tree with refcounting; namespace separation; command vtable | Tree organization over E810's flat recipe hardware; recipe reuse tracking; cleaner FDIR API hiding dummy-packet details | Q4 |
| **RDMA Integration** | ice/irdma (primary) | Loose coupling via ops struct + data snapshot; PF as sole AQ issuer | Richer capability snapshot at probe; synchronous pre/post event model; shared IRQ pool with RDMA partition | Q5 |
| **SR-IOV / VF Mgmt** | mlx5 (eswitch) + ice (virtchnl) | Vport abstraction with devlink_port; two-mode eswitch; pluggable rep ops | Unified VF state machine; per-vport flow counters; software-emulated TC offloads on representors | Q6 |
| **Eswitch / Switchdev** | mlx5 (structure) + ice (bridge) | Richer eswitch struct with vports xarray and offloads state; bridge FDB via rhashtable | ADQ/switchdev coexistence (eliminating mutual exclusion); graceful mode transitions with VFs present | Q6, Q14, Q15 |
| **Devlink** | mlx5 (primary) + ice (regions) | Per-port health reporters; auto-recovery callbacks; extensive parameters | Keep ice's NVM/SRAM/devcaps regions; combine mlx5 trap support with ice's granular version reporting | Q7 |
| **Health Monitoring** | mlx5 (primary) | Multi-layer health: buffer polling, syndrome diagnostics, 5 reporters, automated recovery | Error history ring buffer; reset escalation (per-queue to PF to core to global); recovery success metrics | Q7, Q8 |
| **DMA / Memory** | mlx5 (RX/TX) + ice (control) | Direct page pool with NUMA-aware creation; typed DMA FIFO for TX; page fragmentation bias | Avoid libeth indirection; device-managed allocation for control queues from ice; unified fdice_rx_buf descriptor | Q9 |
| **DCB / QoS** | Hybrid | Register-based interface (mlx5); config triple local/desired/remote (ice); BEFORE/AFTER RDMA notification (ice) | SW-owned port buffer management for PFC headroom; trust state DSCP model (not port-wide mode switch); per-TC max rate | Q10 |
| **Admin Queue** | ice (hardware) + mlx5 (software) | E810 ring mechanics, descriptor format, opcode map (ice); command abstraction concepts (mlx5) | Async completion via workqueue (not busy-wait); multiple outstanding commands; per-opcode statistics; subsystem-grouped command APIs | Q11 |
| **DDP Packages** | ice (required) | Package format parsing, signing verification, download sequence, safe mode fallback | Cleaner error propagation; split monolithic init_pkg into testable stages; validation hardening with bounds checks | Q12 |
| **PTP / Clock** | ice (required) | PHC ops, TX/RX timestamp mechanisms, GPIO pin support, cross-timestamping, state machine | Cleaner lock hierarchy documentation; configurable TX timestamp timeout; testable PHC conversion logic; structured PTP statistics | Q13 |
| **ADQ / TC** | ice (mechanism) + improvements | mqprio MODE_CHANNEL with per-TC VSIs; TC flower via advanced switch recipes | Unified TC manager working in both legacy and switchdev modes; xarray channel management; rhashtable filter lookup; shared filter infrastructure | Q14 |

---

## 3. Key Architectural Decisions

### 3.1 Layered Init/Teardown

**Decision**: Separate initialization into four distinct layers with matching
cleanup functions, following mlx5's proven pattern.

**Motivation** (Q1, Q3): ice's `ice_probe()` is a 136-line monolithic function
with cyclomatic complexity 22. Its teardown in `ice_remove()` has ordering
anomalies -- `ice_deinit_dev()` calls `ice_reset()` after `ice_deinit_hw()` has
already destroyed the control queues. AQ waiters are cancelled after the queues
they wait on are freed. mlx5's probe_one has complexity 11 and delegates to
well-separated layers.

**fdice_ layers**:

| Layer | Init | Cleanup | Scope | Repeatable? |
|-------|------|---------|-------|-------------|
| 0 | `fdice_mdev_init` | `fdice_mdev_uninit` | Core device: mutexes, AQ cmd SW, health, debugfs | No (once per device) |
| 1 | `fdice_function_setup` | `fdice_function_teardown` | FW handshake: AQ init, cap discovery, health poll | No (once per device) |
| 2 | `fdice_init_once` | `fdice_cleanup_once` | One-time SW subsystem allocation (eswitch, flow steering, SRIOV) | No (once per device) |
| 3 | `fdice_load` | `fdice_unload` | Runtime HW activation (IRQs, EQs, scheduling, netdev) | Yes (devlink reload) |

**Key improvements over ice**:
- `FDICE_INTERFACE_STATE_UP` bit gates cleanup of partially-initialized devices
- Layer 3 (load/unload) is repeatable, enabling devlink reload from day one
- Teardown escalation: normal AQ teardown, then force-poll teardown, then fast
  NIC-state transition (adopted from mlx5's three-mode HCA teardown)
- Table-driven subsystem registration (Q3 R3) eliminates divergence between init
  and cleanup sequences
- AQ waiters cancelled before control queues destroyed (fixing ice's race)
- Cleanup functions are idempotent and unconditional (no flag-gating)

### 3.2 Queue Hierarchy with CQ Abstraction

**Decision**: Introduce a software Completion Queue abstraction over E810's flat
descriptor rings, using mlx5-style embedded CQ and channel grouping.

**Motivation** (Q2): E810 hardware uses simple descriptor rings with DD-bit
completion, not hardware CQs like ConnectX. However, a software CQ layer
provides unified completion handling, enables future CQE-style batching, and
creates a clean boundary between hardware descriptor writeback and software
completion processing. mlx5's channel grouping (all queues for one NAPI in a
single struct) gives better cache locality than ice's linked-list ring
containers.

**fdice_ queue structure**:

```
fdice_channel (per-CPU, one NAPI)
  +-- fdice_rxq (embedded fdice_cq for RX completions)
  +-- fdice_txq[FDICE_MAX_TC] (each with embedded fdice_cq)
  +-- fdice_xdpsq (XDP send queue)
  +-- struct napi_struct
```

**Key improvements**:
- Embedded CQ provides cache locality (mlx5 pattern) vs pointer-chasing (ice)
- Typed WQ abstractions (`fdice_wq_cyc`) separate ring management from semantics
- Explicit queue lifecycle state machine with defined transitions
- Cache-line-aware struct layout using ice's `__cacheline_group` annotations
- Software interrupt routing table enabling RDMA/ethernet vector sharing

### 3.3 Tree-Based Flow Steering over Flat Recipes

**Decision**: Build an mlx5-inspired flow steering tree (`fdice_fs_node`) that
maps down to E810's flat recipe-based hardware through a command vtable.

**Motivation** (Q4): ice manages flow classification through flat recipe arrays
and filter lists with no hierarchical organization. Filters are tracked in
hlists with linear-scan lookup. mlx5's `fs_node` tree with refcounted nodes,
per-node locking, and virtual destructors provides clean lifetime management
and logical namespace isolation. The command vtable (`mlx5_flow_cmds`) pattern
enables testability even though E810 has only one steering backend.

**fdice_ flow steering hierarchy**:

```
fdice_flow_steering
  +-- fdice_flow_namespace (NIC RX)
  |     +-- fdice_flow_priority (TC offload > VLAN > L2 > RSS/TTC)
  |           +-- fdice_flow_table
  |                 +-- fdice_flow_group (match criteria -> recipe mapping)
  |                       +-- fdice_flow_entry (match values + action)
  +-- fdice_flow_namespace (NIC TX)
  +-- fdice_flow_namespace (eSwitch FDB)
```

**Key improvements over ice**:
- `fdice_fs_node` base type provides unified lifetime management across all
  flow steering objects (namespaces, tables, groups, entries)
- `fdice_flow_cmds` vtable maps tree operations to E810 AQ commands
- Flow groups map to E810 recipes; recipe reuse tracked automatically
- Namespace isolation prevents cross-domain rule conflicts
- FDIR wrapped in structured API hiding dummy-packet programming details
- rhashtable for O(1) filter cookie lookup (ice uses hlist linear scan)

### 3.4 Loose RDMA Coupling with Improved Ops

**Decision**: Use ice/irdma's IIDC-style loose coupling with an enriched
interface and mlx5-style dynamic auxiliary device management.

**Motivation** (Q5): E810 hardware naturally separates RDMA -- it has dedicated
RDMA MMIO registers and a CQP (Control QP) managed entirely within the RDMA
driver. The PF driver only needs to issue AQ commands for shared resources (VSI
config, QSet TEIDs, vector allocation). mlx5's tight coupling (full core_dev
pointer sharing) does not match this hardware reality and would create
unnecessary fragility.

**fdice_ RDMA interface** (`struct fdice_rdma_export`):
- PCI device, netdev, RDMA MMIO base, PF/vport IDs (from IIDC)
- Capability snapshot at probe: max QPs, CQs, MRs, CEQs, HW revision, features
  (improvement: IIDC passes less upfront data, forcing runtime queries)
- QoS snapshot for traffic class configuration
- `fdice_rdma_ops` vtable: add/del_qset, request_reset, update_vsi_filter,
  alloc/free_vector

**Key improvements over ice/irdma**:
- Richer initial data eliminates runtime IIDC queries
- Synchronous pre/post event semantics for TC changes, MTU changes, resets
  (prevents state inconsistency during transitions)
- Shared `fdice_irq_pool` with pre-partitioned RDMA vectors and dynamic
  rebalancing (avoids ice's proxied alloc/free overhead)
- mlx5-style auxiliary device rescan for graceful RDMA teardown/recreation
  during mode changes

### 3.5 Vport-Based Eswitch without ADQ Mutual Exclusion

**Decision**: Implement mlx5-style vport abstraction and two-mode eswitch, but
eliminate ice's mutual exclusion between ADQ and switchdev.

**Motivation** (Q6, Q14, Q15): ice forces a choice: either ADQ (mqprio channel
mode with per-TC VSIs) or switchdev (representor-based forwarding). This is a
significant operational limitation. mlx5's TC works alongside its eswitch. The
mutual exclusion exists in ice because both ADQ and switchdev compete for TC
flower filter infrastructure and VSI resources without coordination.

**fdice_ approach**:
- `fdice_eswitch` with vports xarray, mode lock, offloads state (mlx5 structure)
- `fdice_vport` per PF/VF/SF with devlink_port, QoS node, ACL state
- `fdice_rep_ops` pluggable per protocol type (ETH now, IB future)
- `fdice_tc_mgr` unified TC management that works in both modes:
  - Legacy mode: flower filters through PF advanced switch rules
  - Switchdev mode: flower filters through eswitch repr path
  - ADQ channels and switchdev repr VSIs coexist in the VSI table

**Key improvements over ice**:
- Vport abstraction (from mlx5) provides unified per-VF/SF context
- Unified VF state machine replacing ice's scattered state bits
- Graceful mode transitions with VFs present (quiesce, save, transition,
  restore, resume) instead of requiring zero VFs
- Per-vport flow counters using E810 per-rule statistics
- Bridge FDB offloads enhanced with aging support and flow counters
- ADQ channels integrated as nodes in the scheduling hierarchy instead of
  tearing down the devlink rate tree

### 3.6 Multi-Layer Health Monitoring

**Decision**: Implement mlx5's five-reporter health system with automated
recovery, adapted to E810 hardware capabilities.

**Motivation** (Q7, Q8): This is the single largest improvement vector over ice.
ice has four device-level reporters with no auto-recovery -- the code explicitly
states "We do not do auto recovering." mlx5 has per-port reporters with recovery
callbacks that can self-heal from fatal firmware errors, TX timeouts, and RX
stalls without full device reset. The gap is critical for production reliability.

**fdice_ health reporters**:

| Reporter | Scope | Diagnose | Dump | Recover | Source |
|----------|-------|----------|------|---------|--------|
| `fw` | Device | FW health status + AQ events | Register snapshot | No | mlx5 + ice status codes |
| `fw_fatal` | Device | Fatal error with syndrome | CR-dump equivalent | Yes (device reload) | mlx5 pattern |
| `tx` | Per-port | SQ state, CQ state, counters | Full SQ descriptor dump | Yes (per-SQ reset) | mlx5 pattern |
| `rx` | Per-port | RQ state, CQ state, ICOSQ | Full RQ descriptor dump | Yes (per-RQ reset) | mlx5 pattern |
| `port` | Per-port | Port/link health from FW | Link diagnostic data | No | ice pattern |

**Key improvements over ice**:
- Per-devlink_port reporter attachment (mlx5 pattern) for VF/SF granularity
- Automated recovery with grace periods (ECPF: 180s, PF: 60s, VF: 30s)
- Reset escalation: per-queue recovery -> channel reopen -> PF reset -> core
  reset -> global reset (only escalate when lower level fails)
- Error history ring buffer with frequency tracking and burst detection
- Recovery success/failure statistics and time-to-recovery metrics
- Health-aware reset: pre-reset diagnostic capture, post-reset validation

**Key improvements over mlx5**:
- Error correlation across subsystems (neither driver does this well)
- Structured diagnostic dump format with versioned schema for tooling

### 3.7 Async AQ Command Interface

**Decision**: Replace ice's synchronous busy-wait AQ completion model with
workqueue-based async completion supporting multiple outstanding commands.

**Motivation** (Q11): ice serializes all AQ commands behind a single mutex and
busy-waits for each completion. mlx5 supports 32 concurrent command slots with
workqueue-based completion and callback-driven async API. While E810 firmware
uses paired ring buffers (not mailbox slots like ConnectX), the software layer
above the rings can still support concurrency and async operation.

**fdice_ AQ architecture**:

```
Subsystem APIs (fdice_switch_*, fdice_phy_*, fdice_nvm_*)
         |
  fdice_aq_cmd layer
    - Typed command builders (not raw descriptor fill)
    - Token-based completion tracking
    - Per-opcode execution time statistics
    - Configurable per-opcode retry policy
    - Async callback support (fdice_aq_exec_cb)
    - Per-subsystem locking (not global mutex)
         |
  === HARDWARE BOUNDARY ===
         |
  fdice_ctlq layer
    - Ring buffer management (exact E810 protocol)
    - libie_aq_desc format (FIXED)
    - Head/tail register mechanics (FIXED)
```

**Key improvements over ice**:
- Workqueue-based completion instead of busy-wait polling
- Multiple outstanding commands (when hardware permits)
- Per-opcode statistics for performance analysis
- Subsystem-grouped APIs (fdice_switch_add_rule vs raw ice_aq_send_cmd)
- Configurable retry policy per opcode (not hardcoded EBUSY retry)

### 3.8 Direct Page Pool with NUMA Awareness

**Decision**: Use page_pool directly with NUMA-aware creation and mlx5-style
page fragmentation, bypassing the libeth abstraction layer.

**Motivation** (Q9): ice delegates RX buffer management to libeth, which adds
indirection without benefit for a new driver that can design its own buffer
management. mlx5 directly manages page pool interactions with custom
fragmentation bias (PAGE_SIZE/64) and explicit NUMA node pinning. ice/libeth
uses NUMA_NO_NODE. For a driver targeting RDMA-class performance, NUMA locality
matters.

**fdice_ memory design**:
- `fdice_page_pool` wrapper with `PP_FLAG_DMA_MAP | PP_FLAG_DMA_SYNC_DEV`
- Explicit NUMA node from channel CPU assignment (`pp_params.nid = node`)
- `page_pool_fragment_netmem()` with configurable bias for page sharing
- Unified `fdice_rx_buf` descriptor: netmem_ref + dma_addr + offset + truesize
- Separate `hd_page_pool` for header-data split (E810 DTYPE support)
- `fdice_sq_dma` typed DMA FIFO for TX (mlx5 pattern, cleaner than ice's
  inline DEFINE_DMA_UNMAP macros)
- `dmam_alloc_coherent()` for control queue buffers (ice pattern, safety net)
- `fdice_frag_buf_alloc_node()` for data-path WQ/CQ with NUMA pinning (mlx5)

### 3.9 Hybrid DCB with SW Buffer Control

**Decision**: Combine mlx5's register-based DCB interface with ice's
configuration triple and add software-owned port buffer management.

**Motivation** (Q10): mlx5 provides explicit SW-controlled port buffer sizing
(`getbuffer`/`setbuffer` dcbnl ops) for PFC headroom calculation based on cable
length, MTU, and link speed. ice leaves buffer management entirely to firmware
with no dcbnl interface. For RDMA lossless guarantees (RoCEv2, iWARP), driver
control over PFC buffer allocation is critical -- firmware may not allocate
optimal headroom for all cable lengths and MTU configurations.

**fdice_ DCB approach**:
- Individual E810 AQ commands per parameter (not LLDP MIB TLV construction)
- Local/desired/remote configuration triple for clean state separation (ice)
- `fdice_port_buffer_config()` with SW-owned buffer mode for PFC headroom
- Cable-length-aware Xoff threshold calculation (from mlx5)
- Trust state model for DSCP (not port-wide QoS mode switch like ice)
- BEFORE/AFTER TC change notifications for RDMA (proven ice/irdma pattern)
- Per-TC max rate limiting (mlx5 feature absent from ice)
- Link toggle on PFC changes (mlx5 approach, avoiding ice's full dev reset)
- Bond/LAG awareness blocking DCB changes when bonded (ice pattern)

---

## 4. Improvement Vectors

Ranked by impact, these are the areas where fdice_ adds the most value over
both reference drivers:

### Tier 1: Critical Improvements

1. **Automated health recovery** (over ice) -- Per-queue TX/RX recovery and
   fatal error auto-recovery eliminate unnecessary full device resets. This is
   the single highest-impact improvement for production reliability. ice
   explicitly does not auto-recover; mlx5 proves the pattern works. (Q8)

2. **Init/teardown correctness** (over ice) -- Table-driven subsystem
   registration, idempotent cleanup, state-bit gating, and teardown escalation
   fix multiple classes of bugs: ordering anomalies in ice_remove, flag-gated
   cleanup fragility, reset-during-remove races, and AQ waiter cancellation
   after queue destruction. (Q1, Q3)

3. **Devlink reload from day one** (over ice) -- The init_once/load separation
   enables `devlink dev reload` without full PCI remove/probe. ice added this
   separation later and some resources straddle the boundary awkwardly. fdice_
   designs for it from the start. (Q1, Q7)

### Tier 2: High-Impact Improvements

4. **Tree-based flow steering** (over ice) -- Refcounted fs_node tree with
   namespace isolation, command vtable, and automatic recipe reuse replaces
   ice's flat recipe arrays and hlist filter tracking. Enables O(1) filter
   lookup and prevents cross-domain rule conflicts. (Q4)

5. **ADQ/switchdev coexistence** (over ice) -- Unified TC manager eliminates
   the mutual exclusion that forces operators to choose between application
   queue dedication and software-defined forwarding. (Q14, Q15)

6. **Async AQ command interface** (over ice) -- Workqueue-based completion with
   multiple outstanding commands and per-opcode statistics replaces ice's
   single-mutex busy-wait model. Better CPU utilization under command load. (Q11)

7. **Per-port health reporters** (over ice) -- Attaching health reporters to
   devlink_ports rather than the device enables per-VF/SF granular health
   monitoring in virtualized environments. (Q7, Q8)

8. **SW-controlled PFC buffer management** (over ice) -- Driver control over
   port buffer sizing with cable-length-aware headroom calculation is critical
   for RDMA lossless guarantees that ice delegates entirely to firmware. (Q10)

### Tier 3: Significant Improvements

9. **NUMA-aware page pools** (over ice) -- Explicit NUMA node pinning for
   page pool creation and fragmented WQ/CQ allocation, where ice uses
   NUMA_NO_NODE. Measurable for RDMA workloads on multi-socket systems. (Q9)

10. **Enriched RDMA interface** (over ice) -- Capability snapshot at probe time,
    synchronous event model, and shared IRQ pool with dynamic rebalancing
    improve on IIDC's minimal data passing and proxied vector allocation. (Q5)

11. **Vport abstraction for VF management** (over ice) -- Unified per-VF/SF
    context with proper state machine replacing scattered state bits and
    flag-gated conditional cleanup. (Q6)

12. **Channel-based queue grouping** (over ice) -- Embedded CQ with channel
    struct gives better cache locality than ice's linked-list ring containers
    with pointer chasing in the NAPI hot path. (Q2)

13. **DDP loading robustness** (over ice) -- Split monolithic ice_init_pkg into
    testable stages with additional bounds checks and unified error type. (Q12)

14. **PTP lock hierarchy documentation** (over ice) -- Explicit lock ordering
    between PTP hardware lock, port ps_lock, and TX spinlock prevents the
    deadlock risks present in ice's undocumented ordering. (Q13)

---

## 5. Non-Goals

These mlx5 patterns are explicitly NOT adopted in fdice_, with rationale:

### 5.1 Multiple Flow Steering Backends (DMFS/SMFS/HMFS)

mlx5 supports three steering modes: firmware-managed (DMFS), software direct
rule insertion (SMFS), and hardware-accelerated steering (HMFS). E810 has only
firmware-managed steering via the Admin Queue. There is no software steering
engine and no hardware steering accelerator. The `fdice_flow_cmds` vtable has
a single implementation. We keep the vtable for testability (mock backend) but
do not build multi-backend infrastructure. (Q4)

### 5.2 Hardware Flow Table Chaining

mlx5 hardware natively supports chained flow tables where a miss in one table
forwards to another. E810 recipes are independent with priority-only ordering
and no table-to-table forwarding. fdice_ implements priority-based pipeline
ordering in software but does not attempt to emulate hardware table chaining.
(Q4)

### 5.3 Tight RDMA Coupling (Shared core_dev)

mlx5_ib gets a direct pointer to the full `mlx5_core_dev`, accessing the command
interface, capability cache, EQ tables, and flow steering without abstraction.
This creates implicit ABI coupling between modules. E810's hardware architecture
(separate RDMA MMIO space, self-contained CQP) makes loose coupling natural and
correct. Forcing tight coupling would add complexity without hardware benefit.
(Q5)

### 5.4 SubFunction (SF) Infrastructure at Init Time

mlx5 initializes full SF support during `mlx5_init_once()` with sf_hw_table,
sf_table, and sf_dev_table. E810 does not have hardware SubFunction support
comparable to ConnectX's Scalable Functions. While ice has added limited SF
support through devlink ports, it is not a core hardware feature. fdice_ designs
the vport abstraction to accommodate SFs but does not build SF-specific
infrastructure during init. (Q1, Q6)

### 5.5 Firmware Page Allocation Subsystem

mlx5 has `mlx5_pagealloc_init/start` and `mlx5_satisfy_startup_pages` because
ConnectX firmware demands pages from the driver for internal use. E810 firmware
manages its own memory. There is no page demand mechanism in the AQ protocol.
This entire subsystem is inapplicable. (Q1)

### 5.6 Hardware Connection Tracking in Eswitch

mlx5's switchdev mode includes full hardware CT offload with zone support, NAT,
and flow aging. E810 has no hardware connection tracking engine. Implementing
software CT emulation in the driver would duplicate the kernel's conntrack
subsystem. fdice_ relies on the host networking stack for connection tracking.
(Q6, Q15)

### 5.7 Hardware Tunnel Encap/Decap in Eswitch

mlx5 offloads VXLAN, GRE, and Geneve encapsulation/decapsulation in the eswitch
FDB pipeline. E810 does not have tunnel encap/decap in its switch pipeline.
Tunnel endpoints are handled by the host stack. fdice_ does not attempt to
emulate this in software within the driver. (Q15)

### 5.8 Per-VF IPsec Offload in Eswitch

mlx5 supports per-VF IPsec crypto and packet offload through its eswitch. E810
has inline IPsec capabilities but not through the eswitch path. fdice_ may
support inline IPsec on the PF in the future but does not build eswitch IPsec
infrastructure. (Q6)

### 5.9 mlx5-Style Vendor TC Group Model for ETS

mlx5 maps IEEE 802.1Qaz ETS onto a vendor-specific TC group model (groups 0-7)
because ConnectX firmware uses groups internally. E810 firmware accepts standard
IEEE TC parameters through LLDP MIBs. fdice_ uses the standard IEEE TC table
model directly, caching TSA state in the driver where firmware does not store it.
(Q10)

### 5.10 CR-Dump / Resource Dump Infrastructure

mlx5 has `mlx5_crdump` and `mlx5_rsc_dump` for collecting full hardware state
dumps on fatal errors. These depend on ConnectX-specific firmware dump
mechanisms. E810 does not expose equivalent dump infrastructure. fdice_ collects
available diagnostic data (AQ error info, register snapshots, error history) but
does not build a full crash dump subsystem. (Q8)
