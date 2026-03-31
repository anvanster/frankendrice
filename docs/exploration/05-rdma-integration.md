<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 05: RDMA Integration Model -- mlx5 vs ice/irdma

## Summary

Both mlx5 and ice/irdma use the Linux auxiliary bus to expose RDMA functionality
as a separate kernel module. However, the coupling between the net driver and the
RDMA driver differs fundamentally. mlx5 shares its entire `mlx5_core_dev` struct
(including command interface, capabilities, EQ tables, and flow steering) directly
with mlx5_ib via a thin `mlx5_adev` wrapper. ice/irdma communicates through a
narrow, formalized IIDC (Inter-Driver Communication) interface that passes only
the minimal information needed: PCI device, MMIO base address, PF ID, VSI/vport
ID, netdev, QoS parameters, and RDMA protocol type.

## mlx5 RDMA Integration Model (Tight Coupling)

### Architecture

mlx5 core creates auxiliary devices for multiple subsystems via an `mlx5_adev_devices[]`
table indexed by protocol type. The RDMA device is registered as suffix `"rdma"`
under `MLX5_INTERFACE_PROTOCOL_IB`:

```
mlx5_init_one_devl_locked()
  -> mlx5_register_device()
       -> mlx5_rescan_drivers_locked()
            -> add_drivers()  -- iterates mlx5_adev_devices[]
                 -> add_adev()  -- auxiliary_device_init + auxiliary_device_add
```

The `mlx5_adev` struct is minimal:

```c
struct mlx5_adev {
    struct auxiliary_device adev;
    struct mlx5_core_dev *mdev;   /* direct pointer to core device */
    int idx;
};
```

### Tight Coupling Mechanism

The RDMA driver (mlx5_ib, in `drivers/infiniband/hw/mlx5/`) gets access to the
full `mlx5_core_dev` via `container_of(adev, struct mlx5_adev, adev)->mdev`.
This gives mlx5_ib direct access to:

- **Command interface** (`dev->cmd`): Issue firmware commands directly
- **Capability cache** (`dev->caps`): Read HCA capabilities without AQ round-trips
- **EQ/IRQ tables** (via `dev->priv`): Share completion event queues with netdev
- **Flow steering**: Program hardware flow rules directly
- **MMIO registers** (`dev->iseg`): Direct hardware register access
- **Resource tracking**: Shared CQ number space, UAR pages, MR keys
- **RoCE state** (`dev->roce`): Shared GID table and RoCE enable state
- **Clock/timestamps** (`dev->clock_info`): Shared hardware clock

The `mlx5_core_dev` struct in `include/linux/mlx5/driver.h` is a public header,
meaning mlx5_ib can (and does) reach into nearly every subsystem of the core
driver. There is no abstraction layer -- mlx5_ib calls `mlx5_cmd_exec()` and
other core functions directly.

### Dynamic Device Management

mlx5 supports dynamic rescan: `mlx5_rescan_drivers_locked()` calls `delete_drivers()`
then `add_drivers()`, allowing auxiliary devices to be torn down and recreated
during eSwitch mode changes, LAG bonding, and devlink reload. Each auxiliary
device type has `is_supported()` and `is_enabled()` callbacks that gate creation.

## ice/irdma RDMA Integration Model (Loose Coupling via IIDC)

### Architecture

ice creates a single RDMA auxiliary device per PF in `ice_plug_aux_dev()`, called
from `ice_rdma_finalize_setup()` after VSI initialization:

```
ice_load()
  -> ice_rdma_finalize_setup()
       -> ice_plug_aux_dev()
            -> auxiliary_device_init + auxiliary_device_add
```

The auxiliary device name is protocol-dependent: `"roce"` or `"iwarp"`.

### IIDC Interface Contract

The IIDC layer defines a narrow, formalized interface in three headers:

**`iidc_rdma.h`** (base interface):
- `iidc_rdma_core_dev_info`: PCI dev, auxiliary device ptr, RDMA protocol, opaque priv pointer
- `iidc_rdma_core_auxiliary_dev`: wraps `auxiliary_device` + `cdev_info` pointer
- `iidc_rdma_core_auxiliary_drv`: wraps `auxiliary_driver` + `event_handler` callback
- Event types: MTU change, TC change, reset warning, critical error

**`iidc_rdma_ice.h`** (ice-specific operations):
- `ice_add_rdma_qset()` / `ice_del_rdma_qset()`: RDMA QSet management via AQ
- `ice_rdma_request_reset()`: Request PFR or CORER reset
- `ice_rdma_update_vsi_filter()`: Enable/disable RDMA packet filtering on VSI
- `ice_alloc_rdma_qvector()` / `ice_free_rdma_qvector()`: MSI-X vector allocation

**`iidc_rdma_priv_dev_info`** (data passed at probe):
- PF ID, vport ID, netdev pointer, QoS info, MMIO hw_addr

