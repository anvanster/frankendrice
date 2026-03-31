<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 15: Switchdev Mode

## Overview

Ice's switchdev implementation provides a basic embedded switch (eswitch) mode
that creates port representor netdevs for VFs and SubFunctions, enabling
software-defined forwarding via the Linux bridge and limited hardware FDB
offload. Compared to mlx5's comprehensive switchdev with full TC flower FDB
offload, tunnel encap/decap, connection tracking, and header modification,
ice's implementation is significantly narrower -- focused primarily on bridge
FDB learning offload and basic representor networking.

## Key Source Files

| File | Role |
|------|------|
| `ice_eswitch.c` | Eswitch mode set/get, enable/disable, attach/detach, environment setup |
| `ice_eswitch.h` | `ice_is_eswitch_mode_switchdev()`, `ice_is_switchdev_running()` helpers |
| `ice_eswitch_br.c` | Bridge offloads: FDB entry management, VLAN handling, switchdev notifiers |
| `ice_eswitch_br.h` | Bridge structures: `ice_esw_br`, `ice_esw_br_port`, `ice_esw_br_fdb_entry` |
| `ice_repr.c` | Port representor creation, netdev_ops, TC flower on reprs |
| `ice_repr.h` | `ice_repr` struct, `ice_repr_type` enum |
| `ice.h` | `ice_eswitch` struct (minimal: uplink_vsi, br_offloads, reprs xarray) |

## Eswitch Data Structures

### `ice_eswitch` (Minimal)

```c
struct ice_eswitch {
    struct ice_vsi *uplink_vsi;              /* PF uplink VSI */
    struct ice_esw_br_offloads *br_offloads; /* bridge offload state */
    struct xarray reprs;                     /* representor xarray */
    bool is_running;                         /* eswitch active flag */
};
```

This is strikingly minimal compared to mlx5's `mlx5_eswitch` which has ~50
fields including vports xarray, mode, offloads state with encap/decap tables,
mod_hdr, flow chains, peer flows for LAG, QoS TSAR tree, and bridge offloads.

### `ice_repr` (Port Representor)

```c
struct ice_repr {
    struct ice_vsi *src_vsi;                 /* VF/SF's VSI */
    struct net_device *netdev;               /* representor netdev */
    struct metadata_dst *dst;                /* metadata for TX steering */
    struct ice_repr_pcpu_stats __percpu *stats;
    u32 id;
    u8 parent_mac[ETH_ALEN];
    enum ice_repr_type type;                 /* VF or SF */
    union {
        struct ice_vf *vf;
        struct ice_dynamic_port *sf;
    };
    struct {
        int (*add)(struct ice_repr *repr);   /* type-specific add */
        void (*rem)(struct ice_repr *repr);  /* type-specific remove */
        int (*ready)(struct ice_repr *repr); /* type-specific ready */
    } ops;
};
```

Representors support both VFs (`ice_repr_create_vf`) and SubFunctions
(`ice_repr_create_sf`), with pluggable ops for type-specific lifecycle.

## Switchdev Mode Lifecycle

### Mode Transition: `ice_eswitch_mode_set`

Called via devlink eswitch mode set. Key constraints:

1. **No VFs present**: Mode change is only allowed with zero VFs created
2. **No ADQ active**: `ice_is_adq_active()` must be false for switchdev
3. **Mode values**: `DEVLINK_ESWITCH_MODE_LEGACY` or `DEVLINK_ESWITCH_MODE_SWITCHDEV`

On transition to switchdev:
- Initializes `pf->eswitch.reprs` xarray
- Sets `pf->eswitch_mode = DEVLINK_ESWITCH_MODE_SWITCHDEV`

On transition to legacy:
- Destroys `pf->eswitch.reprs` xarray
- Sets `pf->eswitch_mode = DEVLINK_ESWITCH_MODE_LEGACY`

Note: The actual switchdev environment is set up lazily when the first VF/SF
is attached (not during mode_set itself).

