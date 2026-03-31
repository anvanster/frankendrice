<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 04: Flow Steering / Traffic Classification

## Overview

This document compares mlx5's sophisticated flow steering tree architecture with
ice's Flow Director and switchdev implementation, identifying patterns that can
be reimplemented for E810 in the fdice_ driver.

## mlx5 Flow Steering Architecture

### Tree Structure: The Namespace Hierarchy

mlx5 builds a **tree-shaped** flow steering hierarchy with five node types
defined in `fs_core.h`:

```
fs_node_type:
  FS_TYPE_NAMESPACE       -- grouping container (root of subtrees)
  FS_TYPE_PRIO            -- priority level within a namespace
  FS_TYPE_PRIO_CHAINS     -- parallel namespace container (for tc chains)
  FS_TYPE_FLOW_TABLE      -- hardware flow table
  FS_TYPE_FLOW_GROUP      -- match criteria grouping within a table
  FS_TYPE_FLOW_ENTRY      -- individual match+action rule (FTE)
  FS_TYPE_FLOW_DEST       -- destination (queue, TIR, other table, etc.)
```

The tree is rooted at `struct mlx5_flow_steering`, which holds multiple
**root namespaces** for different traffic domains:

| Root Namespace       | Purpose                          | Key Capability Gate                     |
|---------------------|----------------------------------|-----------------------------------------|
| `root_ns`           | NIC RX steering                  | `NIC_FLOW_TABLE + ft_support`           |
| `fdb_root_ns`       | FDB (eswitch forwarding database)| `ESW_FLOWTABLE_FDB + ft_support`        |
| `sniffer_rx_root_ns`| Sniffer/mirror RX                | `FLOWTABLE_SNIFFER_RX`                  |
| `sniffer_tx_root_ns`| Sniffer/mirror TX                | `FLOWTABLE_SNIFFER_TX`                  |
| `rdma_rx_root_ns`   | RDMA RX flow tables              | `FLOWTABLE_RDMA_RX`                     |
| `rdma_tx_root_ns`   | RDMA TX flow tables              | `FLOWTABLE_RDMA_TX`                     |
| `egress_root_ns`    | NIC TX steering                  | `FLOWTABLE_NIC_TX`                      |
| `port_sel_root_ns`  | LAG port selection               | `FLOWTABLE_PORT_SELECTION`              |
| `esw_egress_root_ns`| Per-vport egress (xarray)        | eswitch manager                         |
| `esw_ingress_root_ns`| Per-vport ingress (xarray)      | eswitch manager                         |

**File**: `kernel/drivers/net/ethernet/mellanox/mlx5/core/fs_core.c` -- `mlx5_fs_core_init()`

### The fs_node Base Type

Every node in the tree embeds `struct fs_node`, which provides:
- **list/children**: tree linkage via `list_head`
- **parent/root**: navigation pointers
- **rw_semaphore lock**: per-node locking for traversal and modification
- **refcount**: reference counting for lifetime management
- **del_hw_func / del_sw_func**: virtual destructors for hardware/software cleanup
- **atomic version**: for change tracking

This is a **generic tree infrastructure** pattern -- all flow steering objects
(namespaces, priorities, tables, groups, entries) share the same base type and
can be managed uniformly.

### Flow Tables (`mlx5_flow_table`)

- Embeds `fs_node` (children are flow groups)
- Has `id`, `level`, `max_fte`, `type`, `flags`
- Contains an **autogroup** mechanism for automatic group creation
- Links to its parent namespace via `ns` pointer
- Has `fgs_hash` (rhltable) for fast flow group lookup
- Supports forwarding chain via `fwd_rules` list
- Supports multiple steering backends via union: `fs_dr_table` (SMFS) or `fs_hws_table` (HMFS)

### Flow Groups (`mlx5_flow_group`)

- Embeds `fs_node` (children are FTEs)
- Contains `mlx5_flow_group_mask` with `match_criteria_enable` + `match_criteria[]`
- Has `start_index`, `max_ftes`, `fte_allocator` (ida)
- Supports multiple backends: `fs_dr_matcher` (SMFS) or `fs_hws_matcher` (HMFS)
- Groups FTEs that share the same match criteria mask

### Flow Table Entries (`fs_fte`)

- Embeds `fs_node` (children are flow rules/destinations)
- Contains match values `val[]` (the actual packet field values to match)
- Has `fs_fte_action` with flow context and action specification
- Supports duplicate rules via `fs_fte_dup`
- Backend union: `fs_dr_rule` or `fs_hws_rule`

### Command Abstraction (`mlx5_flow_cmds`)

The key architectural insight is `struct mlx5_flow_cmds` -- a **vtable of
function pointers** that abstracts the hardware command interface:

```c
struct mlx5_flow_cmds {
    int (*create_flow_table)(...);
    int (*destroy_flow_table)(...);
    int (*modify_flow_table)(...);
    int (*create_flow_group)(...);
    int (*destroy_flow_group)(...);
    int (*create_fte)(...);
    int (*update_fte)(...);
    int (*delete_fte)(...);
    int (*update_root_ft)(...);
    int (*packet_reformat_alloc)(...);
    int (*packet_reformat_dealloc)(...);
    int (*modify_header_alloc)(...);
    int (*modify_header_dealloc)(...);
    int (*create_ns)(...);
    int (*destroy_ns)(...);
    ...
};
```

Three implementations exist:
1. **FW commands** (`mlx5_fs_cmd_get_fw_cmds`) -- DMFS mode, uses firmware mailbox
2. **Software steering** (`mlx5_fs_cmd_get_dr_cmds`) -- SMFS mode, direct rule insertion
3. **Hardware steering** -- HMFS mode, hardware-accelerated steering

This is stored per-root-namespace, allowing different namespaces to use
different steering backends.

### Steering Modes

```c
enum mlx5_flow_steering_mode {
    MLX5_FLOW_STEERING_MODE_DMFS,  // Device-Managed Flow Steering (firmware)
    MLX5_FLOW_STEERING_MODE_SMFS,  // Software-Managed Flow Steering (dr)
    MLX5_FLOW_STEERING_MODE_HMFS,  // Hardware-Managed Flow Steering (hws)
};
```

### NIC-Level Flow Table Organization

At the NIC level (`en/fs.h`), mlx5e organizes flow tables into a priority-based
pipeline:

```
TC_PRIO (level 0) --> TC flow tables (tc flower offload)
PROMISC_PRIO      --> Promiscuous flow table
NIC_PRIO          --> VLAN -> L2 -> TTC (Type-to-TIR Classification)
```

The `mlx5e_flow_table` wrapper bundles a flow table with its flow groups for
convenient management. Each functional area (VLAN filtering, L2 MAC filtering,
TTC, aRFS) gets its own flow table at a specific level.

## ice Flow Director and Flow Classification

### Architecture: Flat Recipe-Based System

ice takes a fundamentally different approach. Rather than a tree of tables, it
uses a **recipe-based flat matching** system built around the E810 hardware's
switch/filter infrastructure.

### Key Components

#### Switch Recipes (`ice_sw_recipe`)

The central abstraction is the **switch recipe** -- a programmed pattern that
tells the hardware which packet fields to extract and match:

- `ext_words[]` -- Protocol ID + Offset pairs defining extraction points
- `word_masks[]` -- Masks for each extraction word
- `filt_rules` -- List of filter rules using this recipe
- `r_bitmap` -- Bitmap of recipe IDs in a chain (recipes can be combined)
- `tun_type` -- Tunnel type association
- `priority` -- Forwarding priority
- `lkup_exts` -- Protocol lookup extension data

Recipes are stored in `ice_switch_info.recp_list` (a flat array indexed by
recipe ID). The hardware supports up to `ICE_MAX_NUM_RECIPES` recipes.

#### Flow Profiles (`ice_flow_prof`)

Flow profiles define hash/match configurations for RSS and Flow Director:

- `segs[]` -- Up to `ICE_FLOW_SEG_MAX` (2) segments (outer/inner headers)
- Each segment has `hdrs` (protocol bitmask), `match` (field bitmask), `range`
- `entries` -- List of flow entries using this profile
- `vsis` -- Bitmap of VSIs using this profile

#### Flow Director Filters (`ice_fdir_fltr`)

FDIR provides exact-match or masked-match steering to specific queues:

- Supports IPv4/IPv6 with TCP/UDP/SCTP/L2TPv3/ESP/AH/GTP-U
- Each filter specifies: `flow_type`, IP tuple `ip/mask`, `q_index`, `dest_vsi`
- Uses **dummy packet programming** -- the driver builds an actual packet template
  that the hardware matches against (via `ice_fdir_base_pkt`)
- Hardware limit: `ICE_FDIR_MAX_FLTRS` (16384) filters
- Managed via a flat list (`fltr_node`) on the hardware struct

#### Flow Field Definitions (`ice_flow_field`)

An extensive enum of 50+ matchable fields covering L2-L4 headers plus
tunneling protocols (GTP-C, GTP-U, PPPoE, PFCP, L2TPv3, ESP, AH).

#### Segment Header Types (`ice_flow_seg_hdr`)

Bitmask enum covering all supported protocol headers, used to describe which
headers appear in a packet segment for flow classification.

### ice Switchdev / eSwitch Implementation

ice's switchdev mode (`ice_eswitch.c`, `ice_eswitch_br.c`) provides:

#### eSwitch Bridge (`ice_esw_br`)

