<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Architecture 03: RDMA Integration

**References**: Exploration 05 (RDMA Integration Model), Exploration 06 (SR-IOV/VF Management)

## Design Philosophy

FrankenDrICE RDMA integration uses the **auxiliary bus** with a **loose coupling
model** derived from ice/irdma's IIDC pattern, enhanced with mlx5-inspired
improvements in capability passing, event semantics, and interrupt management.
The E810 hardware naturally enforces this separation: RDMA has its own MMIO
register space and CQP (Control Queue Pair), while all shared resource
management (VSI config, QSet TEIDs, vector allocation) must go through the PF
driver's Admin Queue.

The key architectural improvement over both reference drivers is replacing
exported GPL symbols with an **ops vtable** (`struct fdice_iidc_ops`) passed at
probe time. This provides compile-time interface safety, eliminates symbol
version coupling between modules, and enables clean testing with mock
implementations.

## Auxiliary Bus Model

### Device Registration

The PF driver creates a single RDMA auxiliary device per physical function,
registered after queue infrastructure is operational:

```
fdice_load()
  -> fdice_rdma_init()
       -> fdice_iidc_core_init()      -- allocate iidc_core, populate caps
       -> fdice_rdma_aux_register()   -- auxiliary_device_init + add
```

The auxiliary device name is protocol-dependent based on capability discovery:
`"fdice.roce"` or `"fdice.iwarp"`. The RDMA provider driver (a separate module)
probes on this auxiliary device, receives the `fdice_iidc_core` structure, and
operates independently through the ops vtable.

### Dynamic Lifecycle Management

Inspired by mlx5's `mlx5_rescan_drivers_locked()` mechanism, fdice_ supports
dynamic auxiliary device teardown and recreation during:

- eSwitch mode transitions
- Devlink reload
- PF-level reset recovery
- DCB TC reconfiguration (when RDMA must be quiesced)

Each auxiliary device type has gating callbacks:

```c
struct fdice_adev_type {
    const char *suffix;                          /* "roce", "iwarp" */
    bool (*is_supported)(struct fdice_dev *dev);  /* HW capability check */
    bool (*is_enabled)(struct fdice_dev *dev);    /* Runtime enable check */
};
```

The `is_supported` callback checks hardware capabilities discovered at probe.
The `is_enabled` callback checks runtime state (devlink params like
`enable_roce`, `enable_iw`). Auxiliary device creation only proceeds when both
return true.

### Unplug and Replug Sequence

```
fdice_rdma_aux_unregister()
  -> auxiliary_device_delete()    -- triggers RDMA driver remove callback
  -> auxiliary_device_uninit()    -- releases auxiliary device resources
  -> fdice_iidc_core_update()    -- refresh capability snapshot

fdice_rdma_aux_register()
  -> fdice_iidc_core_populate()  -- rebuild capability data
  -> auxiliary_device_init()
  -> auxiliary_device_add()      -- triggers RDMA driver probe callback
```

## struct fdice_iidc_ops -- The Ops Vtable

Instead of ice/irdma's pattern of exporting individual GPL symbols
(`ice_add_rdma_qset`, `ice_del_rdma_qset`, etc.), fdice_ passes a const
pointer to an ops vtable at probe time. This is the single most important
architectural improvement over the IIDC model.

### Advantages Over Exported Symbols

1. **No symbol version coupling**: The RDMA module does not link against PF
   driver symbols. Module updates are independent as long as the ops struct
   layout is stable.
2. **Compile-time type safety**: Function pointer signatures are checked at
   compile time. Exported symbols rely on header synchronization.
3. **Testability**: Mock implementations can be injected for unit testing the
   RDMA driver without loading the full PF driver.
4. **Discoverability**: All PF-provided operations are visible in one struct
   definition rather than scattered across multiple source files.

### Vtable Definition

