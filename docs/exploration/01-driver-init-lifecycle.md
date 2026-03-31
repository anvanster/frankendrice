<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 01: Driver Initialization Lifecycle

**Question**: How does mlx5 probe/init vs ice probe/init? What does mlx5 do that ice doesn't?

**Source files explored via CodeGraph**:
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/main.c` -- `probe_one()`, `mlx5_init_one()`, `mlx5_init_one_devl_locked()`, `mlx5_mdev_init()`, `mlx5_function_setup()`, `mlx5_function_open()`, `mlx5_init_once()`, `mlx5_load()`
- `kernel/drivers/net/ethernet/intel/ice/ice_main.c` -- `ice_probe()`, `ice_init_dev()`, `ice_init()`, `ice_load()`
- `kernel/drivers/net/ethernet/intel/ice/ice_common.c` -- `ice_init_hw()`

---

## mlx5 Initialization Flow

mlx5 uses a **deeply layered** architecture that separates PCI concerns, core device
initialization, firmware handshake, subsystem allocation, and runtime loading into
distinct, reusable phases. The PCI probe function (`probe_one`) is thin and delegates
almost everything.

### probe_one() (PCI probe callback)
```
probe_one(pdev, id)
  |-- mlx5_devlink_alloc()           # Allocate devlink + embedded mlx5_core_dev
  |-- Determine coredev_type (PF vs VF)
  |-- mlx5_adev_idx_alloc()          # Allocate auxiliary device index
  |-- mlx5_mdev_init(dev, prof_sel)  # Core device SW init (see below)
  |-- mlx5_pci_init(dev, pdev, id)   # PCI resource setup
  |-- mlx5_init_one(dev)             # Full device bring-up (see below)
  |-- mlx5_vhca_debugfs_init()       # VHCA debug filesystem
  |-- pci_save_state()
```

### mlx5_mdev_init() -- Core software structures
Initializes mutexes, locks, lists, debugfs root, then:
```
mlx5_mdev_init(dev, profile_idx)
  |-- mutex_init (intf_state_mutex, alloc_mutex, pgdir_mutex, etc.)
  |-- debugfs_create_dir()
  |-- mlx5_cmd_init()               # Command interface SW structures
  |-- mlx5_tout_init()              # Timeout management
  |-- mlx5_health_init()            # Health monitoring framework
  |-- mlx5_pagealloc_init()         # Page allocation framework
  |-- mlx5_adev_init()              # Auxiliary device init
  |-- mlx5_hca_caps_alloc()         # HCA capabilities allocation
  |-- mlx5_notifiers_init()         # Event notification system
  |-- ida_alloc (sw_vhca_id)        # Unique VHCA identifier
```

### mlx5_init_one() -> mlx5_init_one_devl_locked() -- Full device bring-up
Takes the devlink lock, then orchestrates firmware and subsystem initialization:
```
mlx5_init_one_devl_locked(dev)
  |-- mlx5_function_setup()          # FW handshake (see below)
  |-- mlx5_init_once()              # One-time SW subsystem allocation (see below)
  |-- mlx5_devlink_params_register() # Devlink parameter registration
  |-- mlx5_load()                   # Runtime subsystem activation (see below)
  |-- set_bit(MLX5_INTERFACE_STATE_UP)
  |-- mlx5_register_device()        # Register auxiliary devices (eth, rdma, vnet)
  |-- mlx5_crdump_enable()          # CR dump support
  |-- mlx5_hwmon_dev_register()     # Hardware monitoring
```

### mlx5_function_setup() -- Firmware handshake
```
mlx5_function_setup(dev, boot, timeout)
  |-- mlx5_function_enable()         # Enable function (see below)
  |-- mlx5_function_open()           # Open function (see below)
