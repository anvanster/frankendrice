<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Architecture 06: Implementation Plan

**References**: All exploration documents (01-15), Architecture documents (03-05)

## Overview

This document defines the ordered implementation phases for FrankenDrICE,
from bare module scaffold through full netdev operations. Each phase lists
the files created, dependencies on prior phases, estimated complexity, and
test strategy. The ordering follows the dependency graph: lower-level
abstractions are built first, and each phase produces a testable artifact.

## Dependency Graph

```
Phase 3.1: Module Scaffold
  |
  v
Phase 3.2: HAL / Admin Queue
  |
  +----+----+
  |    |    |
  v    |    v
3.3    |   3.6: DDP
Queues |    |
  |    v    |
  |   3.4   |
  |  Health  |
  |    |    |
  +----+----+
       |
       v
Phase 3.5: Flow Steering
  |         |
  v         v
3.7: PTP   3.8: RDMA
  |         |
  v         v
3.9: Devlink (wiring)
  |
  v
3.10: Switchdev / eSwitch
  |
  v
3.11: Netdev Operations
  |
  v
3.12: DCB / QoS
```

## Phase 3.1: Module Scaffold

**Goal**: Minimal loadable kernel module with PCI probe/remove, devlink
allocation, and the layered init/teardown architecture.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_main.c` | Module entry, PCI probe/remove, layered init orchestration |
| `fdice_dev.h` | `struct fdice_dev` (core device), `struct fdice_pf` |
| `Makefile` | Kernel module build system |
| `Kconfig` | Kernel configuration entry |

### Key Decisions

- `MODULE_LICENSE("GPL v2")`
- PCI ID table for E810 devices (8086:1592, 8086:1593, etc.)
- Devlink-first allocation: `devlink_alloc()` embeds `fdice_dev`
- Layered init skeleton from Exploration 01:
  - `fdice_mdev_init()` -> `fdice_pci_init()` -> `fdice_init_one()`
  - `fdice_init_one()` calls: `fdice_function_setup()` -> `fdice_init_once()`
    -> `fdice_load()` -> `fdice_register_device()`
  - Matching teardown: `fdice_unload()`, `fdice_cleanup_once()`,
    `fdice_function_teardown()`, `fdice_mdev_uninit()`
- State bit: `FDICE_INTERFACE_STATE_UP` gates teardown
- kdump FLR handling (from ice)

### Dependencies

None (first phase).

### Estimated Complexity

**Medium** -- ~800 LOC. The init/teardown skeleton is structurally complex
(four layers with error-path goto chains) but does not interact with hardware
beyond PCI resource mapping.

### Test Strategy

- `insmod`/`rmmod` cycle without errors
- Verify PCI BAR mapping succeeds
- Verify devlink device appears (`devlink dev show`)
- Error injection: simulate PCI enable failure, verify clean teardown
- W=1 clean compile, sparse, smatch

---

## Phase 3.2: HAL / Admin Queue

**Goal**: Functional Admin Queue command interface with async completion
support, the hardware abstraction layer, and capability discovery.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_hw.h` | `struct fdice_hw`, hardware register definitions |
| `fdice_hw.c` | Hardware init, capability queries, register helpers |
| `fdice_aq.h` | AQ descriptor format (uses `libie_aq_desc`), command API |
| `fdice_aq.c` | Ring management, command submission, completion, retry |
| `fdice_aq_cmd.h` | Typed command builders per subsystem |
| `fdice_aq_cmd.c` | Subsystem command implementations |

### Key Decisions

- **Below AQ boundary**: Use `libie_aq_desc` format exactly. All opcode values,
  parameter struct layouts, and ring mechanics match E810 firmware spec.