```c
/**
 * struct fdice_iidc_ops - Operations provided by PF driver to RDMA driver
 *
 * All operations are synchronous unless documented otherwise.
 * The caller must not hold any PF-internal locks when calling these.
 * All functions return 0 on success, negative errno on failure.
 */
struct fdice_iidc_ops {
    /* QSet management -- issues AQ commands on behalf of RDMA */
    int (*add_qset)(struct fdice_iidc_core *core,
                    struct fdice_rdma_qset_params *params);
    int (*del_qset)(struct fdice_iidc_core *core,
                    struct fdice_rdma_qset_params *params);

    /* Reset request -- RDMA driver requests PF-level or core reset */
    int (*request_reset)(struct fdice_iidc_core *core,
                         enum fdice_reset_type type);

    /* VSI filter control -- enable/disable RDMA packet filtering */
    int (*update_vsi_filter)(struct fdice_iidc_core *core,
                             u16 vsi_id, bool enable);

    /* Interrupt vector management */
    int (*alloc_vectors)(struct fdice_iidc_core *core,
                         u16 num_vecs, struct msix_entry *entries);
    void (*free_vectors)(struct fdice_iidc_core *core,
                         u16 num_vecs, struct msix_entry *entries);

    /* Event registration -- RDMA driver registers for notifications */
    int (*register_event_handler)(struct fdice_iidc_core *core,
                                  struct fdice_iidc_event_handler *handler);
    void (*unregister_event_handler)(struct fdice_iidc_core *core,
                                     struct fdice_iidc_event_handler *handler);
};
```

## Probe-Time Capability Snapshot

Ice's IIDC passes minimal data at probe: PCI device, MMIO base, PF ID, netdev,
and basic QoS info. This forces the RDMA driver to make runtime queries for
resource limits and feature flags.

FrankenDrICE passes a **rich capability snapshot** at probe time through
`struct fdice_iidc_core`, enabling the RDMA driver to make optimal resource
sizing decisions without any runtime round-trips to the PF driver.

### fdice_iidc_core

```c
/**
 * struct fdice_iidc_core - Complete RDMA integration context
 *
 * Passed to the RDMA auxiliary driver at probe time. The PF driver
 * owns this structure; the RDMA driver holds a const reference.
 * Updated atomically during replug cycles.
 */
struct fdice_iidc_core {
    /* Identity */
    struct pci_dev *pdev;
    struct net_device *netdev;
    void __iomem *rdma_hw_addr;       /* RDMA-specific MMIO base */
    u16 pf_id;
    u16 vport_id;

    /* Protocol selection */
    enum fdice_rdma_protocol protocol; /* FDICE_RDMA_IWARP, FDICE_RDMA_ROCE */

    /* Capability snapshot -- richer than IIDC */
    struct fdice_rdma_caps caps;

    /* QoS snapshot */
    struct fdice_rdma_qos qos;

    /* Operations vtable */
    const struct fdice_iidc_ops *ops;

    /* Opaque PF driver private -- RDMA driver must not dereference */
    void *pf_priv;
};
```

### fdice_rdma_caps

```c
/**
 * struct fdice_rdma_caps - RDMA capability snapshot from HW discovery
 *
 * Populated during fdice_init_once() from AQ capability queries.
 * Immutable after probe; updated only during replug.
 */
struct fdice_rdma_caps {
    u32 max_qps;
    u32 max_cqs;
    u32 max_mrs;
    u32 max_ceqs;               /* Completion Event Queues */
    u32 max_pd;                 /* Protection Domains */
    u32 max_srqs;               /* Shared Receive Queues */
    u16 hw_rev;                 /* E810 stepping */
    u32 features;               /* FDICE_RDMA_FEAT_* flags */
    u16 max_mtu;
    u16 rdma_msix_allocated;    /* Pre-partitioned vectors */
};
```

### fdice_rdma_qos

```c
/**
 * struct fdice_rdma_qos - QoS configuration snapshot for RDMA
 *
 * Updated and re-delivered to RDMA driver on TC change events.
 */
struct fdice_rdma_qos {
    u8 num_tc;
    u8 up_to_tc_map[8];         /* User Priority to TC mapping */
    u8 tc_to_bw_map[8];         /* TC to relative bandwidth */
    u8 pfc_enabled;             /* PFC enable bitmask */
    bool dscp_mode;             /* true = DSCP QoS, false = VLAN PCP */
    u8 dscp_map[64];            /* DSCP to UP mapping (when dscp_mode) */
};
```

