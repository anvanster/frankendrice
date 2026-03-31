<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 10: DCB/QoS Implementation — PFC, ETS, and DCBX

## Overview

Data Center Bridging (DCB) is critical for RDMA deployments, providing lossless
Ethernet through Priority Flow Control (PFC), bandwidth management through
Enhanced Transmission Selection (ETS), and automated negotiation through DCBX.
This exploration compares mlx5 and ice implementations to inform fdice_ design.

## mlx5 DCB/QoS Approach

### Architecture

mlx5 implements DCB entirely in a single file (`en_dcbnl.c`, ~1300 lines) with a
thin header (`en/dcbnl.h`). The architecture is notably **register-based and
direct**: individual firmware register commands handle each DCB parameter
independently.

**Key files:**
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/en_dcbnl.c` — All DCB netlink ops
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/en/dcbnl.h` — Structures and declarations
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/port.c` — Low-level port register access
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/en/port_buffer.c` — Buffer management for PFC

### ETS Implementation

mlx5 ETS uses a **vendor-specific TC group model** to map IEEE 802.1Qaz semantics
onto hardware:

- **TC groups 0-6**: Used for ETS bandwidth allocation. Group 0 is lowest priority.
- **TC group 7** (`MLX5E_VENDOR_TC_GROUP_NUM`): Vendor-specific strict priority.
- Firmware commands: `mlx5_set_port_prio_tc()`, `mlx5_set_port_tc_group()`,
  `mlx5_set_port_tc_bw_alloc()` — each is a separate register write.
- ETS zero-bandwidth TCs get equal share via `mlx5e_build_tc_tx_bw()`.
- TSA (Transmission Selection Algorithm) modes: `IEEE_8021QAZ_TSA_ETS`,
  `IEEE_8021QAZ_TSA_STRICT`, `IEEE_8021QAZ_TSA_VENDOR`.
- TSA state is **cached in driver** (`priv->dcbx.tc_tsa[]`) since firmware does
  not store it directly — it only understands groups and bandwidth percentages.
- Max rate limiting per TC via `mlx5_modify_port_ets_rate_limit()` with support
  for 100Mbps and 1Gbps granularity units.

**Validation**: `mlx5e_dbcnl_validate_ets()` checks priority range and bandwidth
sum (must be 100% for ETS TCs, or zero-sum when allowed for CEE).

### PFC Implementation

mlx5 PFC is straightforward register-based:

- `mlx5_set_port_pfc()` / `mlx5_query_port_pfc()` — direct firmware register access.
- PFC enable is a per-priority bitmask.
- On PFC change, `mlx5_toggle_port_link()` is called to reset the link.
- **Port buffer management** is a distinguishing feature: when `MLX5_BUFFER_SUPPORTED`,
  PFC changes trigger `mlx5e_port_manual_buffer_config()` to recalculate headroom
  based on cable length, MTU, and PFC configuration.
- Cable length is stored in `priv->dcbx.cable_len` and reported/set via the
  `pfc->delay` field (max 1000 meters).
- Buffer ownership model: `MLX5_BUF_OWNERSHIP_SW_OWNED` vs firmware-managed,
  with the driver only reconfiguring buffers when in SW ownership mode.
- `dcbnl_getbuffer` / `dcbnl_setbuffer` ops provide explicit per-priority buffer
  sizing and priority-to-buffer mapping — not present in ice.

### DCBX Mode Management

mlx5 supports multiple DCBX operational modes:

- `MLX5E_DCBX_PARAM_VER_OPER_HOST` — Host (driver) controls DCB.
- `MLX5E_DCBX_PARAM_VER_OPER_AUTO` — Firmware controls DCB.
- Mode is queried/set via `mlx5_query_port_dcbx_param()` / `mlx5_set_port_dcbx_param()`.
- When firmware mode is active (`!HOST`), the firmware `willing_admin` flag is set.
- Capabilities reported: `DCB_CAP_DCBX_VER_CEE | DCB_CAP_DCBX_VER_IEEE`,
  plus `DCB_CAP_DCBX_HOST` when in host mode.
- `DCB_CAP_DCBX_LLD_MANAGED` is explicitly rejected — mlx5 does not support
  LLDP-agent-managed mode through the dcbnl interface.

### DSCP/Trust State

mlx5 has a sophisticated DSCP-to-priority mapping system:

- Trust state toggles between `MLX5_QPTS_TRUST_PCP` (L2 VLAN priority) and
  `MLX5_QPTS_TRUST_DSCP` (L3 DSCP).
- DSCP mappings use the `IEEE_8021QAZ_APP_SEL_DSCP` selector via dcb_app framework.
- Switching trust state requires channel reconfiguration via `mlx5e_safe_switch_params()`.
- When `tx_min_inline_mode` is L2 and DSCP trust is active, it upgrades to IP inline mode.
- DSCP app count tracks active mappings; when count drops to zero, reverts to PCP trust.

### CEE Support

mlx5 supports legacy CEE DCBX through the `mlx5e_dcbnl_setall()` function that
translates CEE configuration into IEEE ETS/PFC structures and applies them
through the same IEEE code path. The `mlx5e_cee_config` structure holds pending
CEE configuration (priority group BW, prio-to-PG map, PFC settings).

### dcbnl_rtnl_ops Registration

```
IEEE: getets, setets, getmaxrate, setmaxrate, getpfc, setpfc, setapp, delapp
DCBx: getdcbx, setdcbx
Buffer: getbuffer, setbuffer
CEE: setall, getstate, getpermhwaddr, setpgtccfgtx, setpgbwgcfgtx,
     getpgtccfgtx, getpgbwgcfgtx, setpfccfg, getpfccfg, getcap,
     getnumtcs, getpfcstate, setpfcstate
```

## ice DCB/QoS Approach

### Architecture

ice DCB is spread across a **three-layer architecture**:

- `ice_dcb_nl.c` (~1150 lines) — DCBNL netlink interface (user-space facing).
- `ice_dcb_lib.c` (~1100 lines) — DCB library/configuration logic (mid-layer).
- `ice_dcb.c` (~1550 lines) — Low-level LLDP MIB parsing/building and AQ commands.

**Key files:**
- `kernel/drivers/net/ethernet/intel/ice/ice_dcb_nl.c` — dcbnl_rtnl_ops
- `kernel/drivers/net/ethernet/intel/ice/ice_dcb_lib.c` — DCB config application
- `kernel/drivers/net/ethernet/intel/ice/ice_dcb.c` — LLDP TLV parsing, AQ commands
- `kernel/drivers/net/ethernet/intel/ice/ice_type.h` — DCB data structures

### ETS Implementation

ice ETS operates through a **LLDP MIB abstraction**:

- Configuration is stored in `ice_dcbx_cfg` structures (`local_dcbx_cfg`,
  `desired_dcbx_cfg`, `remote_dcbx_cfg`) within `ice_qos_cfg`.
- `ice_dcb_ets_cfg` contains: `willing`, `cbs`, `maxtcs`, `prio_table[8]`,
  `tcbwtable[8]`, `tsatable[8]`.
- ETS recommendation (`etsrec`) is maintained separately from configuration (`etscfg`).
- Setting ETS: populate `desired_dcbx_cfg`, call `ice_pf_dcb_cfg()` which:
  1. Validates bandwidth via `ice_dcb_bwchk()`.
  2. Notifies AUX (RDMA) drivers via `IIDC_RDMA_EVENT_BEFORE_TC_CHANGE`.
  3. Disables all affected VSIs.
  4. In SW LLDP mode: builds LLDP MIB TLVs and sends via `ice_set_dcb_cfg()` ->
     `ice_aq_set_lldp_mib()`.
  5. Queries port ETS from firmware via `ice_query_port_ets()`.
  6. Reconfigures VSIs via `ice_pf_dcb_recfg()`.
  7. Re-enables VSIs.
  8. Notifies AUX drivers via `IIDC_RDMA_EVENT_AFTER_TC_CHANGE`.
- Contiguous TC requirement: `ice_dcb_tc_contig()` checks that TCs are contiguous
  starting from TC0. Non-contiguous TCs fall back to single-TC default.
- On significant changes, `ice_dcbnl_devreset()` performs a full `dev_close()` /
  `dev_open()` cycle (heavy-weight reset).

### PFC Implementation

ice PFC also goes through the LLDP MIB path:

- `ice_dcb_pfc_cfg` contains: `willing`, `mbc`, `pfccap`, `pfcena` (bitmask).
- PFC delay read directly from hardware register `PRTDCB_GENC`.
- Statistics: `priority_xoff_tx[i]` and `priority_xoff_rx[i]` from `pf->stats`.
- Setting PFC modifies `desired_dcbx_cfg.pfc` then calls `ice_pf_dcb_cfg()`.
- **Dual PFC modes**: `ICE_QOS_MODE_VLAN` (L2, default) and `ICE_QOS_MODE_DSCP`
  (L3), switchable via `ice_aq_set_pfc_mode()`.
- No explicit port buffer management interface (no `getbuffer`/`setbuffer` ops).
  Buffer allocation is firmware-managed.

### DCBX Mode Management

ice has a different DCBX model centered on the **FW LLDP agent**:

- `ICE_FLAG_FW_LLDP_AGENT` — When set, firmware runs LLDP and manages DCB.
  The dcbnl interface is read-only in this mode.
- `ICE_FLAG_DCB_CAPABLE` — Hardware DCB capability flag.
- `ICE_FLAG_DCB_ENA` — DCB currently enabled (more than 1 TC active).
- DCBX mode stored in `pf->dcbx_cap` — supports `DCB_CAP_DCBX_VER_IEEE` or
  `DCB_CAP_DCBX_VER_CEE` (but not both simultaneously).
- `DCB_CAP_DCBX_LLD_MANAGED` presence blocks all set operations.
- ice enforces `DCB_CAP_DCBX_HOST` requirement for all set operations.
- LLDP MIB change events (`ice_dcb_process_lldp_set_mib_change()`) trigger
  automatic reconfiguration when firmware LLDP agent is active.
- Bond/LAG awareness: All DCB changes are blocked when the interface is bonded.

### DSCP Support

ice DSCP is more deeply integrated into the QoS mode:

- Entire QoS mode switches between `ICE_QOS_MODE_VLAN` and `ICE_QOS_MODE_DSCP`.
- DSCP mode switch requires `ice_aq_set_pfc_mode()` AQ command.
- In DSCP mode, UP-to-TC mapping becomes fixed (cannot change via ETS prio_table).
- `dscp_mapped` bitmap tracks which DSCP values have user mappings.
- `dscp_map[DSCP_MAX]` array holds DSCP-to-TC mappings.
- When last DSCP mapping is deleted, automatically reverts to VLAN mode.
- DSCP support gated by `ICE_F_DSCP` feature flag.

### CEE Support

ice supports CEE through a complete set of `setpgtccfgtx`, `setpgbwgcfgtx`,
etc. callbacks. CEE commit goes through `ice_dcbnl_cee_set_all()` which also
calls `ice_pf_dcb_cfg()`. Notably, ice provides `setstate` for enabling/disabling
DCB at the CEE level, and `setpgtccfgrx`/`setpgbwgcfgrx` stubs (not supported
but required by lldpad).

### dcbnl_rtnl_ops Registration

```
IEEE: getets, setets, getpfc, setpfc, setapp, delapp
CEE: getstate, setstate, getpermhwaddr, setpgtccfgtx, setpgbwgcfgtx,
     setpgtccfgrx, setpgbwgcfgrx, getpgtccfgtx, getpgbwgcfgtx,
     getpgtccfgrx, getpgbwgcfgrx, setpfccfg, getpfccfg, setall,
     getcap, getnumtcs, getpfcstate, getapp
DCBx: getdcbx, setdcbx
```

**Notable absences vs mlx5**: No `getmaxrate`/`setmaxrate`, no `getbuffer`/`setbuffer`.

## Comparison

### Structural Differences

| Aspect | mlx5 | ice |
|--------|------|-----|
| **Architecture** | Single file, register-based | Three-layer, LLDP MIB-based |
| **FW Interface** | Direct register commands per parameter | LLDP MIB TLV build + AQ send |
| **Config Model** | Individual register writes | Desired/local/remote cfg triple |
| **Reset Strategy** | Minimal (link toggle for PFC) | Full dev_close/dev_open cycle |
| **LLDP Agent** | Host vs Auto mode | FW LLDP agent flag |
| **Buffer Mgmt** | Explicit SW control (getbuffer/setbuffer) | Firmware-managed, no dcbnl API |
| **Max Rate** | Per-TC rate limiting supported | Not exposed via dcbnl |
| **DSCP** | Trust state model (PCP vs DSCP) | QoS mode switch (VLAN vs DSCP) |
| **Bond Awareness** | Not checked | Blocks all DCB changes when bonded |
| **Locking** | state_lock + netdev_lock | tc_mutex + RTNL |

