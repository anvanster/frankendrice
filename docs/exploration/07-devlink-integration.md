<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 07: Devlink Integration — mlx5 vs ice

## Overview

Devlink is the kernel's unified device management interface, providing
userspace tools (`devlink` CLI) with control over firmware, diagnostics,
configuration parameters, port management, and health monitoring. Both mlx5
and ice implement devlink extensively, but mlx5's integration is significantly
deeper and broader.

## mlx5 Devlink Features

### Devlink Ops (`mlx5_devlink_ops`)

mlx5 implements a comprehensive devlink_ops structure covering:

| Category | Operations |
|----------|-----------|
| **Flash Update** | `flash_update` — delegates to `mlx5_firmware_flash()` via mlxfw library |
| **Info** | `info_get` — reports FW version, board ID, serial numbers via VPD |
| **Reload** | `reload_down`, `reload_up` — supports `DRIVER_REINIT` and `FW_ACTIVATE` actions, plus `NO_RESET` limit for live patching |
| **eSwitch** | `eswitch_mode_set/get`, `eswitch_inline_mode_set/get`, `eswitch_encap_mode_set/get` |
| **Rate Nodes** | `rate_leaf_tx_share/max_set`, `rate_node_tx_share/max_set`, `rate_leaf/node_tc_bw_set`, `rate_node_new/del`, `rate_leaf/node_parent_set` |
| **Traps** | `trap_init`, `trap_fini`, `trap_action_set` |
| **Port (SF)** | `port_new` — SubFunction port creation via `mlx5_devlink_sf_port_new` |

### Health Reporters

mlx5 implements **6+ distinct health reporters** across multiple subsystems:

**Core FW reporters** (`health.c`):
- **`fw`** — FW syndrome and miss counter reporting, with `diagnose` and `dump` ops
  (PF variant includes dump, VF/SF variant is diagnose-only)
- **`fw_fatal`** — Fatal error reporting with automatic `recover` capability,
  crash dump (`crdump_data`) collection, and configurable graceful periods
  (ECPF: 180s, PF: 60s, VF: 30s)

**Ethernet data-path reporters** (`en/reporter_tx.c`, `en/reporter_rx.c`):
- **`tx`** — Per-port TX health: timeout detection, diagnose (SQ state, CQ state,
  HW/SW counters), dump (full SQ descriptor dump), recover (SQ reset)
- **`rx`** — Per-port RX health: timeout detection, diagnose (RQ state, CQ state,
  ICOSQ state), dump (full RQ descriptor dump), recover (RQ/channel reset)

**VNIC diagnostic reporter** (`diag/reporter_vnic.c`, `en_rep.c`):
- **`vnic`** — VNIC counter diagnostics for representor ports

All reporters are created per devlink_port (not per devlink instance), enabling
per-VF/SF granular health monitoring.

### Devlink Parameters

mlx5 registers an extensive set of parameters organized in groups:

**Core params** (`devlink.c`):
- `enable_roce` (generic) — Enable/disable RoCE
- `fdb_large_groups` (driver-specific) — eSwitch FDB large group count
- `io_eq_size`, `event_eq_size` (generic) — Event queue depths
- `max_macs` (generic) — Maximum unicast MAC addresses

**Auxiliary device params**:
- `enable_eth` — Enable/disable Ethernet auxiliary device
- `enable_rdma` — Enable/disable RDMA auxiliary device
- `enable_vnet` — Enable/disable vDPA auxiliary device

**Hardware-specific params**:
- `hairpin_num_queues`, `hairpin_queue_size` — Hairpin queue configuration
- `pcie_cong_in_low/high`, `pcie_cong_out_low/high` — PCIe congestion thresholds
- `cqe_compression_type`, `swp_l4_csum_mode` — Offload configuration

**NV params** (`lib/nv_param.c`):
- `flow_steering_mode` — DMFS vs SMFS selection
- `esw_port_metadata`, `esw_multiport` — eSwitch tuning
- `enable_sriov` — SR-IOV enable/disable via NV config

**Total**: ~20+ devlink parameters with driver-init and runtime modes.

### Devlink Traps

mlx5 registers trap groups and individual traps:
- **L2_DROPS** group containing `INGRESS_VLAN_FILTER` and `DMAC_FILTER` traps
- Full trap lifecycle: init, fini, action_set (DROP or TRAP)
- Trap reporting via `devlink_trap_report()` with port attribution
- Switchdev mode awareness (traps disabled in switchdev)

### Devlink Resources

mlx5 defines resource IDs for SubFunction limits:
- `MLX5_DL_RES_MAX_LOCAL_SFS` — Maximum local SubFunctions
- `MLX5_DL_RES_MAX_EXTERNAL_SFS` — Maximum external SubFunctions