### Enabling Switchdev: `ice_eswitch_enable_switchdev`

Called from `ice_eswitch_attach` when the first representor is being added:

1. Get uplink (main PF) VSI
2. Verify uplink is not already a bridge port
3. Store `pf->eswitch.uplink_vsi`
4. Call `ice_eswitch_setup_env` -- configures uplink VSI for eswitch mode
5. Call `ice_eswitch_br_offloads_init` -- sets up bridge offload infrastructure
6. Set `pf->eswitch.is_running = true`

### Environment Setup: `ice_eswitch_setup_env`

Transforms the uplink VSI for switchdev operation:

1. Bring down uplink VSI if running
2. Remove all existing filters (`ice_remove_vsi_fltr`)
3. Enable SW LLDP passthrough
4. Unsync unicast/multicast address lists
5. Add VLAN 0, set as default VSI (Rx and Tx)
6. **Disable VLAN Rx filtering** -- promiscuous for VLAN
7. **Enable local loopback** -- allows hairpin between VFs through software
8. Bring uplink back up

This is significantly simpler than mlx5's eswitch enable which sets up vport
contexts, flow steering namespaces, FDB flow tables, and per-vport ACLs.

### Attaching VFs/SFs: `ice_eswitch_attach`

Called from `ice_eswitch_attach_vf` or `ice_eswitch_attach_sf`:

1. If reprs xarray is empty, call `ice_eswitch_enable_switchdev` (lazy init)
2. Stop all existing representors (`ice_eswitch_stop_reprs`)
3. Call `repr->ops.add(repr)` -- type-specific representor creation
4. Setup representor: `ice_eswitch_setup_repr` -- allocates metadata_dst
5. Insert into reprs xarray: `xa_insert(&pf->eswitch.reprs, repr->id, repr)`
6. Restart all representors (`ice_eswitch_start_reprs`)

### Representor TX Steering via metadata_dst

```c
/* In ice_eswitch_setup_repr: */
repr->dst = metadata_dst_alloc(0, METADATA_HW_PORT_MUX, GFP_KERNEL);
dst->u.port_info.port_id = vsi->vsi_num;
dst->u.port_info.lower_dev = uplink_vsi->netdev;
```

When a packet is transmitted on a representor netdev, the metadata_dst carries
the target VSI number. The uplink VSI's TX path reads this metadata to steer
the packet to the correct VF/SF VSI via the hardware switch. This is a
"slow-path" model -- all representor traffic goes through the PF's uplink VSI.

## Bridge FDB Offload

### Architecture

The bridge offload subsystem (`ice_eswitch_br.c`, ~1359 lines) handles:

- FDB entry creation/deletion via switchdev notifiers
- VLAN-aware bridge port management
- Hardware rule programming for learned MAC addresses

### FDB Entry Flow

```
switchdev FDB notification
  -> ice_eswitch_br_fdb_event_work
    -> ice_eswitch_br_fdb_entry_create
      -> ice_eswitch_br_flow_create (forward + guard rules via AQ)
      -> rhashtable_insert (fdb_ht)
      -> ice_eswitch_br_fdb_offload_notify (SWITCHDEV_FDB_OFFLOADED)
```

Each FDB entry creates two hardware rules:
1. **Forward rule** -- matches destination MAC, forwards to correct VSI
2. **Guard rule** -- prevents flooding for known MACs

Rules are programmed via the E810 Admin Queue switch rule interface
(`ice_eswitch_br_rule_delete` etc.), using the same underlying mechanism
as static switch filters.

### FDB Data Structures

- `ice_esw_br` -- bridge instance with `fdb_ht` rhashtable and `ports` xarray
- `ice_esw_br_port` -- per-port state with `vlans` xarray
- `ice_esw_br_fdb_entry` -- FDB entry with `data` (MAC + VID), flow rules, flags
- `ice_esw_br_fdb_data` -- hash key: MAC address + VLAN ID

### VLAN Handling