### Loose Coupling Mechanism

irdma receives only `iidc_rdma_core_dev_info` at probe time. It extracts:
- MMIO base address (for direct register access to RDMA-specific registers)
- PCI device (for DMA mapping)
- Netdev (for L2 information)
- PF/vport IDs (for resource identification)
- QoS parameters (for traffic class configuration)

irdma does NOT have access to:
- ice's Admin Queue command interface
- ice's internal VSI structures
- ice's flow steering tables
- ice's interrupt/vector management internals (only via alloc/free API)
- ice's capability cache

All resource management goes through exported GPL symbols (`EXPORT_SYMBOL_GPL`).
The ice driver handles all AQ commands on irdma's behalf.

### Data Flow at Probe

```
icrdma_probe(aux_dev)
  -> container_of -> iidc_rdma_core_auxiliary_dev -> cdev_info
  -> ib_alloc_device(irdma_device)
  -> icrdma_fill_device_info()  -- copies IIDC data into irdma structs
  -> icrdma_init_interrupts()   -- requests MSI-X vectors via IIDC API
  -> irdma_ctrl_init_hw()       -- initializes RDMA HW via MMIO
  -> irdma_rt_init_hw()         -- runtime HW setup
  -> irdma_ib_register_device() -- registers with IB core
  -> ice_rdma_update_vsi_filter() -- enables RDMA filtering via IIDC API
```

## Comparison: Pros and Cons

### mlx5 Tight Coupling

**Advantages:**
1. **Performance**: No indirection for command submission; mlx5_ib calls
   `mlx5_cmd_exec()` directly rather than going through an intermediary
2. **Resource sharing**: EQs, CQs, and UAR pages can be shared between net and
   RDMA paths seamlessly since both see the same objects
3. **Feature richness**: mlx5_ib can leverage any core capability (ODP, DevX,
   flow steering, eSwitch) without waiting for the net driver to expose new APIs
4. **Consistency**: Single source of truth for device state (capabilities, health,
   link state) shared atomically between net and RDMA
5. **Dynamic reconfiguration**: Rescan mechanism allows seamless mode transitions

**Disadvantages:**
1. **Fragility**: Changes to `mlx5_core_dev` can break mlx5_ib; internal struct
   layout becomes de facto ABI
2. **Module coupling**: mlx5_ib cannot work with any other net driver; it is
   permanently married to mlx5_core
3. **Testing complexity**: Cannot test mlx5_ib in isolation; requires full
   mlx5_core initialization
4. **Upgrade risk**: Core and IB modules must be version-matched; partial updates
   risk undefined behavior
5. **Code comprehension**: Understanding mlx5_ib requires understanding mlx5_core
   internals deeply

### ice/irdma Loose Coupling (IIDC)

**Advantages:**
1. **Clean separation**: irdma has zero knowledge of ice internals; clear boundary
2. **Multi-vendor potential**: irdma already supports ice (Gen2) and idpf (Gen3)
   via different `_if.c` probe functions with the same core
3. **Independent development**: ice and irdma teams can develop, test, and release
   independently as long as IIDC contract is maintained
4. **Stable interface**: IIDC changes are rare and deliberate; adding a new op
   is an explicit design decision
5. **Simpler reasoning**: irdma's HW access is self-contained via MMIO; it manages
   its own CQP (Control QP), CEQ (Completion Event Queue), AEQ (Async Event Queue)

**Disadvantages:**
1. **Duplicated infrastructure**: irdma manages its own interrupt/event queue
   subsystem instead of sharing with ice
2. **Limited integration**: Cannot easily share flow steering rules, packet
   classification, or scheduling between net and RDMA paths
3. **API bottleneck**: New features require IIDC API extensions; ice_add_rdma_qset
   is the only way to get QSet TEIDs
4. **Extra overhead**: Vector allocation goes through ice API rather than direct
   allocation; reset requests are proxied through ice
5. **State synchronization**: Events (MTU change, TC change) are delivered
   asynchronously; potential for state inconsistency during transitions

## E810 Hardware Constraints on RDMA

The E810 hardware architecture influences the design:

1. **Separate RDMA register space**: E810 exposes RDMA-specific MMIO registers
   that irdma accesses directly (hw_addr passed via IIDC). This is fundamentally
   different from mlx5 where RDMA and net share the same command interface.

2. **Admin Queue as boundary**: All E810 configuration goes through the Admin
   Queue (AQ). RDMA QSet creation requires AQ commands that only the PF driver
   can issue (ice_cfg_vsi_rdma, ice_ena_vsi_rdma_qset). This naturally enforces
   the intermediary pattern.

3. **MSI-X vector partitioning**: E810 has a fixed MSI-X table shared between
   net and RDMA. ice manages the allocation and provides vectors to irdma via
   the IIDC API (ice_alloc_rdma_qvector/ice_free_rdma_qvector).

