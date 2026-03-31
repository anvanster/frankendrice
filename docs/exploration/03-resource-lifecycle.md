<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 03: Resource Lifecycle & Cleanup

## Question

mlx5 uses a structured teardown sequence with dependency tracking. How does ice
handle resource cleanup? Where does ice leak or have less robust error recovery?

## Sources Explored

- `kernel/drivers/net/ethernet/mellanox/mlx5/core/main.c` -- mlx5_cleanup_once,
  mlx5_uninit_one, mlx5_unload, mlx5_function_teardown, mlx5_init_one_devl_locked
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/fw.c` -- mlx5_cmd_teardown_hca,
  mlx5_cmd_force_teardown_hca, mlx5_cmd_fast_teardown_hca
- `kernel/drivers/net/ethernet/intel/ice/ice_main.c` -- ice_remove, ice_probe,
  ice_unload, ice_deinit, ice_deinit_dev
- `kernel/drivers/net/ethernet/intel/ice/ice_common.c` -- ice_deinit_hw

---

## mlx5 Teardown Approach: Structured Four-Layer Hierarchy

mlx5 separates resource lifecycle into four distinct layers, each with a clear
scope and matching init/cleanup pair. This creates a dependency-aware teardown
that is robust against partial initialization failures.

### Layer 0: `mlx5_mdev_uninit` (Core Device Object)

Outermost layer -- tears down the core device structure itself, called from
`remove_one()` after `mlx5_uninit_one()`. Destroys foundational objects:
```
mlx5_notifiers_cleanup      -- notification chain
mlx5_hca_caps_free          -- HCA capabilities cache
mlx5_adev_cleanup           -- auxiliary device infrastructure
mlx5_pagealloc_cleanup      -- page allocator metadata
mlx5_health_cleanup         -- health monitoring workqueue
mlx5_tout_cleanup           -- timeout table
mlx5_cmd_cleanup            -- command interface
debugfs_remove_recursive    -- debugfs entries
mutex_destroy (x7)          -- pgdir, alloc, bfregs, wc_state, uplink, intf_state
lockdep_unregister_key      -- lock dependency tracking
```

This layer is the mirror of `mlx5_mdev_init()` called from `probe_one()`.

### Layer 1: `mlx5_function_teardown` (Hardware Interface)

Lowest layer -- tears down the HCA firmware interface. Calls:
- `mlx5_function_close()` -- closes the firmware command interface
- `mlx5_function_disable()` -- disables PCI-level function
- `mlx5_stop_health_poll()` -- stops the health monitoring timer

This layer also has **three teardown escalation modes** for crash resilience:
1. **Normal teardown** (`mlx5_cmd_teardown_hca`) -- standard HCA command
2. **Force teardown** (`mlx5_cmd_force_teardown_hca`) -- uses polling-based
   command execution (no interrupts needed), checks firmware capability flag,
   returns state indicating success or failure
3. **Fast teardown** (`mlx5_cmd_fast_teardown_hca`) -- sets NIC interface to
   DISABLED state, polls for completion with configurable timeout, handles PCI
   channel offline gracefully

The `mlx5_try_fast_unload` function in `remove_one` orchestrates these escalation
modes at PCI remove time, allowing the driver to skip the full software teardown
when the firmware can handle cleanup autonomously.

### Layer 2: `mlx5_uninit_one` (Device Lifecycle Orchestrator)

Orchestrates the full uninit sequence with **state-bit gating**:
```c
mlx5_hwmon_dev_unregister(dev);
mlx5_crdump_disable(dev);
mlx5_unregister_device(dev);            // unplug auxiliary devices
if (!test_bit(MLX5_INTERFACE_STATE_UP, &dev->intf_state)) {
    // Skip unload if never fully initialized
    mlx5_devlink_params_unregister(...);
    mlx5_cleanup_once(dev);
    goto out;
}
clear_bit(MLX5_INTERFACE_STATE_UP, &dev->intf_state);
mlx5_unload(dev);                       // Layer 3
mlx5_devlink_params_unregister(...);
mlx5_cleanup_once(dev);                 // Layer 4
mlx5_function_teardown(dev, true);      // Layer 1
```

The `MLX5_INTERFACE_STATE_UP` bit check is critical -- it allows safe teardown
of a partially-initialized device by skipping the runtime unload.

### Layer 3: `mlx5_unload` (Runtime Subsystems)

Middle layer -- tears down active/runtime subsystems in reverse init order:
```
mlx5_lag_remove_mdev        -- Link Aggregation
mlx5_ec_cleanup             -- Embedded CPU
mlx5_sf_hw_table_destroy    -- Subfunctions HW table
mlx5_fs_core_cleanup        -- Flow Steering core
mlx5_fpga_device_stop       -- FPGA device
mlx5_rsc_dump_cleanup       -- Resource dump diagnostics
mlx5_hv_vhca_cleanup        -- Hyper-V VHCA
mlx5_fw_reset_events_stop   -- Firmware reset events
mlx5_fw_tracer_cleanup      -- Firmware tracer
mlx5_clock_unload           -- PTP clock
mlx5_eq_table_destroy       -- Event queues
mlx5_irq_table_destroy      -- IRQ table
mlx5_pagealloc_stop         -- Page allocation
mlx5_events_stop            -- Event subsystem
mlx5_free_bfreg             -- BlueFlame registers
```

### Layer 4: `mlx5_cleanup_once` (One-Time Software Objects)

Frees software objects allocated once during the device lifetime:
```
mlx5_fs_core_free           -- Flow Steering memory
mlx5_sf_table_cleanup       -- Subfunctions table
mlx5_sf_hw_table_cleanup    -- SF HW table metadata
mlx5_vhca_event_cleanup     -- VHCA events
mlx5_fpga_cleanup           -- FPGA metadata
mlx5_eswitch_cleanup        -- E-Switch
mlx5_sriov_cleanup          -- SR-IOV
mlx5_mpfs_cleanup           -- Multi-Physical-Function Switch
mlx5_cleanup_rl_table       -- Rate Limit table
mlx5_geneve_destroy         -- GENEVE tunnel
mlx5_vxlan_destroy          -- VXLAN tunnel
mlx5_cleanup_clock          -- Clock metadata
mlx5_cleanup_reserved_gids  -- Reserved GIDs
mlx5_cq_debugfs_cleanup     -- CQ debugfs
mlx5_fw_reset_cleanup       -- FW reset objects
mlx5_eq_table_cleanup       -- EQ table metadata
mlx5_irq_table_cleanup      -- IRQ table metadata
mlx5_devcom_unregister_device -- Device communication
mlx5_rsc_dump_destroy       -- Resource dump objects
mlx5_fw_tracer_destroy      -- FW tracer objects
mlx5_hv_vhca_destroy        -- Hyper-V VHCA objects
mlx5_st_destroy             -- Steering tables
```

### Key Design Pattern: init_once/cleanup_once vs load/unload

mlx5 separates **one-time allocation** (init_once/cleanup_once) from **runtime
activation** (load/unload). This separation means:
- A devlink reload only calls unload/load, preserving one-time allocations
- The cleanup_once path is only reached during full device removal
- Each subsystem has both a `_cleanup` (free metadata) and `_destroy` (free
  runtime state) function with distinct semantics

### Error Path in `mlx5_init_one_devl_locked`

mlx5 uses a classic goto-chain pattern with fine-grained labels:
```c
err = mlx5_function_setup(dev, ...);       // goto err_function
err = mlx5_init_once(dev);                 // goto function_teardown
err = mlx5_devlink_params_register(...);   // goto err_devlink_params_reg
err = mlx5_load(dev);                      // goto err_load
err = mlx5_register_device(dev);           // goto err_register
mlx5_crdump_enable(dev);                   // non-fatal, log only
mlx5_hwmon_dev_register(dev);              // non-fatal, log only

