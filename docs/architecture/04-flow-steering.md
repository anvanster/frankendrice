<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Architecture 04: Flow Steering

**References**: Exploration 04 (Flow Steering), Exploration 14 (ADQ),
Exploration 15 (Switchdev Mode)

## Design Philosophy

FrankenDrICE flow steering adopts mlx5's **tree-based flow steering
architecture** as the software abstraction layer above the E810 Admin Queue
boundary, while mapping all hardware operations to E810's recipe-based flat
matching system through a command vtable. This provides hierarchical
organization, clean lifetime management, and namespace isolation -- none of
which E810 hardware provides natively -- while preserving full compatibility
with the E810 switch rule and Flow Director mechanisms.

The core insight is that mlx5's `fs_node` tree infrastructure is a **pure
software pattern** that adds no hardware dependency. It provides refcounted
lifetime management, per-node locking, and uniform traversal for all flow
steering objects. E810's flat recipe array and filter lists are then managed
by the command vtable implementation, hidden behind the tree abstraction.

## Tree-Based Flow Steering: The fdice_fs_node Base Type

### Node Hierarchy

Every flow steering object in FrankenDrICE embeds `struct fdice_fs_node` as
its base type, forming a tree with five levels:

```
fdice_flow_steering (root container)
  |
  +-- fdice_flow_namespace (traffic domain grouping)
  |     |
  |     +-- fdice_flow_priority (ordered priority level)
  |           |
  |           +-- fdice_flow_table (collection of groups)
  |                 |
  |                 +-- fdice_flow_group (shared match criteria/recipe)
  |                       |
  |                       +-- fdice_flow_entry (individual match+action rule)
```

### fdice_fs_node Base Type

```c
/**
 * struct fdice_fs_node - Base type for all flow steering tree nodes
 *
 * Provides unified lifetime management, locking, and tree navigation
 * for all flow steering objects. Inspired by mlx5's fs_node pattern,
 * adapted for E810 hardware constraints.
 */
struct fdice_fs_node {
    struct list_head children;        /* child node list */
    struct list_head sibling;         /* linkage in parent's children list */
    struct fdice_fs_node *parent;     /* parent node (NULL for root) */
    struct fdice_fs_node *root;       /* root namespace node */
    enum fdice_fs_node_type type;     /* node type discriminator */

    struct rw_semaphore lock;         /* per-node read/write lock */
    refcount_t refcount;              /* reference count for lifetime */

    /* Virtual destructors -- called during node removal */
    void (*del_hw_func)(struct fdice_fs_node *);  /* remove from hardware */
    void (*del_sw_func)(struct fdice_fs_node *);  /* free software state */
};

enum fdice_fs_node_type {
    FDICE_FS_TYPE_NAMESPACE,
    FDICE_FS_TYPE_PRIORITY,
    FDICE_FS_TYPE_FLOW_TABLE,
    FDICE_FS_TYPE_FLOW_GROUP,
    FDICE_FS_TYPE_FLOW_ENTRY,
};
```

### Lifetime Management

Nodes follow a strict protocol:

1. **Creation**: Parent holds write lock, child is initialized and added to
   parent's children list. Refcount starts at 1.
2. **Reference**: Any code holding a pointer to a node must hold a reference
   via `fdice_fs_node_get()`. Tree traversal acquires read locks on ancestors.
3. **Removal**: `fdice_fs_node_remove()` calls `del_hw_func` (remove from E810
   hardware via AQ), then `del_sw_func` (free memory). Only proceeds when
   refcount reaches zero.
4. **Cascade**: Removing a parent cascades removal to all children, bottom-up.

This eliminates the class of use-after-free bugs present in ice's unprotected
filter list management.

## struct fdice_flow_cmds -- Command Vtable

The command vtable abstracts all hardware interactions, mapping tree operations
to E810 Admin Queue commands. This is the **bridge between the software tree
and E810 hardware**.