## Event Notification: Synchronous Pre/Post Semantics

Ice/irdma uses simple asynchronous event bitmaps
(`IIDC_RDMA_EVENT_BEFORE_TC_CHANGE`, `IIDC_RDMA_EVENT_AFTER_TC_CHANGE`). This
creates a window for state inconsistency -- the RDMA driver may not have
finished quiescing when the PF begins reconfiguration.

FrankenDrICE uses **synchronous pre/post event semantics** with explicit
acknowledgment, inspired by mlx5's tighter coordination model.

### Event Types

```c
enum fdice_iidc_event_type {
    /* TC/QoS changes */
    FDICE_IIDC_EVENT_PRE_TC_CHANGE,    /* Quiesce RDMA traffic */
    FDICE_IIDC_EVENT_POST_TC_CHANGE,   /* Resume with new QoS */

    /* Reset events */
    FDICE_IIDC_EVENT_PRE_RESET,        /* Save state, stop ops */
    FDICE_IIDC_EVENT_POST_RESET,       /* Restore state, restart */

    /* Link events */
    FDICE_IIDC_EVENT_MTU_CHANGE,       /* MTU changed */
    FDICE_IIDC_EVENT_LINK_DOWN,        /* Link lost */
    FDICE_IIDC_EVENT_LINK_UP,          /* Link restored */

    /* Critical errors */
    FDICE_IIDC_EVENT_CRITICAL_ERROR,   /* Unrecoverable HW error */
};
```

### Event Handler Registration

```c
struct fdice_iidc_event_handler {
    /**
     * @handle_event: Synchronous event callback
     *
     * For PRE_* events: return 0 to acknowledge readiness, or -EBUSY
     * to request a delay. The PF driver will retry after a brief
     * backoff (up to a configurable timeout before proceeding anyway).
     *
     * For POST_* events: return value is logged but does not block
     * the PF driver.
     */
    int (*handle_event)(struct fdice_iidc_core *core,
                        enum fdice_iidc_event_type event,
                        void *event_data);
    void *priv;
    struct list_head list;       /* internal linkage */
};
```

### Pre/Post Event Flow

```
TC Change Request (e.g., DCB reconfiguration):

1. PF calls handler->handle_event(PRE_TC_CHANGE, &old_qos)
   RDMA driver: suspends QPs, drains completions, returns 0
2. PF reconfigures TCs (VSI rebuild, scheduler update)
3. PF updates fdice_rdma_qos in fdice_iidc_core
4. PF calls handler->handle_event(POST_TC_CHANGE, &new_qos)
   RDMA driver: reads new QoS, reconfigures QP-to-TC mapping, resumes
```

The synchronous model eliminates the race window present in ice/irdma where
the RDMA driver might still be processing traffic during the reconfiguration
window.

## Interrupt Pool with RDMA Pre-Partitioning

E810 has a fixed MSI-X table shared between networking and RDMA. Ice proxies
every vector allocation through `ice_alloc_rdma_qvector()`/
`ice_free_rdma_qvector()`, adding overhead and creating a serialization point.

FrankenDrICE pre-partitions the interrupt pool at initialization time, giving
the RDMA subsystem a dedicated range that it can manage independently.

### Design

```
Total MSI-X vectors (from PCI capability)
  |
  +-- fdice_irq_pool (managed by PF driver)
       |
       +-- Networking partition
       |     +-- Async/misc vectors (1-2)
       |     +-- Completion EQ vectors (per-CPU)
       |
       +-- RDMA partition (pre-allocated block)
       |     +-- CEQ vectors (reported in fdice_rdma_caps.rdma_msix_allocated)
       |     +-- AEQ vector (1)
       |
       +-- Reserve pool (for dynamic rebalancing)
```