```

`mlx5_function_enable()`:
```
mlx5_function_enable(dev, boot, timeout)
  |-- wait_fw_init() [pre-init]     # Wait for FW to accept init segment config
  |-- mlx5_cmd_enable()             # Enable command interface
  |-- mlx5_tout_query_iseg()        # Query timeouts from init segment
  |-- wait_fw_init() [init]         # Wait for FW full initialization
  |-- mlx5_cmd_set_state(UP)
  |-- mlx5_core_enable_hca()        # Enable HCA via command interface
  |-- mlx5_start_health_poll()      # Start health polling
  |-- mlx5_core_set_issi()          # Set ISSI (init segment SW interface)
  |-- mlx5_satisfy_startup_pages()  # Provide boot pages to FW
  |-- mlx5_tout_query_dtor()        # Query destruction timeouts
```

`mlx5_function_open()`:
```
mlx5_function_open(dev)
  |-- set_hca_ctrl()                # Configure HCA control
  |-- set_hca_cap()                 # Set HCA capabilities
  |-- mlx5_satisfy_startup_pages()  # Provide init pages to FW
  |-- mlx5_cmd_init_hca()           # INIT_HCA command
  |-- mlx5_set_driver_version()     # Report driver version to FW
  |-- mlx5_query_hca_caps()         # Query full HCA capabilities
  |-- mlx5_start_health_fw_log_up() # Start FW log monitoring
```

### mlx5_init_once() -- One-time subsystem allocation
Allocates SW structures for all subsystems (called once per device lifetime):
```
mlx5_init_once(dev)
  |-- mlx5_devcom_register_device()  # Device communication registration
  |-- mlx5_query_board_id()
  |-- mlx5_irq_table_init()         # IRQ table SW structures
  |-- mlx5_eq_table_init()          # Event queue table SW structures
  |-- mlx5_fw_reset_init()          # FW reset handling
  |-- mlx5_cq_debugfs_init()
  |-- mlx5_init_reserved_gids()
  |-- mlx5_init_clock()             # PTP/hardware clock
  |-- mlx5_vxlan_create() / mlx5_geneve_create()  # Tunnel offload
  |-- mlx5_init_rl_table()          # Rate limiting
  |-- mlx5_mpfs_init()              # Multi-Physical Function Switch
  |-- mlx5_sriov_init()             # SR-IOV framework
  |-- mlx5_eswitch_init()           # eSwitch (switchdev)
  |-- mlx5_fpga_init()              # FPGA support
  |-- mlx5_vhca_event_init()        # VHCA event notification
  |-- mlx5_sf_hw_table_init()       # SubFunction HW table
  |-- mlx5_sf_table_init()          # SubFunction table
  |-- mlx5_fs_core_alloc()          # Flow steering allocation
  |-- mlx5_dm_create() / mlx5_st_create()  # Device memory, steering tables
  |-- mlx5_fw_tracer_create()       # FW tracer
  |-- mlx5_hv_vhca_create()         # Hyper-V VHCA
  |-- mlx5_rsc_dump_create()        # Resource dump
```

### mlx5_load() -- Runtime activation
Activates hardware resources (can be called on reload):
```
mlx5_load(dev)
  |-- mlx5_alloc_bfreg()            # BlueFlame register allocation
  |-- mlx5_events_start()           # Start event processing
  |-- mlx5_pagealloc_start()        # Start page allocation
  |-- mlx5_irq_table_create()       # Create IRQ entries
  |-- mlx5_eq_table_create()        # Create event queues
  |-- mlx5_clock_load()             # Activate hardware clock
  |-- mlx5_fw_tracer_init()         # Initialize FW tracer
  |-- mlx5_fw_reset_events_start()  # Start FW reset event handling
  |-- mlx5_hv_vhca_init()           # Hyper-V VHCA activation
  |-- mlx5_rsc_dump_init()          # Resource dump activation
  |-- mlx5_fpga_device_start()      # Start FPGA device
  |-- mlx5_fs_core_init()           # Initialize flow steering tables
  |-- mlx5_core_set_hca_defaults()  # Set HCA defaults
  |-- mlx5_vhca_event_start()       # Start VHCA events
  |-- mlx5_ec_init()                # Embedded CPU init
  |-- mlx5_lag_add_mdev()           # LAG (link aggregation) support
  |-- mlx5_sriov_attach()           # Attach SR-IOV
  |-- mlx5_sf_dev_table_create()    # SubFunction device table
  |-- mlx5_devlink_traps_register() # Devlink trap registration