err_register:  mlx5_unload(dev);
err_load:      mlx5_devlink_params_unregister(...);
err_devlink_params_reg: mlx5_cleanup_once(dev);
function_teardown: mlx5_function_teardown(dev, true);
err_function:  dev->state = MLX5_DEVICE_STATE_INTERNAL_ERROR;
```

Each goto label exactly undoes the corresponding init step. Non-critical
registrations (crdump, hwmon) log errors but do not fail the probe.

---

## ice Teardown Approach: Flat Procedural Sequence

### `ice_remove` (PCI remove callback)

ice_remove is a single function that calls cleanup helpers in sequence:
```c
// Recovery mode early exit
if (ice_is_recovery_mode(&pf->hw)) {
    ice_service_task_stop(pf);
    ice_deinit_devlink(pf);
    return;
}

// SR-IOV cleanup
if (test_bit(ICE_FLAG_SRIOV_ENA, pf->flags)) {
    set_bit(ICE_VF_RESETS_DISABLED, pf->state);
    ice_free_vfs(pf);
}

// Feature cleanup (conditional)
if (!ice_is_safe_mode(pf))
    ice_remove_arfs(pf);

// Devlink and core unload
ice_dealloc_all_dynamic_ports(pf);
ice_deinit_devlink(pf);
ice_unload(pf);

