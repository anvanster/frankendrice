<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 06: SR-IOV and VF Management -- mlx5 vs ice

## Overview

This document compares the SR-IOV virtual function management models of mlx5
(ConnectX) and ice (E810), with particular focus on mlx5's E-Switch offload
architecture which is significantly more capable than ice's VF management.
Findings inform the fdice_ SR-IOV design.

## mlx5 SR-IOV and E-Switch Architecture

### Two-Mode E-Switch

mlx5 implements a full-featured embedded switch with two operating modes,
selectable via devlink:

1. **Legacy mode** (`MLX5_ESWITCH_LEGACY`): Traditional SR-IOV with MAC/VLAN
   filtering via L2 FDB tables. The PF manages VF networking through the
   standard `ndo_set_vf_*` netdev ops. Each VF gets a vport with ingress/egress
   ACLs for spoofcheck and VLAN enforcement.

2. **Offloads/Switchdev mode** (`MLX5_ESWITCH_OFFLOADS`): The eswitch becomes a
   fully programmable TC flower offload engine. Each VF (and the PF uplink)
   gets a **port representor** netdev. Traffic steering rules are programmed
   via TC flower on representors and offloaded to hardware FDB flow tables.

### Key E-Switch Structures

**`struct mlx5_eswitch`** (eswitch.h:373-422):
- `vports` xarray: all vport objects (PF, VFs, SFs, ECPF, uplink)
- `mode`: LEGACY or OFFLOADS
- `qos`: hierarchical QoS with TSAR scheduling nodes per vport
- `offloads`: the `mlx5_esw_offload` struct containing all switchdev state
- `br_offloads`: bridge offload support for L2 bridging between vports
- `mode_lock`: RW semaphore protecting mode transitions
- `state_lock`: mutex protecting vport state changes

**`struct mlx5_esw_offload`** (eswitch.h:296-326):
- `vport_reps` xarray: port representor objects per vport
- `rep_ops[NUM_REP_TYPES]`: pluggable ops for ETH and IB representors
- `encap_tbl` / `decap_tbl`: tunnel encap/decap hash tables
- `mod_hdr`: header modification table for rewriting
- `peer_flows`: cross-device flows for LAG/dual-port
- `vhca_map`: maps vhca_id to vports for multi-host
- `ft_offloads` / `ft_offloads_restore`: flow tables for TC offload pipeline
- `inline_mode`: controls how much packet header is sent to software
- `encap`: VXLAN/GRE encap mode setting

**`struct mlx5_vport`** (eswitch.h:205-250):
- Per-vport ingress/egress ACLs (with legacy and offloads sub-structs)
- UC/MC L2 address hash tables
- `metadata`: vport metadata for flow matching (reg_c0 source port encoding)
- `qos.sched_node`: QoS scheduling node with TC arbiter support
- `dl_port`: devlink port for devlink integration

**`struct mlx5_eswitch_rep`** (include/linux/mlx5/eswitch.h:53-61):
- `rep_data[NUM_REP_TYPES]`: per-type (ETH, IB) private data and state
- Each rep type has `load/unload/get_proto_dev/event` ops
- REP_ETH creates a netdev representor; REP_IB creates an IB port

### E-Switch Offload Capabilities

The mlx5 eswitch in offloads mode provides:

1. **TC Flower Offload**: Full hardware offload of TC flower rules into FDB
   flow tables. Supports match on L2-L4 headers, tunnel headers, CT state,
   and chain/priority.

2. **Tunnel Encap/Decap**: Hardware VXLAN, GRE, Geneve encap/decap with
   per-flow rewrite.

3. **Connection Tracking**: Hardware CT offload via `en/tc_ct.c` with zone
   support, NAT, and flow aging.

4. **Header Rewriting**: `mod_hdr` table supports packet field modification
   (MAC, IP, TCP/UDP port, VLAN push/pop).

5. **Chain/Priority Support**: Multi-table FDB chains via `fs_chains` for
   complex pipeline processing.

6. **Bridge Offloads**: L2 bridge learning and FDB offload between vports
   (esw/bridge.c, bridge_mcast.c).

7. **IPsec Offload**: Per-VF IPsec crypto and packet offload (esw/ipsec.c).

8. **LAG Support**: Dual-port bonding with eswitch peer flows and shared FDB.

9. **SubFunction (SF) Support**: Lightweight functions beyond VFs, each with
   their own vport and representor.

10. **QoS**: Hierarchical rate limiting with TSAR scheduling, per-vport
    min/max rates, and TC-aware arbiters.