4. **VSI-based isolation**: RDMA traffic flows through a VSI that the PF driver
   owns. RDMA filtering on the VSI must be managed by ice
   (ice_rdma_update_vsi_filter).

5. **CQP (Control QP)**: Unlike mlx5 where firmware commands go through a shared
   mailbox, E810 RDMA uses a CQP managed entirely within irdma for RDMA-specific
   resource management (QP, CQ, MR creation). This supports the loose coupling
   model -- irdma does not need ice's AQ for RDMA resource management.

## Recommendation for fdice_ RDMA Architecture

### Use a Layered Approach: Narrow Interface, Rich Data

The ice/irdma IIDC model is the correct starting point for fdice_, but with
targeted improvements inspired by mlx5's strengths:

1. **ADOPT ice/irdma's IIDC separation pattern**: Define a `struct fdice_rdma_info`
   interface that the RDMA auxiliary driver receives at probe. Keep the PF driver
   as the sole AQ command issuer for shared resources (VSI config, QSet TEIDs,
   vector allocation).

2. **IMPROVE over IIDC with richer initial data**: Pass more information at probe
   time than IIDC does. Include capability flags, max resource limits (QPs, CQs,
   MRs), and hardware generation info so the RDMA driver can make optimal
   decisions without runtime queries.

3. **ADOPT mlx5's rescan/dynamic reconfiguration**: Support auxiliary device
   teardown and recreation during mode changes. Define `is_supported()` and
   `is_enabled()` callbacks per auxiliary device type so the RDMA device can be
   gracefully removed during reset or reconfiguration.

4. **ADOPT mlx5's event model improvements**: Instead of IIDC's simple bitmap
   events, use a richer notification mechanism with pre/post semantics and
   synchronous handshake for critical transitions (reset, MTU change).

5. **KEEP E810's natural separation**: Since E810 RDMA has its own MMIO register
   space and CQP for resource management, the loose coupling is a natural fit.
   Do not try to force mlx5-style tight coupling where the hardware does not
   need it.

6. **IMPROVE interrupt sharing**: Design `fdice_irq_pool` (from exploration 02)
   to natively support RDMA vector allocation without the overhead of ice's
   proxied alloc/free pattern. Pre-partition vectors at init time with dynamic
   rebalancing.

### Proposed Interface

```
struct fdice_rdma_export {
    struct pci_dev *pdev;
    struct net_device *netdev;
    void __iomem *rdma_hw_addr;     /* RDMA MMIO base */
    u16 pf_id;
    u16 vport_id;
    u32 rdma_protocol;              /* iWARP, RoCEv2, or both */

    /* Richer than IIDC -- capability snapshot */
    struct fdice_rdma_caps {
        u32 max_qps;
        u32 max_cqs;
        u32 max_mrs;
        u32 max_ceqs;
        u16 hw_rev;
        u32 features;               /* Feature flags */
    } caps;

    /* QoS snapshot */
    struct fdice_rdma_qos qos;

    /* Operations provided by PF driver */
    const struct fdice_rdma_ops *ops;
};

struct fdice_rdma_ops {
    int (*add_qset)(struct fdice_rdma_export *, struct fdice_rdma_qset_params *);
    int (*del_qset)(struct fdice_rdma_export *, struct fdice_rdma_qset_params *);
    int (*request_reset)(struct fdice_rdma_export *, enum fdice_reset_type);
    int (*update_vsi_filter)(struct fdice_rdma_export *, u16 vsi_id, bool enable);
    int (*alloc_vector)(struct fdice_rdma_export *, struct msix_entry *);
    void (*free_vector)(struct fdice_rdma_export *, struct msix_entry *);
};
```

### Key Design Decisions

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Coupling model | Loose (IIDC-like) | E810 HW naturally separates RDMA MMIO; CQP is self-contained |
| Interface style | Ops struct + data snapshot | Cleaner than IIDC's exported symbols; supports future device types |
| Capability passing | At probe time | Avoids runtime queries; RDMA driver can size resources optimally |
| Event notification | Synchronous pre/post | Prevents state inconsistency during transitions |
| Interrupt management | Shared pool with RDMA partition | Better than proxied alloc; supports dynamic rebalancing |
| Auxiliary device lifecycle | Dynamic with rescan support | Enables graceful RDMA teardown during PF reconfiguration |

### File Structure

```
src/rdma/
  fdice_rdma_export.h    -- struct fdice_rdma_export, fdice_rdma_ops
  fdice_rdma_idc.c       -- auxiliary device creation/deletion, event dispatch
```

The RDMA provider driver itself (equivalent to irdma) would be a separate module
that probes on the fdice auxiliary device, similar to how irdma probes on ice's
auxiliary device.