// Low-level teardown
ice_deinit(pf);
ice_vsi_release_all(pf);
ice_setup_mc_magic_wake(pf);
ice_set_wake(pf);
ice_adapter_put(pdev);
ice_deinit_hw(&pf->hw);
ice_deinit_dev(pf);
ice_aq_cancel_waiting_tasks(pf);
```

### Sub-functions breakdown

**`ice_unload`** tears down the runtime feature set:
- `ice_deinit_rdma` -- RDMA/auxiliary device
- `ice_unplug_aux_dev` -- auxiliary bus device
- `ice_tc_indir_block_unregister` -- TC indirect block
- `ice_unregister_netdev` -- netdev
- `ice_deinit_features` -- PTP, GNSS, etc.
- `ice_decfg_netdev` -- netdev deconfiguration
- `ice_devlink_destroy_pf_port` -- devlink port

**`ice_deinit`** tears down PF-level software state:
- `ice_deinit_pf_sw` -- PF software state
- `ice_deinit_pf` -- PF structure (see detail below)
- `ice_dealloc_vsis` -- VSI array

**`ice_deinit_features`** (flag-gated feature teardown):
```c
ice_deinit_lag(pf);
if (!ice_is_safe_mode(pf))         ice_deinit_fdir(pf);
if (!ice_is_safe_mode(pf) &&
    ice_is_feature_supported(pf, ICE_F_LLDP))
                                   ice_cfg_lldp_mib_change(hw, false);
if (ice_is_feature_supported(pf, ICE_F_GNSS))
                                   ice_gnss_exit(pf);
if (test_bit(ICE_FLAG_PTP_SUPPORTED, pf->flags))
                                   ice_ptp_release(pf);
if (test_bit(ICE_FLAG_DPLL, pf->flags))
                                   ice_dpll_deinit(pf);
if (pf->eswitch_mode == DEVLINK_ESWITCH_MODE_SWITCHDEV)
                                   xa_destroy(&pf->eswitch.reprs);
ice_hwmon_exit(pf);
```

This flag-gating pattern is a significant weakness -- see analysis below.

**`ice_deinit_pf`** (PF object cleanup):
```c
bitmap_free(pf->avail_txqs);       // conditional on non-NULL
bitmap_free(pf->avail_rxqs);       // conditional on non-NULL
bitmap_free(pf->txtime_txqs);      // conditional on non-NULL
ptp_clock_unregister(pf->ptp.clock); // conditional on clock exists
ice_free_irq_msix_misc(pf);        // conditional on !xa_empty(irq_tracker)
xa_destroy(&pf->dyn_ports);
xa_destroy(&pf->sf_nums);
```

**`ice_deinit_dev`** tears down hardware-level state:
- `ice_service_task_stop` -- stop service task
- `ice_clear_interrupt_scheme` -- free IRQ resources
- `ice_reset(hw, ICE_RESET_PFR)` -- issue PF reset

**`ice_deinit_hw`** frees hardware metadata:
- `ice_cleanup_fltr_mgmt_struct` -- filter management
- `ice_sched_cleanup_all` -- scheduler trees
- `ice_sched_clear_agg` -- aggregator nodes
- `ice_free_seg` -- DDP segment
- `ice_free_hw_tbls` -- hardware tables
- `ice_destroy_all_ctrlq` -- control queues
- `ice_clear_all_vsi_ctx` -- VSI contexts
- `__fwlog_deinit` -- firmware logging
- `mutex_destroy` -- various mutexes

### Error Path in `ice_probe`

ice_probe uses a goto-chain pattern similar to mlx5:
```c
err = ice_init_hw(hw);           // return err (no cleanup needed yet)
adapter = ice_adapter_get(pdev); // goto unroll_hw_init
err = ice_init_dev(pf);          // goto unroll_adapter
err = ice_init(pf);              // goto unroll_dev_init
err = ice_load(pf);              // goto unroll_init
err = ice_init_devlink(pf);      // goto unroll_load