### mlx5 SR-IOV Enable Flow

```
mlx5_sriov_enable()
  -> mlx5_device_enable_sriov(num_vfs)
       -> mlx5_eswitch_enable(esw, num_vfs)
            -> mlx5_eswitch_enable_locked()
                 -> mlx5_esw_acls_ns_init()       // ACL namespaces
                 -> mlx5_eswitch_update_num_of_vfs()
                 -> esw_legacy_enable() OR esw_offloads_enable()
                 -> mlx5_eswitch_event_handler_register()
       -> for each VF:
            mlx5_core_enable_hca(vf)       // FW-level VF enable
            mlx5_set_msix_vec_count(vf)    // Configure MSI-X
            sriov_restore_guids(vf)        // Restore IB GUIDs
  -> pci_enable_sriov(num_vfs)             // PCI SR-IOV enable
```

### Representor Model

In switchdev mode, each vport (uplink PF + every VF/SF) gets a netdev
representor. The representor model uses pluggable ops:

```c
struct mlx5_eswitch_rep_ops {
    int (*load)(dev, rep);           // Create netdev/IB port
    void (*unload)(rep);             // Destroy netdev/IB port
    void *(*get_proto_dev)(rep);     // Return netdev or ib_device
    int (*event)(esw, rep, event, data); // Handle switchdev events
};
```

Two rep types exist: `REP_ETH` (netdev representors, in en_rep.c) and
`REP_IB` (RDMA representors). The ETH rep creates a full netdev with TC
flower offload hooks, allowing OVS or TC to program the hardware FDB.

## ice VF Management Model

### Architecture

ice implements a more traditional SR-IOV model centered on:

1. **PF-managed VFs** via virtchnl mailbox protocol
2. **Per-VF VSI** for L2 isolation
3. **Basic switchdev support** (added later, less capable)

### Key Structures

**`struct ice_vf`** (ice_vf_lib.h:139-209):
- `vf_id`, `lan_vsi_idx`: VF identity and VSI mapping
- `cfg_lock`: mutex for VF configuration serialization
- `vf_states` bitmap: INIT, ACTIVE, QS_ENA state machine
- `virtchnl_ops`: pluggable virtchnl message handler ops
- `vf_ops`: pluggable VF lifecycle ops (reset, cleanup)
- `devlink_port`: devlink port for each VF
- MAC, VLAN, spoofcheck, trust, link, bandwidth config
- `opcodes_allowlist`: per-VF virtchnl opcode whitelist
- `qs_bw[]`: per-queue bandwidth configuration

**`struct ice_vf_ops`** (ice_vf_lib.h:116-126):
- `free`, `clear_reset_state`, `clear_mbx_register`
- `trigger_reset_register`, `poll_reset_status`
- `clear_reset_trigger`, `irq_close`, `post_vsi_rebuild`
- Focused entirely on reset lifecycle, not data-path offloads

**`struct ice_virtchnl_ops`** (virt/virtchnl.h:41-80):
- ~35 ops covering VF-to-PF message handling
- Queue config, RSS, VLAN, MAC, promiscuous mode
- QoS, FDIR, PTP capabilities
- This is the primary VF configuration interface

**`struct ice_vfs`** (ice_vf_lib.h:129-136):
- Hash table of VF entries with table_lock
- `num_supported`, `num_qps_per`, `num_msix_per`

**`struct ice_eswitch`** (ice.h:534-539):
- `uplink_vsi`: the PF's main VSI
- `br_offloads`: bridge offload support
- `reprs` xarray: port representors
- `is_running`: simple boolean state
- Dramatically simpler than mlx5's eswitch

### ice SR-IOV Enable Flow

```
ice_sriov_configure(num_vfs)
  -> ice_pci_sriov_ena(num_vfs)
       -> ice_check_sriov_allowed()
       -> ice_set_per_vf_res()          // Allocate per-VF resources
       -> ice_create_vf_entries()        // Create VF structs
       -> for each VF:
            ice_initialize_vf_entry()    // Set defaults
            ice_create_vf_entries()      // VSI, mappings
       -> pci_enable_sriov(num_vfs)
       -> ice_sriov_remap_vectors()
```

### ice Switchdev Mode

ice's switchdev is minimal compared to mlx5:

- `ice_eswitch_mode_set()`: switches between LEGACY and SWITCHDEV
- In switchdev mode, creates port representors via `ice_repr_add_vf()`
- Uses `metadata_dst` for TX steering to VF VSIs
- Supports basic bridge offloads (`ice_eswitch_br.c`)
- **No TC flower hardware offload** of the FDB flow table
- **No tunnel encap/decap offload** through the eswitch
- **No connection tracking offload**
- **No header rewriting offload**
- Mode switch requires no VFs to be present

### Virtchnl Protocol

The ice driver's primary VF management interface is the virtchnl mailbox:

- PF and VF communicate via Admin Queue mailbox messages
- VF sends capability negotiation, queue config, filter requests
- PF validates and applies via `ice_virtchnl_ops` dispatch table
- Opcodes are allowlisted per-VF for security
- Malicious VF detection via mailbox overflow monitoring

## Key Capability Differences

| Capability | mlx5 | ice |
|---|---|---|
| E-Switch modes | Legacy + Offloads (switchdev) | Legacy + basic switchdev |
| Port representors | Full netdev per VF/SF with TC hooks | Basic netdev per VF |
| TC flower FDB offload | Yes, full L2-L4 + tunnel + CT | No |
| Tunnel encap/decap | VXLAN, GRE, Geneve in hardware | No eswitch offload |
| Connection tracking | Hardware CT with zones, NAT | No |
| Header rewriting | Full mod_hdr table | No |
| FDB chain/priority | Multi-table chains | No |
| Bridge offloads | Full L2 learning + multicast | Basic bridge offload |
| IPsec per-VF | Crypto + packet offload | No |
| SubFunctions (SF) | Yes, lightweight SR-IOV alternative | No (added later, limited) |
| QoS model | Hierarchical TSAR with TC arbiters | Per-VF min/max rate |
| LAG with SR-IOV | Shared FDB, peer flows | Basic LAG support |
| VF configuration | FW commands (HCA vport context) | Virtchnl mailbox |
| Vport metadata | reg_c0 source port encoding | VSI number in TX descriptor |
| Rep types | ETH + IB (RDMA) | ETH only |
| ECPF support | Yes (embedded CPU PF) | No |
| Devlink port functions | hw_addr, roce, migratable, ipsec, state, max_io_eqs | Basic |

## What E810 Hardware Supports for SR-IOV

The E810 hardware provides these SR-IOV primitives (accessible via Admin Queue):

1. **VF VSI allocation**: Each VF gets a dedicated VSI with its own queues,
   MAC/VLAN filters, and RSS configuration.

2. **Switch rules**: The E810 internal switch supports MAC, VLAN, and
   ethertype-based forwarding rules. Rules can direct traffic to specific
   VSIs.

3. **TX descriptor steering**: The TX context descriptor supports
   `SWTCH_VSI` and `SWTCH_UPLINK` commands for directing packets between
   VSIs, enabling basic representor TX.

4. **Anti-spoofing**: Per-VSI source MAC and VLAN checking.

5. **Mailbox queues**: VF-to-PF mailbox via Admin Queue for virtchnl.

6. **VF reset**: VFLR and software-triggered VF reset via VPGEN_VFRTRIG.

7. **Per-queue bandwidth**: Committed and peak rate per queue.

8. **FDIR**: Flow Director rules can be programmed per-VF for hardware
   flow steering (ice has virtchnl FDIR ops for VF-initiated rules).

9. **Advanced switch recipes**: The E810 supports programmable switch
   recipes for custom match fields, but ice uses this sparingly.

The E810 does **not** have:
- Hardware FDB flow table programmable via a generic API (like mlx5 flow steering)
- Hardware connection tracking
- Hardware tunnel encap/decap in the switch pipeline
- Hardware header modification engine (comparable to mlx5 modify_hdr)
- Hardware packet replication for multicast in the eswitch path
- SubFunction support at the hardware level

## Recommendations for fdice_ VF/SR-IOV Design

### ADOPT from mlx5

1. **Vport abstraction**: Create `struct fdice_vport` as a unified abstraction
   for PF, VFs, and any future SF-like entities. Include devlink_port, QoS
   node, and ACL state. This is above the AQ interface and benefits from
   mlx5's superior design.

2. **Two-mode eswitch with clean separation**: Implement
   `struct fdice_eswitch` with legacy and switchdev modes. Even though E810
   hardware cannot match mlx5's offload capabilities, the software
   architecture should support mode switching cleanly.

3. **Port representor framework**: Implement pluggable representor ops
   (`struct fdice_rep_ops`) for extensibility. Start with ETH reps; the
   abstraction allows RDMA reps later when irdma integration happens.