### Reload Capabilities

mlx5 supports sophisticated reload:
- **DRIVER_REINIT**: Full `unload_one` / `load_one` cycle, lightweight mode aware
- **FW_ACTIVATE**: Firmware activation with reset sync, PCI health wait
- **FW live patching**: `NO_RESET` limit triggers `mlx5_fw_reset_set_live_patch()`
- Multi-port slave awareness (reload blocked for slaves)
- VF presence warning during reload

### SubFunction Port Management (`sf/devlink.c`)

- `mlx5_devlink_sf_port_new` / `mlx5_devlink_sf_port_del`
- Full SF lifecycle management through devlink port API
- Nested devlink instances for SFs

---

## ice Devlink Features

### Devlink Ops (`ice_devlink_ops`)

| Category | Operations |
|----------|-----------|
| **Flash Update** | `flash_update` — extensive NVM update via AQ with progress notification, overwrite mask support |
| **Info** | `info_get` — comprehensive version reporting (FW, NVM, OROM, netlist, DDP, serial) |
| **Reload** | `reload_down`, `reload_up` — supports `DRIVER_REINIT` and `FW_ACTIVATE` (EMP reset) |
| **eSwitch** | `eswitch_mode_get/set` |
| **Rate Nodes** | `rate_node_new/del`, `rate_leaf_tx_max/share/priority/weight_set`, `rate_node_tx_max/share/priority/weight_set`, `rate_leaf/node_parent_set` |
| **Port** | `port_new` — SF port creation via `ice_devlink_port_new` |

### Health Reporters

ice implements **4 health reporters** (`devlink/health.c`):

- **`mdd`** — Malicious Driver Detection events, dump only (no auto-recover)
- **`tx_hang`** — TX hang detection with full ring dump (descriptors, SKB data)
- **`fw`** — FW health status codes from AQ with diagnose and dump; includes
  extensive lookup table mapping ~30 health status codes to human-readable
  descriptions and solutions
- **`port`** — Port-level health with diagnose and dump

All reporters are device-level (not per-port), and none support auto-recovery
(`devlink_health_report` return value is ignored).

### Devlink Parameters

ice registers fewer parameters:

**RDMA params**:
- `enable_roce` (runtime get/set) — Toggle RoCE with aux device plug/unplug
- `enable_iw` (runtime get/set) — Toggle iWARP (mutually exclusive with RoCE)
- `enable_rdma` (driverinit) — Master RDMA enable

**Resource params**:
- `msix_vec_per_pf_max`, `msix_vec_per_pf_min` (driverinit) — MSI-X allocation
- `tx_scheduling_layers` (driverinit, conditional) — TX scheduler topology

**Local forwarding**:
- `local_fwd` — Local forwarding mode (enabled/disabled/prioritized)

**Total**: ~7 devlink parameters.

### Devlink Regions

ice provides **3 devlink regions** for flash/capability inspection:

- **`nvm-flash`** — Full NVM flash snapshot and direct read
- **`shadow-ram`** — Shadow RAM snapshot and direct read
- **`device-caps`** — Device capabilities snapshot from AQ

These regions enable `devlink region dump` and `devlink region read` for
debugging NVM contents without specialized tools.

### Reload Capabilities

ice supports:
- **DRIVER_REINIT**: Full teardown (unload, VSI decfg, deinit_pf, deinit_hw)
  with restrictions: blocked in switchdev mode, with ADQ active, or with VFs
- **FW_ACTIVATE**: EMP reset to activate pending NVM updates, with 60s timeout
- **OVERWRITE_MASK**: Flash update supports section-specific overwrite control

### Port Operations (`devlink/port.c`)

- Port split/unsplit for multi-port options
- Port option display and selection
- SF port creation (`ice_devlink_port_new`, `ice_devlink_create_sf_port`)
- SF device port management

---

## Gap Analysis