### ETS Comparison

| Feature | mlx5 | ice |
|---------|------|-----|
| **TC Groups** | Vendor-specific group model (0-7) | Standard IEEE TC table |
| **TSA Caching** | Driver caches TSA (FW doesn't store) | FW stores via LLDP MIB |
| **Zero-BW ETS** | Special handling with group splitting | Standard behavior |
| **Validation** | Custom validator with zero-sum option | `ice_dcb_bwchk()` + contiguity check |
| **Recommendation** | Not separate | Separate etsrec maintained |
| **Willing** | Not exposed in getets | Directly from dcbxcfg |

### PFC Comparison

| Feature | mlx5 | ice |
|---------|------|-----|
| **Stats Source** | Per-priority pport stats | Per-priority xoff counters |
| **Delay** | Cable length (driver-managed) | HW register (PRTDCB_GENC) |
| **Buffer Config** | SW-owned buffer management | FW-managed |
| **Link Reset** | Toggle link on PFC change | Full device reset if HW change |
| **PFC Mode** | Implicit via trust state | Explicit VLAN vs DSCP PFC mode |

### DCBX Comparison

| Feature | mlx5 | ice |
|---------|------|-----|
| **Modes** | Host vs Auto | IEEE-only, CEE-only, or FW-managed |
| **CEE+IEEE** | Both simultaneously | Mutually exclusive |
| **FW Agent** | Version-based (OPER mode) | Binary flag (FW_LLDP_AGENT) |
| **MIB Events** | Not directly handled | Full LLDP MIB change processing |
| **Remote Config** | Not cached | remote_dcbx_cfg maintained |

## RDMA Implications

### Critical Path: TC Change Notification

For RDMA (iWARP via irdma on ice, RoCE on mlx5), TC changes are the most
disruptive DCB event. The notification path differs significantly:

**ice + irdma path:**
1. `ice_pf_dcb_cfg()` sends `IIDC_RDMA_EVENT_BEFORE_TC_CHANGE` to irdma AUX driver.
2. irdma sets `tc_change_pending` on the VSI, suspends QPs.
3. ice disables VSIs, reconfigures TCs, re-enables VSIs.
4. ice sends `IIDC_RDMA_EVENT_AFTER_TC_CHANGE`.
5. irdma calls `irdma_change_l2params()` -> `irdma_set_qos_info()` to update
   per-UP QoS parameters -> `irdma_sc_suspend_resume_qps()` to resume.
6. QoS info propagated: UP-to-TC mapping, relative bandwidth, priority type,
   DSCP mode/mapping.

**mlx5 + mlx5_ib path:**
1. DCB changes are applied via individual register writes (no bulk reconfiguration).
2. TC setup goes through `mlx5e_setup_tc_mqprio_dcb()` ->
   `mlx5e_safe_switch_params()` which atomically switches channel parameters.
3. mlx5_ib has more direct firmware access and can query TC state independently.
4. No explicit BEFORE/AFTER event pattern — changes are more atomic.

### PFC Lossless Guarantee

For RoCEv2 and iWARP, PFC on the RDMA traffic class is essential for lossless
transport:

- **mlx5**: Port buffer management gives fine-grained control over headroom
  allocation per priority. Cable length awareness allows proper Xoff threshold
  calculation. SW-owned buffer mode ensures the driver has full control.
- **ice**: Buffer management is firmware-handled. The driver trusts firmware to
  allocate appropriate headroom based on PFC configuration. Less control but
  simpler driver code.

### DSCP for RDMA

Both drivers support DSCP-based QoS, which is important for routed RDMA fabrics:

- **mlx5**: Trust state model means DSCP classification can be enabled per-port
  without disrupting VLAN-based QoS on other ports. Inline mode adjustment
  ensures hardware inspects L3 headers.
- **ice**: QoS mode is port-wide VLAN or DSCP. Switching modes is heavyweight
  (reconfigures all TCs). In DSCP mode, UP-to-TC mapping is fixed.
- **irdma**: Consumes DSCP mapping from ice via `dscp_map[]` and `dscp_mode` flag.
  Each QP is assigned to a user priority which maps to a TC and QS handle.

### QP-to-TC Assignment

irdma maintains per-UP QoS lists (`vsi->qos[priority].qplist`) with mutex
protection. Each QP gets a `qs_handle` (queue set handle) from its user priority's
QoS entry. This allows the scheduler to apply per-TC bandwidth and priority
policies to RDMA traffic.

## fdice_ Recommendations

### Architecture Decision: Hybrid Approach

Adopt a **hybrid model** combining the best of both:

1. **mlx5-inspired register-based interface** for the below-AQ layer: Individual
   E810 register/AQ commands for ETS, PFC, and DCBX parameters. This avoids the
   complexity of LLDP MIB TLV construction while maintaining precise control.

2. **ice-inspired configuration triple** for state management: Maintain
   `local`, `desired`, and `remote` configuration structures. This provides
   clean separation between what the peer wants, what we want, and what is active.

3. **mlx5-inspired port buffer management**: Implement `fdice_port_buffer_config()`
   with SW-owned buffer mode for PFC headroom calculation. This is critical for
   RDMA lossless guarantees.

### Specific Recommendations

#### ETS (`fdice_ets_*`)

- Use standard IEEE TC table model (not mlx5's vendor group abstraction).
- Cache TSA state in driver like mlx5 (E810 firmware may not store it natively).
- Implement bandwidth validation with the zero-sum allowance for CEE compatibility.
- Support ETS recommendation tracking from remote peers.
- Enforce contiguous TC requirement (E810 limitation, like ice).
- Implement per-TC max rate limiting (mlx5 feature missing in ice).

#### PFC (`fdice_pfc_*`)

- Implement SW-controlled port buffer management for PFC headroom.
- Calculate Xoff thresholds based on cable length, MTU, and link speed.
- Track PFC statistics per priority (requests and indications).
- Support both VLAN-based and DSCP-based PFC modes.
- Avoid full device reset on PFC changes — use link toggle approach like mlx5.

#### DCBX (`fdice_dcbx_*`)

- Support IEEE and CEE modes (mutually exclusive, like ice, is simpler and safer).
- Implement FW LLDP agent awareness with read-only mode when agent is active.
- Handle LLDP MIB change events for firmware-managed mode.
- Block DCB changes when interface is bonded (ice pattern, important for production).
- Provide host/auto mode switching via `fdice_dcbx_set_mode()`.

#### RDMA Integration (`fdice_dcb_rdma_*`)

- Implement BEFORE/AFTER TC change notification pattern (ice/irdma model is proven).
- Export QoS info structure for RDMA AUX driver consumption:
  - UP-to-TC mapping
  - Per-TC relative bandwidth
  - PFC mode (VLAN vs DSCP)
  - DSCP mapping table when in DSCP mode
- Provide `fdice_dcb_get_qos_info()` API for RDMA driver to query current state.
- Ensure atomic TC reconfiguration to minimize RDMA disruption.

#### DSCP Support (`fdice_dscp_*`)

- Implement trust state model (mlx5 approach) rather than port-wide QoS mode switch.
- This allows more flexible per-port DSCP configuration without disrupting
  VLAN-based QoS.
- Track DSCP-to-priority mappings via dcb_app framework.
- Auto-revert to PCP trust when all DSCP mappings are removed.

### Naming Convention

All DCB/QoS functions follow the fdice_ prefix rule:

```
fdice_dcbnl_ieee_getets()    fdice_dcbnl_ieee_setets()
fdice_dcbnl_ieee_getpfc()    fdice_dcbnl_ieee_setpfc()
fdice_dcbnl_getdcbx()        fdice_dcbnl_setdcbx()
fdice_dcbnl_getbuffer()      fdice_dcbnl_setbuffer()
fdice_ets_init()             fdice_pfc_init()
fdice_dcbx_initialize()      fdice_trust_initialize()
fdice_dcb_notify_rdma()      fdice_dcb_get_qos_info()
```

### Implementation Priority

1. **Phase 1**: Basic ETS and PFC via IEEE dcbnl ops (minimum for RDMA).
2. **Phase 2**: DCBX mode management and FW LLDP agent support.
3. **Phase 3**: Port buffer management for PFC headroom optimization.
4. **Phase 4**: DSCP support and trust state management.
5. **Phase 5**: CEE legacy support and full RDMA QoS integration.