- Port management via xarray (`ports`)
- FDB (Forwarding Database) via rhashtable (`fdb_ht`) + linked list
- VLAN filtering support
- Ageing timer for learned entries

#### Bridge Flow Offload (`ice_esw_br_flow`)

Each FDB entry has associated hardware flows:
- `fwd_rule` -- The forwarding rule (via `ice_rule_query_data`)
- `guard_rule` -- A guard rule for proper traffic handling

#### TC Flower Integration (`ice_tc_lib.c`)

ice supports TC flower offload for switchdev mode:
- `ice_tc_flower_fltr` -- TC flower filter representation
- `ice_tc_flower_action` -- Parsed TC actions
- `ice_add_tc_flower_adv_fltr()` -- Installs advanced switch filters
- Uses the switch recipe mechanism underneath for hardware programming

## Comparison: What Is Hardware-Specific vs. Software Abstraction

### Hardware-Specific (Below AQ Interface)

| Aspect | mlx5 (ConnectX) | ice (E810) |
|--------|-----------------|------------|
| Match mechanism | Flow table + group + FTE hierarchy in hardware | Recipe-based extraction with dummy packet programming |
| Table organization | Hardware supports hierarchical table chaining | Flat recipe array, no native table hierarchy |
| Steering backends | DMFS/SMFS/HMFS (three modes) | Single firmware-driven mode via AQ |
| Group/mask concept | Hardware flow groups with match criteria | Recipe extraction words define matching |
| Max filters | Very large (depends on table size) | 16384 FDIR filters, switch rules separate |
| Tunnel support | Built into flow table match criteria | Via tunnel-type recipes and segments |

### Software Abstraction (Above AQ Interface)

| Aspect | mlx5 | ice |
|--------|------|-----|
| Tree infrastructure | `fs_node` generic tree with ref counting | No tree -- flat lists and arrays |
| Command abstraction | `mlx5_flow_cmds` vtable (3 backends) | Direct AQ commands, no abstraction layer |
| Namespace isolation | Multiple root namespaces per domain | Single switch with recipes |
| Autogroup | Automatic flow group management | Manual recipe management |
| Priority/chaining | Multi-level priorities with table linking | Recipe priority field, no chaining |
| Switchdev | Full eswitch with FDB flow tables | Basic bridge offload with rule pairs |

## Patterns That Translate to E810

### Strongly Recommended for fdice_

1. **Generic Tree Infrastructure (`fs_node` pattern)**
   - The `fs_node` base type with embedded list, parent/root pointers, refcount,
     and virtual destructors is a clean, reusable pattern
   - For fdice_, adapt this as `fdice_fs_node` to manage flow steering objects
   - Even though E810 hardware is flat, the software tree provides clean lifetime
     management and hierarchical organization

2. **Command Abstraction Vtable (`mlx5_flow_cmds` pattern)**
   - Create `fdice_flow_cmds` with function pointers for create/destroy/modify
     operations on flow tables, groups, and entries
   - This enables future extensibility (e.g., different firmware versions,
     testing with mock backends)
   - Map the vtable operations to E810 Admin Queue commands

3. **Namespace Separation**
   - Maintain separate flow steering domains (NIC RX, NIC TX, eswitch FDB)
   - Even with E810's flat recipe model, logical namespaces prevent rule
     conflicts and simplify management
   - Use `fdice_flow_namespace` to group related flow tables

4. **NIC-Level Flow Table Pipeline**
   - Implement a priority-based pipeline similar to mlx5e's organization:
     TC offload -> VLAN filtering -> L2 MAC -> RSS/TTC classification
   - Each stage maps to E810 switch recipes at different priorities

5. **TC Flower Offload Integration**
   - Both drivers implement TC flower; ice's approach via `ice_tc_lib.c` maps
     directly to E810 capabilities
   - Rewrite as `fdice_tc_*` with cleaner recipe management

### Partially Applicable

6. **Flow Group / Match Criteria Grouping**
   - mlx5's flow groups (entries sharing the same match mask) are elegant but
     do not map 1:1 to E810 hardware
   - For fdice_, implement a software grouping layer that maps to E810 recipes:
     each "group" corresponds to a recipe with specific extraction words
   - This provides better organization than ice's raw recipe array

7. **Autogroup Mechanism**
   - mlx5's automatic flow group creation when adding rules is convenient
   - For fdice_, implement automatic recipe selection/creation when a new
     filter pattern is encountered

8. **Switchdev Bridge Offload**
   - ice's `ice_esw_br` is relatively simple; mlx5's eswitch is far more
     sophisticated with full FDB flow tables
   - For fdice_, start with ice's simpler bridge model but use the fdice_
     tree infrastructure for cleaner management

### Does Not Translate

9. **Multiple Steering Backends (DMFS/SMFS/HMFS)**
   - E810 only has firmware-managed steering via the Admin Queue
   - No need for the multi-backend abstraction; a single implementation suffices
   - However, keep the vtable for testability