```c
/**
 * struct fdice_flow_cmds - Hardware command interface for flow steering
 *
 * Single implementation for E810 firmware-managed steering via AQ.
 * The vtable pattern is retained (from mlx5's multi-backend design)
 * for testability -- a mock implementation enables unit testing of
 * the tree logic without hardware.
 */
struct fdice_flow_cmds {
    /* Flow table operations */
    int (*create_flow_table)(struct fdice_flow_table *ft,
                             struct fdice_flow_namespace *ns);
    int (*destroy_flow_table)(struct fdice_flow_table *ft);

    /* Flow group operations -- maps to E810 recipe management */
    int (*create_flow_group)(struct fdice_flow_group *fg,
                             struct fdice_flow_table *ft);
    int (*destroy_flow_group)(struct fdice_flow_group *fg);

    /* Flow entry operations -- maps to E810 switch rules */
    int (*create_flow_entry)(struct fdice_flow_entry *fe,
                             struct fdice_flow_group *fg);
    int (*update_flow_entry)(struct fdice_flow_entry *fe);
    int (*delete_flow_entry)(struct fdice_flow_entry *fe);

    /* FDIR-specific operations */
    int (*create_fdir_entry)(struct fdice_flow_entry *fe,
                             struct fdice_fdir_params *params);
    int (*delete_fdir_entry)(struct fdice_flow_entry *fe);
};
```

### E810 AQ Command Mapping

| Tree Operation | E810 AQ Command | Notes |
|----------------|-----------------|-------|
| create_flow_group | add_recipe (0x0290) | Match criteria become extraction words |
| destroy_flow_group | (recipe persists, ref-counted) | Recipes freed when last group drops |
| create_flow_entry | add_sw_rules (0x02A0) | Switch forwarding rule |
| update_flow_entry | update_sw_rules (0x02A1) | In-place rule update |
| delete_flow_entry | remove_sw_rules (0x02A2) | Rule removal |
| create_fdir_entry | (via FDIR AQ path) | Dummy packet programming |
| create_flow_table | (software only) | No direct HW table concept |
| destroy_flow_table | (software only, cascades entries) | Cleanup all child rules |

## Namespace Separation for Traffic Domains

FrankenDrICE maintains separate root namespaces for different traffic domains,
preventing rule conflicts and simplifying management. Each namespace has its
own command vtable pointer, allowing domain-specific command routing.

### Root Namespaces

```c
struct fdice_flow_steering {
    struct fdice_dev *dev;

    /* Root namespaces -- one per traffic domain */
    struct fdice_flow_namespace *nic_rx_ns;      /* NIC receive steering */
    struct fdice_flow_namespace *nic_tx_ns;      /* NIC transmit steering */
    struct fdice_flow_namespace *fdb_ns;          /* eSwitch FDB */
    struct fdice_flow_namespace *rdma_rx_ns;      /* RDMA receive (future) */

    /* Command vtable (single implementation for E810) */
    const struct fdice_flow_cmds *cmds;

    /* Recipe manager (shared across namespaces) */
    struct fdice_recipe_mgr *recipe_mgr;
};
```

### NIC RX Namespace Priority Pipeline

Within the NIC RX namespace, flow tables are organized into a priority-based
pipeline that mirrors mlx5e's organization, adapted for E810:

```
NIC RX Namespace
  |
  +-- Priority 0: TC Offload
  |     +-- Flow Table: TC flower hardware-offloaded rules
  |
  +-- Priority 1: FDIR
  |     +-- Flow Table: Flow Director exact-match rules
  |
  +-- Priority 2: ADQ Channel Steering
  |     +-- Flow Table: Per-TC channel steering rules
  |
  +-- Priority 3: VLAN Filtering
  |     +-- Flow Table: VLAN ID match rules
  |
  +-- Priority 4: L2 MAC Filtering
  |     +-- Flow Table: Unicast/multicast MAC rules
  |
  +-- Priority 5: RSS/Default
  |     +-- Flow Table: RSS hash-based distribution (default path)
```

Priorities are enforced in software. When a packet arrives, the E810 hardware
evaluates rules based on recipe priority values that the driver programs to
match this logical ordering. Higher-priority tables shadow lower ones.