| Feature | mlx5 | ice | Gap |
|---------|------|-----|-----|
| **Health reporters** | 6+ (fw, fw_fatal, tx, rx, vnic; per-port) | 4 (mdd, tx_hang, fw, port; device-level) | mlx5 has per-port granularity, auto-recovery, crash dump |
| **Auto-recovery** | fw_fatal has recover callback | None (all report-only) | Significant — mlx5 can self-heal |
| **Crash dump** | crdump collection in fw_fatal dump | None | mlx5 collects full CR space dump |
| **Params count** | ~20+ (init + runtime + NV) | ~7 | mlx5 has 3x more tunable parameters |
| **Param scope** | driverinit + NV config | driverinit + runtime | mlx5 persists to NV flash |
| **Traps** | L2_DROPS group with VLAN/DMAC filters | None | ice has no devlink trap support |
| **Resources** | SF resource limits | None | ice has no devlink resource tracking |
| **Regions** | None visible in core | 3 (nvm-flash, shadow-ram, device-caps) | ice has better NVM inspection |
| **Rate nodes** | tx_share, tx_max, tc_bw | tx_share, tx_max, priority, weight | ice adds priority/weight |
| **Reload actions** | REINIT + FW_ACTIVATE + live patch | REINIT + FW_ACTIVATE (EMP reset) | mlx5 has live patching |
| **eSwitch ops** | mode + inline_mode + encap_mode | mode only | mlx5 has richer eSwitch control |
| **SF/Port mgmt** | Full SF lifecycle | SF + port split/options | Both good; ice has port options |
| **Info reporting** | FW ver, board ID, serial via VPD | FW, NVM, OROM, netlist, DDP, serial | ice has more version granularity |

### Key Architectural Differences

1. **Reporter granularity**: mlx5 attaches health reporters to devlink_ports
   (per-VF/SF), while ice attaches them to the device level. This gives mlx5
   much finer-grained health monitoring in virtualized environments.

2. **Recovery model**: mlx5's fw_fatal reporter implements automatic recovery
   with configurable graceful periods (30-180s depending on function type).
   ice reporters are observe-only with no recovery callbacks.

3. **Parameter persistence**: mlx5 uses NV parameter infrastructure to persist
   settings across reboots (flow_steering_mode, enable_sriov, etc.). ice
   parameters are either driverinit (lost on unload) or runtime-only.

4. **Trap infrastructure**: mlx5 integrates with the devlink trap subsystem for
   packet-level visibility into drops. ice has no equivalent.

5. **Flash regions**: ice provides devlink regions for inspecting NVM contents,
   which mlx5 does not expose through devlink regions (mlx5 uses other
   mechanisms for NVM access).

---

## Recommendations for fdice_

### Phase 1: Core Devlink Foundation
1. **fdice_devlink_alloc/free** — Allocate devlink with `struct fdice_dev` as priv
2. **fdice_devlink_register/unregister** — Standard registration lifecycle
3. **fdice_devlink_info_get** — Report FW version, NVM version, board ID, serial
   (E810 exposes these via AQ capabilities and VPD)
4. **fdice_devlink_reload** — Support DRIVER_REINIT using mlx5's init_once/load
   separation pattern, and FW_ACTIVATE via EMP reset (ice's AQ mechanism)

### Phase 2: Health Monitoring (mlx5-inspired, E810-adapted)
5. **fdice_fw_reporter** — FW health with diagnose/dump using E810 health status
   codes (adopt ice's extensive status lookup table)
6. **fdice_fw_fatal_reporter** — Fatal error with auto-recovery callback
   (adopt mlx5's graceful period and recover pattern)
7. **fdice_tx_reporter** / **fdice_rx_reporter** — Per-port data path health
   (adopt mlx5's per-devlink_port attachment model)
8. **fdice_mdd_reporter** — MDD event reporting (E810 hardware feature)

### Phase 3: Parameters and Configuration
9. **fdice_devlink_params** — Start with essential params:
   - `enable_roce` / `enable_iw` (runtime, from ice model)
   - `enable_rdma` (driverinit)
   - `msix_vec_per_pf_max/min` (driverinit, from ice)
   - `flow_steering_mode` (driverinit, from mlx5's concept adapted to E810 recipes)
10. **fdice_devlink_flash_update** — NVM update via AQ (ice's mechanism with
    mlx5's progress notification pattern)

### Phase 4: Advanced Features
11. **fdice_devlink_regions** — Adopt ice's NVM/SRAM/devcaps regions for
    debuggability (unique to E810, very useful)
12. **fdice_devlink_traps** — Implement trap support for E810 drop reasons
    (mlx5 pattern adapted to E810 hardware capabilities)
13. **fdice_devlink_rate** — Rate limiting for eSwitch/QoS (combine mlx5's
    tc_bw with ice's priority/weight for complete coverage)

### Design Principles
- **Adopt mlx5's per-port reporter model** for health — this is strictly
  superior for virtualization scenarios
- **Adopt mlx5's auto-recovery pattern** for fw_fatal — E810 supports PF
  reset which maps to this
- **Keep ice's devlink regions** — direct NVM access is invaluable for
  debugging E810 firmware issues
- **Keep ice's extensive info_get** — E810's multi-component firmware
  (NVM + OROM + netlist + DDP) needs granular version reporting
- **Improve parameter organization** with mlx5's grouped registration
  pattern (auxdev params, rdma params, etc.) for clean init/teardown