Bridge ports maintain a VLAN xarray (`port->vlans`). VLAN metadata is looked
up per-port to ensure FDB entries are VLAN-aware. The
`ice_esw_br_port_vlan_lookup` function resolves VLAN context for FDB
operations.

## TC Flower on Representors

Representors have their own `ndo_setup_tc` via `ice_repr_setup_tc`:

```
ice_repr_setup_tc
  -> TC_SETUP_BLOCK
    -> ice_repr_setup_tc_cls_flower
      -> FLOW_CLS_REPLACE: ice_add_cls_flower (same as PF path)
      -> FLOW_CLS_DESTROY: ice_del_cls_flower
```

This reuses the same `ice_tc_lib.c` infrastructure as ADQ flower offload.
Flower filters on representors can steer traffic between VFs and the uplink,
using E810 advanced switch recipes. The `ice_is_port_repr_netdev` check in
`ice_add_cls_flower` adjusts the VSI context when the filter target is a
representor.

## Comparison with mlx5 Switchdev (Reference: Exploration 06)

| Aspect | ice Switchdev | mlx5 Switchdev |
|--------|---------------|----------------|
| **Eswitch struct** | 4 fields (minimal) | ~50 fields (comprehensive) |
| **Modes** | LEGACY / SWITCHDEV | LEGACY / OFFLOADS (switchdev) |
| **Representor model** | Simple ops struct (add/rem/ready) | Pluggable `rep_ops` per protocol (ETH/IB) |
| **Representor types** | VF, SF | VF, SF, PF uplink, with vport abstraction |
| **FDB offload** | Basic MAC+VLAN via AQ switch rules | Full TC flower FDB with flow tables |
| **Tunnel offload** | None in eswitch | encap/decap (VXLAN, GRE, Geneve) |
| **Connection tracking** | None | Full CT offload in FDB |
| **Header modification** | None | mod_hdr (push/pop/set actions) |
| **Flow chains** | None | Multi-chain/priority flow table hierarchy |
| **Bridge offload** | FDB learning + VLAN-aware ports | FDB + L2 learning + group flooding |
| **QoS in eswitch** | None | Per-vport TSAR tree with tc_bw |
| **Per-vport ACLs** | None | Ingress/egress ACLs per vport |
| **Metadata encoding** | metadata_dst port_id | reg_c0 source port + metadata |
| **Lazy init** | Yes (first VF attach) | No (mode set triggers full setup) |
| **ADQ coexistence** | Mutually exclusive | TC works alongside eswitch |

### Key Gaps in ice Switchdev

1. **No TC flower FDB offload** -- cannot program arbitrary flow rules in eswitch
2. **No tunnel encap/decap** -- no VXLAN/GRE offload in eswitch mode
3. **No connection tracking** -- all CT must be done in software
4. **No header modification** -- no push/pop/set actions in hardware
5. **No per-vport QoS** -- no bandwidth/scheduling in eswitch context
6. **No flow table chaining** -- flat rule model only
7. **Slow-path only for repr TX** -- all traffic through PF uplink VSI

### What ice Does Well

1. **Clean lazy initialization** -- switchdev infra only created when needed
2. **xarray-based repr management** -- modern, scalable data structure
3. **Bridge VLAN awareness** -- proper per-port VLAN tracking
4. **Unified flower path** -- same `ice_tc_lib` for PF and repr filters
5. **SF support** -- SubFunction representors alongside VF representors

## E810 Hardware Switchdev Capabilities

Based on the E810's switch architecture:

- **Switch rules**: MAC/VLAN/ethertype match with forward-to-VSI action
- **Programmable recipes**: Up to 64 custom extraction patterns
- **Per-VSI isolation**: Hardware enforces VSI-based traffic separation
- **No generic FDB flow table API**: Unlike ConnectX, no hardware table hierarchy
- **No hardware CT/NAT**: Connection tracking must be software-based
- **No tunnel encap/decap in switch**: Tunnel endpoints handled by host stack
- **Metadata_dst for TX steering**: Software-based port mux, not hardware registers

## fdice_ Design Recommendations