### FDB Namespace (eSwitch)

```
FDB Namespace
  |
  +-- Priority 0: TC Flower on Representors
  |     +-- Flow Table: Hardware-offloadable repr rules
  |
  +-- Priority 1: Bridge FDB
  |     +-- Flow Table: Learned MAC forwarding rules
  |
  +-- Priority 2: Default Forwarding
        +-- Flow Table: Uplink-to-VF default path
```

## Recipe Management: Flow Groups to E810 Recipes

### The Translation Layer

E810 hardware uses **switch recipes** to define packet field extraction
patterns. Each recipe specifies protocol ID + offset pairs that tell the
hardware which bytes to extract and match. FrankenDrICE maps the flow group
abstraction to E810 recipes through `fdice_recipe_mgr`.

### fdice_flow_group

```c
/**
 * struct fdice_flow_group - Collection of entries sharing match criteria
 *
 * Each flow group has a specific match mask (which fields to compare).
 * This maps to an E810 switch recipe that defines the extraction pattern.
 * Multiple flow groups can share the same recipe if their match masks
 * are identical.
 */
struct fdice_flow_group {
    struct fdice_fs_node node;         /* base type */

    /* Match criteria -- defines WHAT fields to match */
    struct fdice_match_criteria mask;
    u32 match_criteria_enable;         /* bitmask of active criteria */

    /* E810 recipe binding */
    u16 recipe_id;                     /* bound E810 recipe, or 0 if pending */
    struct fdice_recipe *recipe;       /* recipe reference (shared) */

    /* Entry management */
    u32 max_entries;
    u32 num_entries;
    struct ida entry_ida;              /* entry index allocator */

    /* Autogroup support */
    bool is_autogroup;                 /* created by autogroup mechanism */
};
```

### fdice_recipe_mgr

```c
/**
 * struct fdice_recipe_mgr - E810 recipe lifecycle manager
 *
 * Manages the limited set of E810 switch recipes (max 64).
 * Implements recipe sharing: when two flow groups need the same
 * extraction pattern, they bind to the same recipe rather than
 * allocating a new one.
 */
struct fdice_recipe_mgr {
    struct fdice_dev *dev;
    struct mutex lock;

    /* Recipe registry -- indexed by recipe ID */
    struct fdice_recipe recipes[FDICE_MAX_RECIPES];

    /* Match-to-recipe hash for O(1) sharing lookup */
    DECLARE_HASHTABLE(match_ht, 6);    /* 64 buckets */

    /* Allocation tracking */
    DECLARE_BITMAP(used_recipes, FDICE_MAX_RECIPES);
    u16 num_used;
};

struct fdice_recipe {
    u16 recipe_id;
    refcount_t refcount;               /* shared across flow groups */
    struct fdice_match_criteria mask;   /* extraction pattern */
    struct hlist_node hash_node;        /* in match_ht */
    bool hw_programmed;                /* true if sent to FW via AQ */
};
```

### Recipe Sharing Protocol

1. `fdice_flow_group_create()` computes a hash of the match criteria.
2. Looks up `recipe_mgr->match_ht` for an existing recipe with identical mask.
3. If found: increments recipe refcount, binds group to existing recipe.
4. If not found: allocates a new recipe ID from `used_recipes` bitmap,
   programs the recipe via AQ (`add_recipe` opcode 0x0290), inserts into hash.
5. On group destruction: decrements recipe refcount. When refcount reaches zero,
   the recipe is freed (but the HW recipe slot is retained for reuse).

This avoids the ice problem of redundant recipe allocation when multiple
subsystems (ADQ, FDIR, switchdev) need the same match pattern.

## FDIR Integration Behind Structured Match API

Ice's Flow Director requires callers to understand dummy-packet programming --
building a literal packet template that the hardware matches against. This
leaks hardware implementation details into higher-level code.

FrankenDrICE wraps FDIR behind the same `fdice_flow_entry` abstraction used
for all flow steering rules. The caller specifies structured match criteria;
the command vtable implementation translates to dummy-packet format internally.

### fdice_flow_entry