- **Above AQ boundary**: mlx5-inspired command abstraction:
  - Workqueue-based completion (replace ice's busy-wait polling)
  - Token-based command tracking for multiple outstanding commands
  - Per-opcode execution statistics
  - Configurable retry policy per opcode
- Three control queue types: Admin (AQ), Mailbox (MBX), Sideband (SB)
- AQ lifecycle: `fdice_aq_init()` -> `fdice_aq_enable()` -> `fdice_aq_disable()`
  -> `fdice_aq_cleanup()` (explicit phases, not ice's monolithic `ice_aq_init`)
- Capability discovery via `list_func_caps` (0x000A) and `list_dev_caps` (0x000B)
  populates `fdice_hw.caps`
- Async event processing from ARQ: link events, health events, mailbox, LLDP

### Dependencies

Phase 3.1 (module scaffold, PCI BAR mapping).

### Estimated Complexity

**High** -- ~2500 LOC. The AQ is the hardware boundary; correctness is critical.
The completion model redesign (polling -> workqueue) is the most complex aspect.

### Test Strategy

- `get_ver` (0x0001) command succeeds, returns valid FW version
- `list_func_caps` returns valid capability data
- `send_driver_ver` (0x0002) succeeds
- AQ error injection: verify timeout handling, retry logic
- ARQ event processing: simulate link event, verify dispatch
- Per-opcode stats: verify counters increment correctly
- devlink info shows FW version (`devlink dev info`)

---

## Phase 3.3: Queue Management

**Goal**: Full queue lifecycle with mlx5-inspired channel grouping, embedded
CQ abstraction, and interrupt routing table.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_queue.h` | `fdice_txq`, `fdice_rxq`, `fdice_cq`, WQ abstractions |
| `fdice_queue.c` | Queue allocation, configuration, enable/disable via AQ |
| `fdice_channel.h` | `fdice_channel` (per-CPU queue grouping) |
| `fdice_channel.c` | Channel create/destroy, NAPI registration |
| `fdice_eq.h` | `fdice_eq`, `fdice_irq_pool` (interrupt routing) |
| `fdice_eq.c` | Interrupt vector management, EQ-like routing table |
| `fdice_cq.h` | `fdice_cq` software completion queue abstraction |
| `fdice_cq.c` | CQ polling, arming, completion processing |
| `fdice_txrx.h` | Data path structures, DMA tracking |
| `fdice_txrx.c` | NAPI poll, TX completion, RX completion |
| `fdice_page_pool.c` | Page pool creation, NUMA-aware allocation |

### Key Decisions

- `fdice_channel` groups: 1 RQ + N TXQs (per-TC) + XDP SQ + NAPI
- Embedded CQ in each TXQ/RXQ (mlx5 pattern, cache locality)
- Cache-line-aware struct layout with `__cacheline_group` annotations (ice pattern)
- `fdice_irq_pool` manages MSI-X vectors with RDMA pre-partitioning
- Queue state machine: `UNINIT -> ALLOCATED -> CREATED -> ENABLED -> ACTIVE
  -> DISABLED -> DESTROYED`
- Page pool for all RX DMA (no direct `dma_map_page` in hot path)
- Typed TX DMA FIFO (mlx5 pattern) for clean completion unmap

### Dependencies

Phase 3.2 (AQ commands for queue enable/disable: `add_txqs` 0x0C30,
`dis_txqs` 0x0C31, VSI management).

### Estimated Complexity

**Very High** -- ~4000 LOC. This is the largest phase, covering the entire
data path infrastructure. The hot path code (NAPI poll, TX/RX completion)
requires careful performance optimization.

### Test Strategy

- Queue allocation and AQ-based enable succeeds
- NAPI poll loop processes synthetic completions
- TX descriptor ring wraps correctly
- RX buffer allocation via page pool succeeds
- Interrupt vector allocation and routing works
- XDP redirect path compiles and links
- DIM (Dynamic Interrupt Moderation) integration
- Performance: packets/sec baseline with iperf3

---

## Phase 3.4: Health Monitoring

**Goal**: Full 6-layer health system with five devlink reporters and
automated recovery (Architecture 05).

### Files Created

| File | Purpose |
|------|---------|
| `fdice_health_core.c` | Timer, workqueue, sensor checks, history tracking |
| `fdice_health_core.h` | `fdice_health`, syndrome table, history structs |
| `fdice_health_fw.c` | fw + fw_fatal reporters, AQ health event processing |
| `fdice_health_tx.c` | TX reporter with per-queue SQ recovery |
| `fdice_health_rx.c` | RX reporter with per-queue RQ recovery |
| `fdice_health_port.c` | Port reporter, link health events |

### Key Decisions

- Health polling timer initialized in `fdice_mdev_init()`, started after
  AQ is functional (mlx5 pattern: health is a first-class subsystem)
- fw_fatal reporter with `recover` callback -> `fdice_recover_device()`
- Per-queue TX/RX recovery: disable -> reset -> re-enable via AQ
- Reset escalation: per-queue -> channel -> PF -> core -> global
- Error history ring buffer with configurable escalation thresholds
- Grace period: 60s PF, 30s VF (configurable via devlink)

### Dependencies

Phase 3.2 (AQ for health status queries), Phase 3.3 (queue structures for
per-queue reporters and recovery).

### Estimated Complexity

**High** -- ~2000 LOC. The recovery state machines and escalation logic
are the complex parts. The reporter registration is straightforward.

### Test Strategy

- `devlink health show` lists all five reporters
- Simulate FW heartbeat loss -> fw reporter triggers
- Simulate TX timeout -> tx reporter triggers, per-queue recovery succeeds
- Simulate fatal error -> fw_fatal reporter triggers, auto-recovery succeeds
- Error history tracks events correctly
- Escalation: inject repeated queue failures, verify escalation to PF reset
- Grace period: verify timer-based auto-recovery after grace period

---

## Phase 3.5: Flow Steering

**Goal**: Tree-based flow steering with recipe management, FDIR integration,
and TC flower offload (Architecture 04).

### Files Created

| File | Purpose |
|------|---------|
| `fdice_fs_core.c` | Tree infrastructure, namespace init, node lifecycle |
| `fdice_fs_core.h` | `fdice_fs_node`, namespace, priority, table, group, entry |
| `fdice_fs_cmd.c` | AQ command vtable implementation |
| `fdice_fs_cmd.h` | `fdice_flow_cmds` vtable definition |
| `fdice_fs_recipe.c` | Recipe manager with sharing and allocation |
| `fdice_fs_fdir.c` | FDIR wrapper (structured match -> dummy packet) |
| `fdice_fs_tc.c` | TC flower offload integration |
| `fdice_fs_ttc.c` | Type-to-queue classification (RSS) |

### Key Decisions

- `fdice_fs_node` base type for all objects (refcount, per-node lock, destructors)
- Three root namespaces: NIC RX, NIC TX, FDB
- Recipe manager with hash-based sharing (max 64 E810 recipes)
- Software priority pipeline (E810 lacks HW table chaining)
- FDIR behind structured match API (hides dummy-packet details)
- Unified TC flower path for PF, ADQ, and representor filters

### Dependencies

Phase 3.2 (AQ commands: `add_recipe` 0x0290, `add_sw_rules` 0x02A0,
FDIR programming), Phase 3.3 (queue targets for flow actions).

### Estimated Complexity

**Very High** -- ~3500 LOC. The tree infrastructure, recipe management, and
FDIR translation are all substantial. TC flower parsing adds significant code.

### Test Strategy

- Create namespace -> priority -> table -> group -> entry hierarchy
- Verify recipe sharing: two groups with same mask share one recipe
- TC flower add/delete on PF interface
- FDIR rule add via ethtool, verify queue steering
- `devlink` recipe usage statistics
- Verify refcount cleanup: remove table, all children freed
- Concurrent rule insertion stress test

---

## Phase 3.6: DDP Loading

**Goal**: Dynamic Device Personalization package loading with signing
verification, multi-PF awareness, and safe mode fallback.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_ddp.h` | Package format structs, `fdice_ddp_state` enum |
| `fdice_ddp.c` | Verify, init, download, compatibility checking |
| `fdice_flex_pipe.c` | Pipeline table population (XLT, TCAM, field vectors) |

### Key Decisions

- Package format parsing matches E810 firmware spec exactly (hardware boundary)
- Signing verification for RSA2K/RSA3K packages
- Multi-PF: global config lock during download, already-loaded detection
- Safe mode fallback: `FDICE_FLAG_ADV_FEATURES` cleared on DDP failure
- Staged initialization: verify -> init_info -> match_signing -> check_compat
  -> download -> fill_tables (cleaner than ice's monolithic `ice_init_pkg`)
- Structured error reporting (unified result type, not mixed enum/errno)

### Dependencies

Phase 3.2 (AQ commands: `download_pkg` 0x0C40, `get_pkg_info_list` 0x0C43,
global config lock).

### Estimated Complexity

**High** -- ~2000 LOC. The package format is complex with nested segments,
sections, and buffers. Signing verification adds security-critical code.

### Test Strategy

- Load official Intel DDP package, verify `ice_ddp_state` SUCCESS
- Load with wrong device ID, verify COMPAT error
- Load with corrupt package, verify INVALID_FILE error
- Multi-PF: second PF detects ALREADY_LOADED
- Safe mode: verify basic L2 functionality without DDP
- `devlink dev info` shows DDP package name and version

---

## Phase 3.7: PTP

**Goal**: PTP Hardware Clock with TX/RX timestamping, GPIO support, and
cross-timestamping.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_ptp.h` | PTP state machine, `fdice_ptp`, `fdice_ptp_port` |
| `fdice_ptp.c` | PHC registration, TX/RX timestamp processing, GPIO |
| `fdice_ptp_hw.h` | Hardware abstraction types |
| `fdice_ptp_hw.c` | PHY register access, timer ops, Vernier calibration |

### Key Decisions

- State machine: `UNINIT -> INITIALIZING -> READY -> RESETTING -> READY`
- Source timer ownership (only one PF controls the master timer)
- Hardware semaphore (`PFTSYN_SEM`) for multi-PF atomic timer operations
- Cached PHC time for RX timestamp extension (periodic kthread worker)
- TX timestamp bitmap tracking with 2-second configurable timeout
- Cross-timestamping via `get_device_system_crosststamp()`
- Separate HW abstraction for future E830 support

### Dependencies

Phase 3.2 (AQ for CGU/SyncE commands), Phase 3.3 (TX descriptor timestamp
index, RX descriptor flex field extraction).

### Estimated Complexity

**High** -- ~2500 LOC. E810 PTP hardware is sophisticated with per-port
timestamp blocks, Vernier calibration, and multi-PF coordination.

### Test Strategy

- `ptp4l` synchronization to grandmaster clock
- TX timestamp delivery via `SO_TIMESTAMPING`
- RX timestamp accuracy within E810 spec
- `phc2sys` cross-timestamping
- GPIO PPS output verification with oscilloscope/counter
- Reset recovery: PTP state preserved across PF reset
- `ethtool -T` shows correct timestamping capabilities

---

## Phase 3.8: RDMA Integration

**Goal**: Auxiliary bus RDMA device with ops vtable, capability snapshot, and
synchronous event notifications (Architecture 03).

### Files Created

| File | Purpose |
|------|---------|
| `fdice_rdma.c` | PF-side RDMA subsystem init/cleanup |
| `fdice_rdma.h` | `fdice_rdma_dev` internal declarations |
| `fdice_iidc.c` | Auxiliary device lifecycle, event dispatch |
| `fdice_iidc.h` | `fdice_iidc_core`, `fdice_iidc_ops`, `fdice_rdma_caps` |

### Key Decisions

- Loose coupling via ops vtable (no exported symbols)
- Rich capability snapshot at probe time (`fdice_rdma_caps`)
- Synchronous pre/post event semantics for TC changes and resets
- Interrupt pool pre-partitioning (1/3 of MSI-X vectors for RDMA)
- Dynamic auxiliary device lifecycle (unplug/replug during reconfiguration)
- `is_supported()` + `is_enabled()` gating callbacks

### Dependencies

Phase 3.2 (AQ: `add_rdma_qset` 0x0C33), Phase 3.3 (interrupt pool with
RDMA partition), Phase 3.4 (health events for RDMA pre-reset notification).

### Estimated Complexity

**Medium** -- ~1200 LOC. The interface is well-defined; most complexity is
in the event synchronization protocol and interrupt partitioning.

### Test Strategy

- Auxiliary device appears when RDMA-capable E810 is present
- `devlink param set enable_roce false` -> auxiliary device removed
- `devlink param set enable_roce true` -> auxiliary device re-created
- Capability snapshot contains valid resource limits
- Event handler registration/unregistration lifecycle
- Interrupt vector allocation from RDMA partition succeeds
- TC change: verify pre/post event sequence delivered to mock handler

---

## Phase 3.9: Devlink

**Goal**: Wire all subsystems into devlink: parameters, regions, health
reporter registration, flash update, reload, and info reporting.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_devlink.c` | Devlink ops, params, reload, info, flash, rate nodes |
| `fdice_devlink.h` | Devlink declarations |
| `fdice_devlink_region.c` | NVM, Shadow RAM, device-caps regions |

### Key Decisions

- Devlink params: `enable_roce`, `enable_iw`, `enable_rdma` (runtime/driverinit),
  `msix_vec_per_pf_max/min` (driverinit), `flow_steering_mode` (driverinit)
- Devlink reload: `DRIVER_REINIT` (fdice_unload/fdice_load cycle),
  `FW_ACTIVATE` (EMP reset via AQ)
- Devlink regions: nvm-flash, shadow-ram, device-caps (ice pattern, invaluable
  for E810 debugging)
- Devlink info: FW version, NVM version, OROM version, netlist version, DDP
  package name/version, board ID, serial number
- Rate nodes for eSwitch QoS (integrated with scheduling tree)
- Health reporters registered per-phase (fw/fw_fatal from 3.4, tx/rx from 3.3,
  port from 3.4); this phase wires them to devlink ports

### Dependencies

Phase 3.4 (health reporters), Phase 3.5 (flow steering for steering mode param),
Phase 3.6 (DDP info), Phase 3.8 (RDMA enable params).

### Estimated Complexity

**Medium** -- ~1500 LOC. Primarily wiring existing subsystems to devlink APIs.
Flash update involves AQ NVM commands but follows ice's proven pattern.

### Test Strategy

- `devlink dev info` shows all version components
- `devlink param show/set` for all registered parameters
- `devlink health show` lists all reporters with correct state
- `devlink region dump` for nvm-flash, shadow-ram, device-caps
- `devlink dev reload` -> driver reinit succeeds, state preserved
- Flash update via `devlink dev flash` (requires NVM update file)

---

## Phase 3.10: Switchdev / eSwitch

**Goal**: Two-mode eSwitch (legacy + switchdev) with port representors,
bridge FDB offload, and vport abstraction.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_eswitch.c` | Mode management, vport abstraction |
| `fdice_eswitch.h` | `fdice_eswitch`, `fdice_vport` |
| `fdice_eswitch_br.c` | Bridge FDB offload, VLAN handling |
| `fdice_repr.c` | Port representor lifecycle, netdev_ops |
| `fdice_repr.h` | `fdice_repr`, `fdice_rep_ops` |
| `fdice_repr_tc.c` | TC flower on representors |

### Key Decisions

- mlx5-inspired `fdice_eswitch` with vport xarray (richer than ice's minimal struct)
- Pluggable `fdice_rep_ops` (ETH type now, IB type future-ready)
- VF state machine: `DISABLED -> INITIALIZING -> ACTIVE -> QS_ENABLED -> RESETTING`
- Bridge offload: rhashtable FDB + forward/guard rule pairs (ice pattern)
- Lazy switchdev init (ice pattern: setup on first VF attach)
- Metadata_dst for TX steering (proven pattern, with abstraction for future HW)
- ADQ/switchdev coexistence design (improvement over ice's mutual exclusion)
- Virtchnl mailbox protocol for VF communication (E810 hardware boundary)

### Dependencies

Phase 3.5 (flow steering FDB namespace for bridge rules and TC flower),
Phase 3.3 (VSI management for VF VSIs).

### Estimated Complexity

**Very High** -- ~3000 LOC. The eSwitch subsystem spans VF management,
representor networking, bridge offloads, and TC flower integration.

### Test Strategy

- `devlink eswitch mode set switchdev` succeeds
- VF creation -> representor netdev appears
- Bridge FDB learning offload: `bridge fdb show` shows offloaded entries
- TC flower on representor: add rule, verify hardware programming
- Mode switch: switchdev -> legacy transition
- VF reset recovery: representor survives VF reset
- Virtchnl message handling for basic VF operations

---

## Phase 3.11: Netdev Operations

**Goal**: Full network device operations including ethtool, data path TX/RX,
and feature negotiation.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_ethtool.c` | Ethtool ops (link, stats, coalesce, channels, rings) |
| `fdice_netdev.c` | ndo_* operations (open, stop, xmit, set_rx_mode, etc.) |
| `fdice_stats.c` | Statistics collection and reporting |

### Key Decisions

- `ndo_start_xmit`: TX path using fdice_txq with DMA FIFO tracking
- `ndo_open`/`ndo_stop`: Channel creation/destruction, queue enable/disable
- `ndo_set_rx_mode`: MAC filter programming via AQ switch rules
- Feature flags: NETIF_F_SG, NETIF_F_IP_CSUM, NETIF_F_IPV6_CSUM,
  NETIF_F_TSO, NETIF_F_TSO6, NETIF_F_GRO, NETIF_F_RXCSUM,
  NETIF_F_HW_VLAN_CTAG_RX/TX, NETIF_F_HW_TC, NETIF_F_NTUPLE
- Ethtool: link settings, statistics (per-port and per-queue), coalescing
  (with DIM), channel count, ring size, RSS configuration, self-test
- XDP support: `ndo_bpf` and `ndo_xdp_xmit`
- AF_XDP zero-copy support via XSK

### Dependencies

Phase 3.3 (queue infrastructure), Phase 3.5 (flow steering for ntuple and
RX classification), Phase 3.6 (DDP for advanced features vs safe mode).

### Estimated Complexity

**Very High** -- ~3500 LOC. The data path must be highly optimized. Ethtool
covers a large surface area. XDP/XSK add significant complexity.

### Test Strategy

- `ip link set up`, interface comes up with link
- `iperf3` throughput matches line rate (or close)
- `ethtool -S` shows incrementing counters
- `ethtool -l/-L` channel count adjustment works
- `ethtool -g/-G` ring size adjustment works
- `ethtool -k/-K` feature toggle works
- `ethtool -N` FDIR rule programming (integrates with flow steering)
- XDP program attach and basic forwarding
- AF_XDP zero-copy with `xdpsock` benchmark
- Jumbo frames (MTU 9000)

---

## Phase 3.12: DCB / QoS

**Goal**: Data Center Bridging with ETS, PFC, DCBX, and DSCP support for
RDMA lossless operation.

### Files Created

| File | Purpose |
|------|---------|
| `fdice_dcb.c` | dcbnl_rtnl_ops, ETS, PFC, DCBX, CEE |
| `fdice_dcb.h` | DCB configuration structs, QoS mode |
| `fdice_dcb_lib.c` | Configuration application, VSI rebuild, RDMA notification |

### Key Decisions

- Hybrid approach: mlx5-inspired register-based interface below AQ, ice-inspired
  config triple (local/desired/remote) above AQ
- IEEE + CEE support (mutually exclusive modes, like ice)
- FW LLDP agent awareness (read-only when agent active)
- SW-controlled port buffer management for PFC headroom (mlx5 improvement)
- Trust state model for DSCP (mlx5 approach, more flexible than ice)
- BEFORE/AFTER TC change notifications to RDMA driver
- Per-TC max rate limiting (mlx5 feature missing in ice)
- Bond/LAG awareness: block DCB changes when bonded

### Dependencies

Phase 3.8 (RDMA TC change notifications), Phase 3.3 (VSI rebuild for TC
reconfiguration), Phase 3.11 (netdev for dcbnl registration).

### Estimated Complexity

**High** -- ~2000 LOC. The LLDP MIB interaction and VSI rebuild coordination
are complex. PFC buffer management adds significant logic.

### Test Strategy

- `lldptool` set ETS, verify TC-to-queue mapping
- `lldptool` set PFC, verify lossless behavior with traffic generator
- DCBX negotiation with partner switch
- DSCP mode: verify DSCP-to-TC mapping
- RDMA with PFC: iperf3 RDMA shows zero drops under congestion
- CEE mode compatibility with older switches
- `devlink` rate nodes reflect DCB configuration

---

## Critical Path Analysis

The **critical path** through the dependency graph determines the minimum
time to a functional driver:

```
3.1 (2w) -> 3.2 (4w) -> 3.3 (5w) -> 3.5 (4w) -> 3.11 (4w)
                                        |
Total critical path: ~19 weeks           +-> 3.10 (4w) parallel with 3.11
```

### Parallelization Opportunities

Several phases can proceed in parallel once their dependencies are met:

| Parallel Group | Phases | Prerequisite |
|----------------|--------|-------------|
| A | 3.4 (Health) + 3.6 (DDP) | After 3.2 |
| B | 3.7 (PTP) + 3.8 (RDMA) | After 3.3 + 3.5 |
| C | 3.10 (Switchdev) + 3.11 (Netdev) | After 3.5 |
| D | 3.9 (Devlink) + 3.12 (DCB) | After 3.8 + 3.11 |

### Phase Complexity Summary

| Phase | Files | Est. LOC | Weeks | Risk |
|-------|-------|----------|-------|------|
| 3.1 Module Scaffold | 4 | 800 | 2 | Low |
| 3.2 HAL/AQ | 6 | 2500 | 4 | High (HW boundary) |
| 3.3 Queue Management | 11 | 4000 | 5 | High (performance) |
| 3.4 Health Monitoring | 6 | 2000 | 3 | Medium |
| 3.5 Flow Steering | 8 | 3500 | 4 | High (recipe mgmt) |
| 3.6 DDP Loading | 3 | 2000 | 3 | High (FW compat) |
| 3.7 PTP | 4 | 2500 | 3 | High (HW timing) |
| 3.8 RDMA Integration | 4 | 1200 | 2 | Medium |
| 3.9 Devlink | 3 | 1500 | 2 | Low |
| 3.10 Switchdev/eSwitch | 6 | 3000 | 4 | High (VF mgmt) |
| 3.11 Netdev Operations | 3 | 3500 | 4 | High (data path) |
| 3.12 DCB/QoS | 3 | 2000 | 3 | Medium |
| **Total** | **61** | **~28,500** | **~24** (with parallelism) | |

### Risk Assessment

**Highest risk phases**:

1. **Phase 3.2 (HAL/AQ)**: This is the hardware boundary. Any misunderstanding
   of E810 AQ protocol will cascade to all subsequent phases. Mitigation:
   extensive comparison with ice's AQ implementation, early hardware testing.

2. **Phase 3.6 (DDP)**: Firmware compatibility is binary -- the package either
   loads or the device enters safe mode. Signing verification is security-critical.
   Mitigation: use official Intel DDP packages for testing, validate every error
   path.

3. **Phase 3.3 (Queue Management)**: Performance is non-negotiable for a network
   driver. The data path code will be profiled and optimized iteratively.
   Mitigation: establish performance baselines early, profile with perf/ftrace.

4. **Phase 3.5 (Flow Steering)**: Recipe management is a novel abstraction over
   E810's flat model. Recipe exhaustion (max 64) is a real constraint.
   Mitigation: recipe sharing from day one, monitoring of recipe usage.