### 1. mlx5-Inspired Eswitch Structure (Above AQ Boundary)

Adopt mlx5's richer `fdice_eswitch` structure while respecting E810 HW limits:

```
struct fdice_eswitch {
    struct fdice_vsi *uplink_vsi;
    enum fdice_eswitch_mode mode;
    struct xarray vports;              /* per-vport state (mlx5 pattern) */
    struct xarray reprs;               /* representor tracking */
    struct fdice_esw_bridge *bridge;   /* bridge offload state */
    struct fdice_esw_offloads offloads; /* offload capabilities */
    struct mutex mode_lock;
    bool is_running;
};
```

The vport abstraction (from mlx5, see Exploration 06) provides a clean
per-VF/SF context with devlink_port, QoS node, and future ACL support.

### 2. Pluggable Representor Ops (Adopt mlx5 Pattern)

Extend ice's simple add/rem/ready ops toward mlx5's protocol-aware model:

```
struct fdice_rep_ops {
    int (*load)(struct fdice_repr *repr);
    void (*unload)(struct fdice_repr *repr);
    struct net_device *(*get_proto_dev)(struct fdice_repr *repr);
    void (*event)(struct fdice_repr *repr, enum fdice_rep_event event);
};
```

This enables future RDMA representor support and cleaner lifecycle management
during eswitch reconfiguration.

### 3. Software-Emulated TC Flower on Representors

Since E810 lacks hardware FDB flow tables, implement a software fast-path:

- Use E810 switch recipes for simple MAC/VLAN/IP steering rules
- Build a software flow cache for complex matches that fall back to host stack
- Design the interface so hardware acceleration can be added transparently
  when future E810 variants support it

### 4. Eliminate ADQ/Switchdev Mutual Exclusion

This is a key improvement opportunity over ice:

- Design `fdice_tc_mgr` (from Exploration 14) to work in both modes
- In switchdev mode: flower filters go through eswitch repr path
- In legacy mode: flower filters go through PF advanced switch rules
- ADQ channel VSIs and switchdev repr VSIs should coexist in the VSI table

### 5. Bridge Offload Enhancement

Adopt ice's bridge offload pattern but enhance:

- Keep rhashtable-based FDB (good pattern from ice)
- Add flow counter support per FDB entry (E810 supports per-rule counters)
- Implement bridge group flooding via multicast replication rules
- Add aging support with configurable timeout
- Use switchdev notifier workqueue pattern (ice does this well)

### 6. Graceful Mode Transitions

Improve over ice's "no VFs present" requirement:

- Allow mode transitions with VFs present by implementing migration:
  1. Quiesce all VF traffic
  2. Save VF state (MAC, VLAN, QoS)
  3. Transition eswitch mode
  4. Recreate representors / restore legacy config
  5. Resume VF traffic
- This matches mlx5's more flexible eswitch mode management

### 7. Metadata-Based Source Port Encoding

Ice uses `metadata_dst` with `METADATA_HW_PORT_MUX` for TX steering.
For fdice_, consider:

- Keep metadata_dst for initial implementation (proven pattern)
- Design abstraction layer that could support hardware register-based
  encoding (like mlx5's reg_c0) if future E810 firmware adds support
- Use `fdice_port_id` abstraction that hides the steering mechanism

## Module Layout Recommendation

```
fdice_eswitch.c       -- mode management, vport abstraction
fdice_eswitch_br.c    -- bridge FDB offload, VLAN handling
fdice_repr.c          -- representor lifecycle, netdev_ops
fdice_repr_tc.c       -- TC flower on representors
fdice_eswitch_offloads.c  -- offload capability management (future)
```

## References

- **Exploration 04**: Flow steering architecture -- recipe-based matching
- **Exploration 06**: SR-IOV and VF management -- mlx5 eswitch vs ice comparison
- **Exploration 07**: Devlink integration -- eswitch mode ops
- **Exploration 14**: ADQ -- mutual exclusion with switchdev, TC flower sharing