```c
/**
 * struct fdice_flow_entry - Individual match+action flow rule
 *
 * Represents a single hardware flow rule. Can be backed by either
 * a switch rule (via add_sw_rules AQ) or a FDIR filter (via FDIR
 * AQ path), depending on the parent flow table's type.
 */
struct fdice_flow_entry {
    struct fdice_fs_node node;         /* base type */

    /* Match specification -- WHAT to match */
    struct fdice_match_values match;

    /* Action specification -- WHAT to do */
    struct fdice_flow_action action;

    /* Hardware binding */
    union {
        struct {
            u16 rule_id;               /* switch rule ID from AQ */
            u16 recipe_id;             /* recipe used */
        } sw_rule;
        struct {
            u32 fltr_id;              /* FDIR filter ID */
            u16 q_index;              /* target queue index */
        } fdir;
    } hw;

    /* Tracking */
    u64 cookie;                        /* TC flower cookie / user handle */
    u64 packets;                       /* matched packet counter */
    u64 bytes;                         /* matched byte counter */
};
```

### Match Criteria and Values

```c
/**
 * struct fdice_match_criteria - Bitmask defining which fields to match
 *
 * Used in flow groups to define the match pattern (mask).
 * Maps to E810 recipe extraction words.
 */
struct fdice_match_criteria {
    u32 outer_headers;                 /* FDICE_MATCH_* flags for L2-L4 */
    u32 inner_headers;                 /* tunnel inner headers */
    u32 misc;                          /* miscellaneous match fields */
};

/**
 * struct fdice_match_values - Concrete field values for a flow entry
 *
 * Combined with the parent group's match criteria to form a complete rule.
 */
struct fdice_match_values {
    /* L2 */
    u8 dmac[ETH_ALEN];
    u8 smac[ETH_ALEN];
    __be16 ethertype;
    u16 vlan_id;
    u8 vlan_prio;

    /* L3 */
    union {
        __be32 ipv4_src;
        struct in6_addr ipv6_src;
    };
    union {
        __be32 ipv4_dst;
        struct in6_addr ipv6_dst;
    };
    u8 ip_proto;
    u8 tos;
    u8 ttl;

    /* L4 */
    __be16 sport;
    __be16 dport;

    /* Tunnel */
    __be32 tunnel_id;                  /* VXLAN VNI, GRE key, GTP TEID */
    enum fdice_tunnel_type tun_type;
};

struct fdice_flow_action {
    enum fdice_flow_action_type type;  /* FORWARD, DROP, MARK, COUNT */
    union {
        struct {
            u16 dest_vsi;              /* forward to VSI */
            u16 dest_queue;            /* specific queue within VSI */
        } forward;
        u32 flow_mark;                 /* mark value for FDIR */
    };
};
```

## Software Priority Pipeline

E810 hardware does not support table chaining -- there is no "miss action"
that forwards unmatched packets from one table to the next. All rules are
evaluated based on recipe priority values.

FrankenDrICE implements a **software priority pipeline** that maps the
logical table hierarchy to E810 recipe priorities:

### Priority Mapping

```
Logical Priority    E810 Recipe Priority    Domain
-----------------   --------------------    ------
0 (highest)         0                       TC Offload
1                   1                       FDIR
2                   2                       ADQ Channel
3                   3                       VLAN Filter
4                   4                       L2 MAC
5 (lowest)          5                       RSS Default
```

When a flow entry is created, the command vtable implementation reads the
priority from the entry's ancestor priority node and programs the corresponding
E810 recipe priority. This ensures that TC offload rules take precedence over
FDIR rules, which take precedence over VLAN filters, and so on.

### Limitation Acknowledgment

Unlike mlx5 where an unmatched packet genuinely "falls through" from one
hardware table to the next, E810 evaluates all matching recipes in parallel
and selects the highest-priority match. This means:

- Overlapping rules across priority levels may produce different behavior than
  mlx5's sequential evaluation.
- The software must ensure that higher-priority rules are strict supersets of
  lower-priority rules to avoid ambiguity.