### Allocation Strategy

1. At `fdice_init_once()`, the driver queries total MSI-X capacity and RDMA
   capability flags.
2. If RDMA is supported, a configurable fraction (default: 1/3) of vectors
   is reserved for the RDMA partition.
3. The RDMA partition size is reported in `fdice_rdma_caps.rdma_msix_allocated`.
4. The RDMA driver allocates vectors from its partition via `ops->alloc_vectors()`,
   which performs a simple bitmap allocation within the pre-partitioned range
   (no AQ commands, no PF driver locks in the allocation path).
5. Dynamic rebalancing: If the RDMA driver is not loaded, the networking stack
   can temporarily reclaim RDMA-partition vectors from the reserve pool.

### Advantages Over Ice

- **No per-vector AQ round-trip**: Vector allocation is a bitmap operation, not
  an Admin Queue command.
- **Predictable latency**: The RDMA driver knows its vector budget at probe time.
- **Dynamic rebalancing**: Unused RDMA vectors are not wasted when RDMA is idle.

## Integration with Queue Hierarchy

### Shared CQ/EQ Potential

E810's RDMA subsystem manages its own Completion Event Queues (CEQs) and Async
Event Queue (AEQ) through the CQP (Control Queue Pair) in the RDMA MMIO space.
These are independent of the networking EQ/CQ infrastructure.

However, the interrupt vectors are shared at the MSI-X level. The pre-partitioned
pool ensures that RDMA CEQs and networking CQs do not contend for the same
interrupt vectors while still allowing coordinated CPU affinity management.

Future optimization: If both networking and RDMA completions target the same CPU,
a shared EQ could multiplex both completion types through a single MSI-X vector,
reducing interrupt overhead. The `fdice_irq_pool` abstraction supports this by
allowing a single vector to be registered with both a networking CQ callback and
an RDMA CEQ callback.

### Queue Set (QSet) Management

RDMA queue sets require AQ commands that only the PF driver can issue
(`add_rdma_qset` opcode 0x0C33). The ops vtable provides this through
`ops->add_qset()` and `ops->del_qset()`, which wrap the AQ command with proper
serialization and error handling.

```c
struct fdice_rdma_qset_params {
    u16 vsi_id;
    u8 tc;                    /* Traffic class for this QSet */
    u16 num_qs;               /* Number of queue pairs */
    u32 teid;                 /* OUT: Scheduler TEID (filled by PF) */
    u16 qs_handle;            /* OUT: Queue set handle (filled by PF) */
};
```

## Key Structures Summary

| Structure | Role | Owner |
|-----------|------|-------|
| `fdice_rdma_dev` | PF-side RDMA subsystem state (auxiliary device, irq partition) | PF driver |
| `fdice_iidc_core` | Complete integration context passed to RDMA driver at probe | PF driver (RDMA holds const ref) |
| `fdice_iidc_ops` | Vtable of PF-provided operations for RDMA driver use | PF driver (static const) |
| `fdice_rdma_caps` | Hardware capability snapshot (immutable between replug cycles) | PF driver |
| `fdice_rdma_qos` | QoS configuration snapshot (updated on TC change events) | PF driver |
| `fdice_iidc_event_handler` | RDMA driver's event callback registration | RDMA driver |
| `fdice_rdma_qset_params` | Parameters for QSet add/delete AQ commands | Caller-allocated |

## Module Layout

```
src/rdma/
  fdice_rdma.c          -- PF-side RDMA subsystem init/cleanup
  fdice_rdma.h          -- fdice_rdma_dev, internal PF-side declarations
  fdice_iidc.c          -- Auxiliary device lifecycle, event dispatch
  fdice_iidc.h          -- fdice_iidc_core, fdice_iidc_ops, fdice_rdma_caps
                           (public header consumed by RDMA provider module)
```

The RDMA provider module (equivalent to irdma) is a separate out-of-tree or
in-tree module that probes on the fdice auxiliary device using standard
`auxiliary_driver` registration.