4. **Hierarchical QoS model**: Design QoS with scheduling nodes per vport
   rather than flat per-VF rates. Use mlx5's TSAR-like tree structure even
   if E810 QoS is simpler -- the software abstraction enables future
   hardware.

5. **Devlink port function integration**: Expose VF management through
   devlink port functions (hw_addr_get/set, state_get/set) rather than
   only through legacy ndo_set_vf_* ops.

6. **xarray-based vport/rep management**: Use xarrays (not arrays) for
   vport and representor storage, following mlx5's scalable approach.

### ADOPT from ice

7. **Virtchnl as the VF communication protocol**: This is the E810 hardware
   boundary. Use the virtchnl mailbox protocol with an ops dispatch table
   (`struct fdice_virtchnl_ops`) for VF message handling.

8. **VF ops abstraction**: Keep ice's `vf_ops` pattern for VF lifecycle
   (reset, cleanup) which cleanly separates SR-IOV VF mechanics from
   potential future SF mechanics.

9. **Opcodes allowlist**: Per-VF virtchnl opcode allowlisting for security.

10. **Malicious VF detection**: Mailbox overflow monitoring from ice.

### IMPROVE in fdice_

11. **Software-emulated offloads on representors**: Even though E810 lacks
    hardware FDB offload, implement the TC flower infrastructure on
    representors with software fast-path fallback. This gives operators the
    switchdev UX (OVS offload API) while allowing future hardware
    acceleration if the architecture supports it.

12. **Unified VF state machine**: Combine ice's scattered VF state bits
    into a proper state machine with defined transitions:
    `DISABLED -> INITIALIZING -> ACTIVE -> QS_ENABLED -> RESETTING -> DISABLED`

13. **Clean mode transition**: Unlike ice (which requires no VFs for mode
    switch) and mlx5 (which handles it internally), fdice_ should support
    graceful mode transition with VF migration/pause if possible.

14. **Per-vport flow counter framework**: Even without full FDB offload,
    implement per-vport flow counters using E810's statistics capabilities
    to provide visibility into VF traffic patterns.

15. **Eswitch bridge offloads from day one**: Implement basic bridge
    offloads using E810 switch rules, providing L2 forwarding between
    VF VSIs without going through the host CPU.

### Architecture Summary

```
            +-- fdice_eswitch (mlx5-inspired, above AQ) --+
            |                                              |
            |  fdice_vport[]  -- xarray of vports          |
            |    |-- devlink_port                          |
            |    |-- qos scheduling node                   |
            |    |-- ingress/egress ACLs                   |
            |                                              |
            |  fdice_rep_ops  -- pluggable representors    |
            |    |-- ETH rep (netdev)                      |
            |    |-- IB rep (future, for irdma)            |
            |                                              |
            |  Mode: LEGACY | SWITCHDEV                    |
            +----------------------------------------------+
                              |
                         AQ boundary
                              |
            +-- E810 Hardware (ice-derived, below AQ) -----+
            |                                              |
            |  Virtchnl mailbox  (VF <-> PF)               |
            |  VF VSI allocation                           |
            |  Switch rules (MAC/VLAN/recipe)              |
            |  VF reset (VFLR/SW)                          |
            |  Per-queue BW scheduling                     |
            +----------------------------------------------+
```

## Source Files Examined

### mlx5
- `core/sriov.c` -- SR-IOV enable/disable, VF HCA management
- `core/eswitch.h` -- mlx5_eswitch, mlx5_vport, mlx5_esw_offload structs
- `core/eswitch.c` -- eswitch init/enable/disable, vport management
- `core/eswitch_offloads.c` -- switchdev mode, representor loading, FDB rules
- `core/en_rep.c` -- ETH representor netdev ops
- `core/esw/` -- bridge, QoS, ACL, IPsec, devlink_port, LAG sub-modules
- `include/linux/mlx5/eswitch.h` -- rep_ops, rep types, vport metadata

### ice
- `ice_sriov.c` -- SR-IOV configure, VF create/destroy, reset lifecycle
- `ice_sriov.h` -- SR-IOV function declarations
- `ice_vf_lib.h` -- ice_vf struct, ice_vf_ops, ice_vfs, state machine
- `ice_vf_lib.c` -- VF lifecycle, reset, reference counting
- `ice_eswitch.c` -- switchdev mode, representor setup, TX steering
- `ice_eswitch.h` -- eswitch declarations
- `ice_repr.c` -- port representor netdev creation
- `virt/virtchnl.h` -- ice_virtchnl_ops dispatch table
- `virt/virtchnl.c` -- virtchnl message handling