```

### Key architectural patterns
1. **init vs load separation**: `mlx5_init_once` allocates SW structures; `mlx5_load`
   activates HW. This enables **devlink reload** -- you can `unload` + `load` without
   tearing down SW structures. `mlx5_devlink_reload_up()` calls
   `mlx5_init_one_devl_locked()` directly.
2. **Auxiliary device model**: `mlx5_register_device()` triggers auxiliary bus probes
   for netdev (`mlx5e_probe`), RDMA (`mlx5_ib_probe`), etc. The core driver never
   directly creates a netdev.
3. **Reusable across contexts**: `mlx5_init_one` is called from both `probe_one`
   (PCI PF) and `mlx5_sf_dev_probe` (Scalable Functions).
4. **Devlink lock held through init**: `devl_lock()` taken at start of
   `mlx5_init_one`, held through entire `mlx5_init_one_devl_locked`.
5. **Lightweight probe path**: `mlx5_init_one_light` exists for reduced-capability init.

---

## ice Initialization Flow

ice uses a **flatter** architecture where `ice_probe()` is a large monolithic function
(136 lines, cyclomatic complexity 22) that handles PCI setup, HW init, and device
bring-up in a single call chain.

### ice_probe() (PCI probe callback)
```
ice_probe(pdev, id)
  |-- Reject VF probe
  |-- kdump kernel handling (FLR reset)
  |-- pcim_enable_device()           # Enable PCI device (devres managed)
  |-- pcim_iomap_regions()           # Map BAR0
  |-- ice_allocate_pf()              # Allocate PF via devlink_alloc
  |-- dma_set_mask_and_coherent()    # DMA mask setup (64-bit)
  |-- pci_set_master()
  |-- Manual hw struct population:
  |     hw->hw_addr, vendor_id, device_id, revision_id,
  |     subsystem_vendor_id, subsystem_device_id, bus info
  |-- ice_set_ctrlq_len()            # Set control queue lengths
  |-- ice_is_recovery_mode() check   # Branch to recovery if needed
  |     |-- ice_probe_recovery_mode()
  |-- ice_init_hw()                  # Full HW initialization (see below)
  |-- ice_adapter_get()              # Get/create adapter (multi-PF coordination)
  |-- ice_init_dev()                 # Device-level init
  |-- ice_init()                     # PF software init (see below)
  |-- devl_lock()
  |-- ice_load()                     # Netdev + features (see below)
  |-- ice_init_devlink()             # Devlink registration (late!)
  |-- devl_unlock()
```

### ice_init_hw() -- Hardware initialization (in ice_common.c)
This is the E810 Admin Queue command-heavy phase:
```
ice_init_hw(hw)
  |-- ice_set_mac_type()             # Determine MAC type (E810 vs E830)
  |-- ice_aq_init()                  # Initialize Admin Queue
  |-- ice_init_nvm()                 # NVM (flash) initialization
  |-- ice_get_caps()                 # Query device capabilities via AQ
  |-- ice_init_port_info()           # Port configuration
  |-- ice_init_hw_tbls()             # HW tables (VSI, switch rules, etc.)
  |-- ice_get_mac_addr()             # Get MAC address from NVM
  |-- ... other AQ-based initialization
```

### ice_init() -- PF software initialization
```
ice_init(pf)
  |-- ice_init_pf()                  # PF data structures, timers, workqueues
  |-- pci_enable_ptm() [E830 only]   # Precision Time Measurement
  |-- ice_alloc_vsis()               # Allocate VSI array
  |-- ice_init_pf_sw()              # PF switch setup (default VSI, rules)
  |-- ice_init_wakeup()             # Wake-on-LAN setup
  |-- ice_init_link()               # Link configuration
  |-- ice_send_version()            # Send driver version to FW via AQ
  |-- ice_verify_cacheline_size()
  |-- ice_set_safe_mode_vlan_cfg()  # Safe mode VLAN (if applicable)
  |-- clear_bit(ICE_DOWN) / clear_bit(ICE_SERVICE_DIS)
  |-- mod_timer(serv_tmr)           # Start service timer