unroll_load:     ice_unload(pf);
unroll_init:     ice_deinit(pf);
unroll_dev_init: need_dev_deinit = true;
unroll_adapter:  ice_adapter_put(pdev);
unroll_hw_init:  ice_deinit_hw(hw);
                 if (need_dev_deinit) ice_deinit_dev(pf);
```

---

## Error Recovery Comparison

### mlx5 Strengths

1. **Four-tier teardown hierarchy** with strict layering ensures subsystems
   are torn down in the correct dependency order. Each layer has a clear scope
   and its own init/cleanup pair (mdev_uninit, uninit_one, unload, cleanup_once).

2. **Firmware-assisted teardown escalation**: mlx5 has three HCA teardown modes
   (normal, force, fast) that allow graceful degradation when the command
   interface is impaired. The `mlx5_try_fast_unload` path can skip expensive
   software cleanup entirely when firmware handles it.

3. **Separate alloc vs activate lifecycle**: The `init_once`/`cleanup_once` vs
   `load`/`unload` split means devlink reload only cycles runtime state, not
   one-time allocations. This reduces the surface area for leaks during reload.

4. **Interface state tracking**: mlx5 uses `MLX5_INTERFACE_STATE_UP` bit to
   guard teardown -- `mlx5_uninit_one` checks this bit and skips unload if the
   interface was never fully brought up, preventing double-free.

5. **Non-fatal registration pattern**: Late registrations (crdump, hwmon) log
   errors but do not fail the probe, preventing regression from optional features.

6. **Health monitoring integration**: `mlx5_stop_health_poll` is called during
   teardown, preventing health checks from firing on a partially torn-down device.

### ice Weaknesses

1. **Flat teardown without dependency tracking**: `ice_remove` is a linear
   sequence of function calls without the layered init/cleanup symmetry that
   mlx5 uses. The teardown order is hardcoded and not structurally enforced.

2. **Ordering anomaly in ice_remove**: `ice_deinit_dev` is called **after**
   `ice_deinit_hw`, but `ice_deinit_dev` calls `ice_reset(hw, ICE_RESET_PFR)`
   which sends a command through the admin queue -- yet `ice_deinit_hw` already
   called `ice_destroy_all_ctrlq`. This means the PF reset in `ice_deinit_dev`
   races with already-destroyed control queues. This is a potential issue if the
   reset actually attempts to use the AQ.

3. **`ice_aq_cancel_waiting_tasks` called last**: This function cancels AQ
   waiters, but it is called after `ice_deinit_hw` has already destroyed the
   control queues. Any task waiting on an AQ response may access freed memory
   between `ice_deinit_hw` and `ice_aq_cancel_waiting_tasks`.

4. **No firmware-assisted fast teardown**: ice lacks an equivalent to mlx5's
   force/fast teardown HCA commands. If the firmware is unresponsive, ice has no
   escalation path and will block or timeout during control queue operations.

5. **Conditional cleanup fragility**: `ice_remove` conditionally cleans up ARFS
   only if not in safe mode, and SR-IOV only if the flag is set. Worse,
   `ice_deinit_features` gates cleanup on `ICE_FLAG_PTP_SUPPORTED`,
   `ICE_FLAG_DPLL`, `ice_is_safe_mode()`, and `ice_is_feature_supported()`. If
   any of these flags/checks are cleared or modified before teardown (e.g., by
   a concurrent reset, error recovery, or firmware event), the corresponding
   cleanup will be skipped, potentially leaking resources. mlx5 avoids this by
   making its cleanup functions idempotent and calling them unconditionally.

6. **Probe error path ordering issue**: In the `ice_probe` error path,
   `ice_deinit_dev` is called after `ice_deinit_hw`, controlled by the
   `need_dev_deinit` flag. This creates a non-obvious ordering dependency --
   `ice_deinit_dev` calls `ice_reset()` and `ice_clear_interrupt_scheme()`, but
   the HW structures it depends on were already freed by `ice_deinit_hw`.

7. **No reload-aware resource separation**: ice does not separate one-time
   allocations from runtime state as cleanly as mlx5. The `ice_load`/`ice_unload`
   split exists but was a later addition -- some resources straddle the boundary
   awkwardly.

8. **Reset-during-remove race**: While `ice_remove` sets `ICE_VF_RESETS_DISABLED`
   before freeing VFs, there is no equivalent guard for PF resets. If a reset
   is in progress when `ice_remove` is called, `ice_is_reset_in_progress` is
   checked but the remove proceeds regardless, with only a comment noting the
   potential issue.

---

## Recommendations for fdice_ Resource Lifecycle

### R1: Adopt mlx5's Four-Layer Teardown Hierarchy

Design the fdice_ lifecycle around four explicit layers:
- **`fdice_mdev_uninit`** -- core device object destruction (mutexes, cmd
  interface, debugfs, health workqueue)
- **`fdice_hw_teardown`** -- firmware/hardware interface teardown (AQ shutdown,
  HCA teardown commands)
- **`fdice_unload`** -- runtime subsystem deactivation (reverse order of load)
- **`fdice_cleanup_once`** -- one-time software object destruction

Each layer should have a matching init function, and the init function should
use goto-chain error handling that calls the exact corresponding cleanup.

Add a state-bit gate (`FDICE_INTERFACE_STATE_UP`) in the orchestrator to skip
runtime unload when the device never fully initialized.

### R2: Implement Teardown Escalation

Implement firmware-assisted teardown with escalation:
1. Normal TEARDOWN_HCA command
2. Force teardown via polling (no interrupt dependency)
3. Fast teardown with NIC state transition and timeout

This protects against firmware hangs during shutdown/crash.

### R3: Enforce init/cleanup Symmetry with a Registration Table

Instead of manually writing mirror-image init/cleanup sequences, consider a
table-driven approach:
```c
struct fdice_subsystem {
    const char *name;
    int (*init)(struct fdice_dev *dev);
    void (*cleanup)(struct fdice_dev *dev);
    bool initialized;
};
```
Walk the table forward for init (stopping on first error and recording which
subsystems were initialized), walk backward for cleanup (only cleaning up
initialized subsystems). This eliminates the class of bugs where init and cleanup
sequences diverge.

### R4: Cancel Waiters Before Destroying Queues

Ensure that AQ waiters are cancelled **before** the control queues are destroyed,
not after. This avoids the race condition present in ice.

### R5: Guard Against Reset-During-Remove

Use an atomic state machine or mutex to serialize remove against ongoing resets.
mlx5's `intf_state_mutex` pattern is a good model. Set a "removing" state early
and have reset paths check for it.

### R6: Non-Fatal Optional Registrations

Follow mlx5's pattern for optional subsystems (hwmon, debugfs, diagnostics):
log errors but do not fail the probe. This prevents optional feature regressions
from blocking device operation.

### R7: Explicit State Tracking

Track initialization state with a bitmask or structured state variable. During
cleanup, only tear down subsystems that were actually initialized. This is more
robust than ice's flag-based conditional cleanup and prevents cleanup of
uninitialized resources.

### R8: Idempotent and Unconditional Cleanup

Never gate cleanup on feature flags or runtime state. Instead, make each cleanup
function internally check whether its subsystem was initialized:
```c
void fdice_ptp_cleanup(struct fdice_pf *pf)
{
    if (!pf->ptp.initialized)
        return;
    /* actual cleanup ... */
    pf->ptp.initialized = false;
}
```
This avoids ice's fragile flag-gating pattern where `ice_deinit_features()`
conditionally calls cleanup based on `ICE_FLAG_PTP_SUPPORTED`, `ICE_FLAG_DPLL`,
etc., which can be cleared by concurrent events before teardown runs.