10. **Hardware Flow Table Hierarchy**
    - mlx5 hardware natively supports chained flow tables with miss actions
    - E810 does not have table chaining in hardware
    - Software can emulate priority ordering, but true table-to-table forwarding
      is not available

11. **Flow Table Levels and Chaining**
    - mlx5's `connect_flow_table` and `list_add_flow_table` create linked chains
    - E810 recipes are independent; ordering is by priority only

## Recommendations for fdice_ Flow Steering Design

### Core Data Structures

```
fdice_flow_steering (top-level container)
  |
  +-- fdice_flow_namespace (NIC RX)
  |     +-- fdice_flow_priority (TC offload, VLAN, L2, RSS)
  |           +-- fdice_flow_table
  |                 +-- fdice_flow_group (match criteria -> recipe mapping)
  |                       +-- fdice_flow_entry (match values + action)
  |
  +-- fdice_flow_namespace (NIC TX)
  +-- fdice_flow_namespace (eSwitch FDB)
```

### Key Design Decisions

1. **Use `fdice_fs_node` as base type** for all flow steering objects, providing
   unified lifetime management, locking, and tree traversal.

2. **Implement `fdice_flow_cmds` vtable** that maps to E810 Admin Queue
   commands for recipe creation, filter programming, and FDIR operations.

3. **Map flow groups to E810 recipes**: Each `fdice_flow_group` with a specific
   match criteria mask gets a corresponding E810 switch recipe. Reuse recipes
   when multiple tables need the same match pattern.

4. **Implement FDIR with cleaner abstraction**: Wrap the dummy-packet
   programming in a `fdice_fdir` module that accepts structured match
   criteria rather than requiring callers to understand packet offsets.

5. **Priority-based pipeline in software**: Since E810 lacks hardware table
   chaining, implement a software priority system where each namespace has
   ordered flow tables, and the driver programs recipe priorities accordingly.

6. **Separate flow steering from recipe management**: Keep `fdice_recipe`
   as a low-level module managing E810 recipe hardware, while `fdice_flow_*`
   provides the higher-level abstraction. This mirrors the mlx5 separation
   between `fs_core.c` (tree management) and `fs_cmd.c` (hardware commands).

### Module Organization

```
src/flow/
  fdice_fs_core.c      -- Tree infrastructure, namespace/table management
  fdice_fs_cmd.c       -- AQ command interface (vtable implementation)
  fdice_fs_fdir.c      -- Flow Director wrapper (structured API over dummy packets)
  fdice_fs_recipe.c    -- E810 recipe management (low-level)
  fdice_fs_tc.c        -- TC flower offload integration
  fdice_fs_eswitch.c   -- Switchdev flow offload
  fdice_fs_ttc.c       -- Type-to-TIR/queue classification (RSS steering)
```

### Key Improvements Over ice

- **Structured tree** instead of flat recipe arrays and filter lists
- **Proper lifetime management** via refcounted fs_node objects
- **Command abstraction** for testability and future extensibility
- **Cleaner FDIR API** hiding dummy-packet implementation details
- **Namespace isolation** preventing cross-domain rule conflicts
- **Recipe reuse tracking** to avoid redundant recipe allocation

## Source Files Referenced

### mlx5 Flow Steering
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/fs_core.h` -- Core data structures
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/fs_core.c` -- Tree management, init
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/fs_cmd.h` -- Command vtable definition
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/fs_cmd.c` -- FW command implementation
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/en/fs.h` -- NIC-level flow tables
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/en_tc.c` -- TC offload
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/eswitch_offloads.c` -- Switchdev

### ice Flow Director and Switchdev
- `kernel/drivers/net/ethernet/intel/ice/ice_fdir.h` -- FDIR structures
- `kernel/drivers/net/ethernet/intel/ice/ice_flow.h` -- Flow field/segment definitions
- `kernel/drivers/net/ethernet/intel/ice/ice_flow.c` -- Flow profile management
- `kernel/drivers/net/ethernet/intel/ice/ice_switch.h` -- Switch recipe structures
- `kernel/drivers/net/ethernet/intel/ice/ice_switch.c` -- Recipe/filter programming
- `kernel/drivers/net/ethernet/intel/ice/ice_eswitch.c` -- eSwitch mode control
- `kernel/drivers/net/ethernet/intel/ice/ice_eswitch_br.h` -- Bridge offload structs
- `kernel/drivers/net/ethernet/intel/ice/ice_eswitch_br.c` -- Switchdev bridge offload
- `kernel/drivers/net/ethernet/intel/ice/ice_tc_lib.c` -- TC flower offload
- `kernel/drivers/net/ethernet/intel/ice/ice_ethtool_fdir.c` -- Ethtool FDIR interface