```

### ice_load() -- Netdev and feature registration
```
ice_load(pf)
  |-- ice_cfg_netdev()              # Allocate and configure netdev directly
  |-- ice_dcbnl_setup()             # DCB netlink interface
  |-- ice_init_mac_fltr()           # MAC filter setup
  |-- ice_devlink_create_pf_port()  # Devlink port creation
  |-- ice_register_netdev()         # Register network device directly
  |-- ice_tc_indir_block_register() # TC indirect block
  |-- ice_napi_add()                # NAPI registration
  |-- ice_init_features()           # Feature flags (GNSS, clock, hwmon, etc.)
  |-- ice_init_rdma()               # RDMA/iWARP auxiliary device via IDC
  |-- ice_rdma_finalize_setup()     # Finalize RDMA
  |-- ice_service_task_restart()    # Restart service task
```

---

## Key Differences: What mlx5 Does That ice Doesn't

| Aspect | mlx5 | ice |
|--------|------|-----|
| **Probe decomposition** | 5+ layers: devlink_alloc -> mdev_init -> pci_init -> init_one (function_setup -> init_once -> load -> register_device) | 2-tier: probe (PCI+HW inline) -> init+load |
| **Probe complexity** | probe_one: 11 cyclomatic | ice_probe: 22 cyclomatic |
| **Core dev allocation** | `mlx5_mdev_init` -- dedicated init with profiles, mutexes, cmd interface, health, pagealloc | Direct struct population inline in probe |
| **Netdev creation** | Via auxiliary bus (`mlx5e_probe` triggered by `mlx5_register_device`) | Directly in `ice_load` -> `ice_cfg_netdev` -> `ice_register_netdev` |
| **Devlink integration** | Early: devlink_alloc embeds core_dev, lock held through full init | Late: `ice_init_devlink` called after everything else |
| **Reload support** | Native: `mlx5_init_once` (allocate once) + `mlx5_load`/`mlx5_unload` (repeatable) | Partial: `ice_init`+`ice_load` exist but less cleanly separated |
| **FW handshake** | Multi-step: enable -> wait -> cmd_enable -> enable_hca -> set_issi -> pages -> open -> init_hca -> query_caps | Single `ice_init_hw`: aq_init -> nvm -> get_caps |
| **Page allocation** | `mlx5_pagealloc_init/start` + `mlx5_satisfy_startup_pages` (FW demands pages) | Not needed -- E810 FW manages own memory |
| **Command interface** | Explicit lifecycle: cmd_init -> cmd_enable -> cmd_set_state -> cmd_disable | AQ init as part of ice_init_hw, no separate lifecycle |
| **eSwitch** | Full eswitch subsystem (`mlx5_eswitch_init`) in init_once | No eswitch |
| **SubFunctions** | Full SF support: sf_hw_table_init, sf_table_init, sf_dev_table_create | No SFs (VFs only) |
| **Health monitoring** | First-class: health_init in mdev_init, health_poll in function_setup, fw_log_up | Service task timer only |
| **HW monitoring** | `mlx5_hwmon_dev_register` in core init | hwmon via `ice_init_features` in load path |
| **Crash dump** | `mlx5_crdump_enable` / `mlx5_rsc_dump_create` | Not present |
| **Rate limiting** | `mlx5_init_rl_table` in init_once | Not in init path |
| **Flow steering** | Split: `mlx5_fs_core_alloc` (init) + `mlx5_fs_core_init` (load) | Not in init path |
| **LAG** | `mlx5_lag_add_mdev` in load | Not in init path |
| **FPGA** | `mlx5_fpga_init`/`mlx5_fpga_device_start` | Not applicable |
| **Tunnel offload** | `mlx5_vxlan_create`/`mlx5_geneve_create` in init_once | Not in init path |
| **Recovery mode** | `mlx5_init_one_light` (reduced-cap) | `ice_is_recovery_mode` -> `ice_probe_recovery_mode` (NVM corruption) |
| **kdump handling** | Not in probe path | FLR before PCI enable when kdump kernel detected |

---

## Implications for fdice_ Design

### Adopt from mlx5

1. **Layered init architecture**: fdice_ should separate:
   - `fdice_mdev_init()` -- core SW structures, AQ command interface, health framework
   - `fdice_pci_init()` -- PCI resource management (BAR, DMA, MSI-X)
   - `fdice_function_setup()` -- AQ-based FW handshake (E810-specific AQ commands)
   - `fdice_init_once()` -- one-time subsystem SW allocation
   - `fdice_load()` -- runtime HW activation
   - `fdice_register_device()` -- auxiliary device registration (netdev, RDMA)

2. **Devlink reload support**: The init_once/load separation enables devlink reload
   from day one. This is a significant operational advantage over ice.

3. **Auxiliary device model**: Register netdev and RDMA as auxiliary devices rather
   than tightly coupling them into the core driver. This gives cleaner module boundaries
   and matches the kernel's direction for modern NIC drivers.

4. **Health monitoring from boot**: Initialize health checking early in `fdice_mdev_init`,
   make it a first-class subsystem with dedicated polling like mlx5.

5. **Explicit AQ lifecycle**: The E810 Admin Queue should have init/enable/disable/cleanup
   phases like mlx5's command interface, rather than ice's monolithic `ice_aq_init`.

6. **Devlink-first allocation**: Use `devlink_alloc` to embed `fdice_core_dev`, take
   `devl_lock` early, register params before load.

### Keep from ice (E810-specific)

1. **Admin Queue command ordering**: The `ice_init_hw()` flow defines the correct E810
   AQ command sequence. Study it for the AQ init -> NVM -> get_caps ordering.

2. **Recovery mode**: `ice_is_recovery_mode()` / `ice_probe_recovery_mode()` handles
   corrupted NVM. fdice_ needs this for E810 robustness.

3. **Control queue length configuration**: `ice_set_ctrlq_len()` sets the E810 AQ/MBX
   queue depths. This is hardware-specific and must be preserved.

4. **kdump kernel handling**: The FLR-before-enable pattern for kdump kernels is
   E810-specific PCI behavior that ice handles correctly.

5. **Adapter coordination**: `ice_adapter_get()` provides multi-PF coordination for
   shared resources. Evaluate whether fdice_ needs this.

### Design Principle

The E810 Admin Queue command interface is the hardware boundary.
- **Below the AQ** (HW init, AQ commands, NVM access): study ice's implementation,
  rewrite as fdice_ with the correct E810 command sequences.
- **Above the AQ** (subsystem management, devlink, auxiliary devices, health monitoring,
  init/load lifecycle): adopt mlx5 patterns for superior architecture.

### Proposed fdice_ Init Skeleton
```
fdice_probe(pdev, id)                    # Thin PCI probe (like mlx5's probe_one)
  |-- [kdump FLR if needed]              # from ice
  |-- fdice_devlink_alloc()              # devlink-first (from mlx5)
  |-- fdice_adev_idx_alloc()             # auxiliary device index (from mlx5)
  |-- fdice_mdev_init(dev)               # core dev: mutexes, AQ cmd init, health (from mlx5)
  |-- fdice_pci_init(dev, pdev)          # PCI enable, BAR, DMA, MSI-X
  |-- fdice_init_one(dev)                # device-level init (reusable for future SFs)
        |-- devl_lock()
        |-- fdice_function_setup()       # AQ-based FW handshake
        |     |-- fdice_set_ctrlq_len()  # E810 queue sizing (from ice)
        |     |-- fdice_init_hw()        # AQ init + cap discovery (ice HW boundary)
        |     |-- fdice_start_health_poll() # health monitoring (from mlx5)
        |-- fdice_init_once()            # one-time SW objects (mlx5 pattern)
        |-- fdice_devlink_params_register()
        |-- fdice_load()                 # HW activation (repeatable for reload)
        |     |-- fdice_irq_create()
        |     |-- fdice_eq_create()
        |     |-- fdice_fs_init()        # flow steering
        |     |-- fdice_sriov_attach()
        |-- fdice_register_device()      # trigger auxiliary bus (mlx5 pattern)
        |-- devl_unlock()
```