- FDIR and switch rules occupy separate hardware namespaces; the priority
  ordering between them is enforced by recipe priority values.

## Switchdev FDB Integration

The FDB namespace integrates with the eswitch subsystem (Architecture 06 --
Switchdev, future document). Flow steering provides the infrastructure;
the eswitch module populates it.

### Bridge FDB Flow

```
switchdev FDB notification
  -> fdice_esw_br_fdb_event()
    -> fdice_flow_entry_create() in FDB namespace, Bridge FDB priority
      -> fdice_flow_cmds->create_flow_entry()
        -> AQ add_sw_rules with MAC match, forward-to-VSI action
    -> fdice_flow_entry_create() (guard rule)
```

Each learned MAC address creates two entries in the FDB flow table:
1. **Forward rule**: Match destination MAC, forward to the correct VF/SF VSI.
2. **Guard rule**: Prevent flooding for known MACs.

### TC Flower on Representors

TC flower rules on representor netdevs flow through the same tree:

```
ndo_setup_tc(TC_SETUP_BLOCK) on repr netdev
  -> fdice_repr_add_flower()
    -> fdice_flow_entry_create() in FDB namespace, TC Flower priority
      -> match criteria parsed from cls_flower
      -> action: forward to target VSI (uplink or VF)
    -> fdice_flow_cmds->create_flow_entry()
```

This unifies the filter infrastructure across ADQ, switchdev, and FDIR paths,
eliminating ice's separate code paths for each subsystem.

## Key Structures Summary

| Structure | Role |
|-----------|------|
| `fdice_fs_node` | Base type for all tree nodes: lifetime, locking, traversal |
| `fdice_flow_steering` | Root container with namespace pointers and recipe manager |
| `fdice_flow_namespace` | Traffic domain grouping (NIC RX, NIC TX, FDB, RDMA) |
| `fdice_flow_priority` | Ordered priority level within a namespace |
| `fdice_flow_table` | Collection of flow groups at a specific priority |
| `fdice_flow_group` | Match criteria grouping, bound to E810 recipe |
| `fdice_flow_entry` | Individual match+action rule (switch rule or FDIR filter) |
| `fdice_flow_cmds` | Command vtable mapping tree ops to E810 AQ commands |
| `fdice_recipe_mgr` | E810 recipe lifecycle, sharing, and allocation |
| `fdice_match_criteria` | Bitmask defining which packet fields to match |
| `fdice_match_values` | Concrete field values for a specific rule |
| `fdice_flow_action` | Action specification (forward, drop, mark, count) |

## Module Layout

```
src/flow/
  fdice_fs_core.c      -- Tree infrastructure, namespace init, node lifecycle
  fdice_fs_core.h      -- fdice_fs_node, namespace, priority, table structs
  fdice_fs_cmd.c       -- AQ command vtable implementation for E810
  fdice_fs_cmd.h       -- fdice_flow_cmds definition
  fdice_fs_recipe.c    -- Recipe manager, sharing, allocation
  fdice_fs_fdir.c      -- FDIR wrapper (structured API over dummy packets)
  fdice_fs_tc.c        -- TC flower offload integration
  fdice_fs_ttc.c       -- Type-to-queue classification (RSS steering)
```

## Key Improvements Over Ice

| Aspect | ice | fdice_ |
|--------|-----|--------|
| Organization | Flat recipe arrays, filter linked lists | Hierarchical tree with refcounted nodes |
| Lifetime management | Manual, error-prone | Automatic via fdice_fs_node refcount + destructors |
| Locking | Global switch lock | Per-node rw_semaphore for fine-grained concurrency |
| Recipe management | Ad-hoc, potential duplication | Centralized manager with hash-based sharing |
| FDIR API | Callers must understand dummy packets | Structured match criteria, internal translation |
| Namespace isolation | Single switch, cross-domain conflicts possible | Separate namespaces prevent rule collisions |
| TC flower path | Separate code for PF, ADQ, repr | Unified tree insertion for all callers |
| Command abstraction | Direct AQ calls scattered across files | Single vtable, testable with mock implementation |
