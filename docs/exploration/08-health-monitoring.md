<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 08: Health Monitoring & Error Recovery

## Overview

Comparison of mlx5's comprehensive health/recovery system with ice's simpler
approach. This is a **major improvement vector** for fdice_ -- mlx5's
architecture is significantly more capable in error detection, diagnosis,
and automated recovery.

## mlx5 Health System

### Architecture

mlx5 implements a multi-layered health monitoring system spanning firmware
diagnostics, hardware sensors, data path health reporters, and VNIC
diagnostics. The system is centered around `struct mlx5_core_health`
embedded in `dev->priv.health`.

**Source**: `kernel/drivers/net/ethernet/mellanox/mlx5/core/health.c`

### Layer 1: Hardware Health Buffer Polling

mlx5 firmware exposes a shared-memory `health_buffer` in the Initial
Segment (ISEG) of the device, which the driver polls via a timer:

```
poll_health() [timer callback, jittered interval]
  -> mlx5_health_check_fatal_sensors()
     -> sensor_pci_not_working()      -- ioread returns 0xFFFFFFFF
     -> pci_channel_offline()         -- PCI AER detection
     -> mlx5_get_nic_state()          -- NIC IFC disabled/SW reset
     -> sensor_fw_synd_rfr()          -- FW syndrome + reset-for-recovery bit
  -> check health_counter             -- FW heartbeat (miss_counter)
  -> check synd field changes         -- new syndrome codes
```

The health buffer contains rich diagnostic data read via MMIO:
- `synd` -- syndrome code identifying the specific error
- `ext_synd` -- extended syndrome for additional detail
- `rfr_severity` -- reset-for-recovery flag + severity level + cold-reset-required flag
- `assert_var[5]` -- FW assertion variables for debugging
- `assert_exit_ptr` -- FW crash address
- `assert_callra` -- FW call return address
- `fw_ver` -- firmware version at time of error
- `hw_id` -- hardware identifier
- `irisc_index` -- which FW RISC processor faulted
- `time` -- FW timestamp of the error

### Layer 2: Syndrome-Based Diagnostics

mlx5 defines 12+ specific health syndrome codes that provide precise
root cause identification:

| Syndrome | Meaning |
|----------|---------|
| `FW_INTERNAL_ERR` | Firmware internal error |
| `DEAD_IRISC` | FW RISC processor not responding |
| `HW_FATAL_ERR` | Unrecoverable hardware error |
| `FW_CRC_ERR` | Firmware image CRC error |
| `ICM_FETCH_PCI_ERR` | Internal memory fetch PCI error |
| `ICM_PAGE_ERR` | HW fatal error |
| `ASYNCHRONOUS_EQ_BUF_OVERRUN` | Async event queue overrun |
| `EQ_IN_ERR` | Event queue error |
| `EQ_INV` | Invalid EQ referenced |
| `FFSER_ERR` | FFSER error |
| `HIGH_TEMP_ERR` | High temperature |
| `ICM_PCI_POISONED_ERR` | ICM PCI data poisoned |
| `TRUST_LOCKDOWN_ERR` | Trust lockdown error |

Each syndrome maps to human-readable text and a severity level
(EMERGENCY through DEBUG, using standard kernel log levels).

### Layer 3: Devlink Health Reporters

mlx5 creates **five** distinct devlink health reporters:

1. **`fw`** -- Non-fatal firmware health issues (syndrome changes,
   heartbeat misses). Supports `diagnose` (reads syndrome from health
   buffer) and `dump` (triggers core dump + FW tracer saved traces).

2. **`fw_fatal`** -- Fatal firmware errors requiring recovery. Supports
   `recover` (calls `mlx5_health_try_recover`), `dump` (collects
   CR-dump binary data), with role-specific grace periods:
   - ECPF: 180s, PF: 60s, VF/SF: 30s

3. **`tx`** -- Per-channel TX data path errors (CQE errors, timeouts,
   PTP SQ health). Supports per-queue recover + dump with full SQ/CQ/EQ
   state via `mlx5e_reporter_tx_*`.

4. **`rx`** -- Per-channel RX data path errors (CQE errors, timeouts,
   ICOSQ issues). Supports per-queue recover + dump with full RQ/CQ/EQ
   state via `mlx5e_reporter_rx_*`.

5. **`vnic`** -- VNIC environment diagnostics. Reports queue counters
   (error queues, priority updates), EQ/CQ overruns, invalid commands,
   quota exceeded, steering discards, ICM consumption, BAR UAR access,
   ODP page faults.

### Layer 4: Recovery Mechanisms

**Fatal error recovery flow** (`mlx5_fw_fatal_reporter_err_work`):
1. Enter error state (flush command interface, notify subsystems)
2. If `fw_fatal` reporter exists, report through devlink (triggers
   user-space notification + optional auto-recover)
3. If recovery cancelled (grace period), unload driver
4. Recovery: `mlx5_health_try_recover` -> `mlx5_handle_bad_state` ->
   wait for PCI up -> `mlx5_recover_device` (full reload)

**SW Reset flow** (`mlx5_error_sw_reset`):
1. Acquire SW reset semaphore (PF only, via VSC gateway)
2. Write NIC IFC to SW_RESET state
3. Poll for NIC to reach DISABLED state
4. Release semaphore
5. Multi-PF coordination via shared health buffer + semaphore

**Data path recovery** (TX/RX reporters):
- SQ recovery: error -> reset -> ready state machine via `mlx5e_health_sq_to_ready`
- Channel recovery: full channel reopen via `mlx5e_safe_reopen_channels`
- EQ recovery: poll IRQ disabled, rearm EQ

### Layer 5: Firmware Tracing

`mlx5_fw_tracer` provides continuous firmware log collection:
- Shared buffer between FW and driver
- Decoded trace messages with timestamps
- Saved traces included in health dumps
- Hourly FW log timestamp synchronization (`mlx5_health_log_ts_update`)

### Layer 6: Resource Dump

`mlx5_rsc_dump` enables dumping internal firmware/hardware resources:
- Per-resource-type dump commands
- Binary data collection for offline analysis
- Integrated with health reporter dump callbacks

## ice Health System

### Architecture

ice implements a simpler health monitoring system across two subsystems:
devlink health reporters (relatively new, added 2024) and the traditional
reset mechanism.

**Source**: `kernel/drivers/net/ethernet/intel/ice/devlink/health.c`,
`ice_main.c`, `ice_common.c`

### Devlink Health Reporters

ice creates **four** reporters:

1. **`fw`** -- Firmware health status events. Diagnose + dump only (no
   auto-recover). Reports status codes received from FW via Admin Queue
   health status events (global scope).

2. **`port`** -- Port health status events. Diagnose + dump only.
   Reports port-specific and PF-specific health codes from FW.

3. **`mdd`** -- Malicious Driver Detection events. Dump only. Reports
   source (TX PQM/TCLAN/TDPU or RX), PF/VF number, event type, queue.

4. **`tx_hang`** -- TX hang events. Dump only. Reports head, interrupt
   register, VSI, queue, next_to_clean/use, ring descriptor data, skb data.

**Key limitation**: The comment in `ice_devlink_health_report` explicitly
states: *"We do not do auto recovering"*. All reporters are report-only
with no recovery callbacks.

### Firmware Health Status Codes

ice receives health status from firmware via Admin Queue events
(`ice_process_health_status_event`). The driver maintains a lookup table
of ~25 status codes with:
- Description string
- Suggested solution string
- Up to 2 data labels

Status codes are primarily **port/link focused**:
- Module errors: unsupported, wrong type, not qualified, communication failure
- Link errors: invalid config, parallel fault, PHY limited
- Netlist errors: topology corruption, unrecoverable
- Flash/NVM errors: access failure, authentication failure
- DDP/OROM errors: authentication, compatibility

There are no firmware internal error syndromes, no crash diagnostics,
no assertion data, and no temperature monitoring.

### Reset Mechanism

ice implements a traditional reset hierarchy managed via state bits:

```
ice_schedule_reset(pf, type)     -- sets PFR/CORER/GLOBR_REQ bit
  -> ice_service_task_schedule() -- defers to service task
    -> ice_reset_subtask()       -- picks largest pending reset
      -> ice_do_reset()
        -> ice_prepare_for_reset()
        -> ice_reset(hw, type)   -- writes GLGEN_RTRIG register
        -> ice_check_reset()     -- polls GLGEN_RSTAT + GLNVM_ULD
        -> ice_rebuild()         -- reconstructs driver state
```

Reset types (in order of severity):
- `ICE_RESET_PFR` -- PF reset (PFGEN_CTRL register)
- `ICE_RESET_CORER` -- Core reset (GLGEN_RTRIG)
- `ICE_RESET_GLOBR` -- Global reset (GLGEN_RTRIG)
- `ICE_RESET_EMPR` -- EMP reset (firmware-initiated only)

External reset detection: The OICR interrupt handler detects
firmware-initiated resets and sets `ICE_*_RECV` state bits.

**Recovery mode**: `ice_is_recovery_mode` / `ice_probe_recovery_mode`
handles cases where firmware is in recovery (limited to devlink-only
probe for NVM update).

### What ice Lacks

1. **No health buffer polling** -- No periodic heartbeat monitoring
2. **No syndrome-based root cause** -- No firmware crash diagnostics
3. **No automated recovery** -- Reporters are report-only
4. **No firmware tracing** -- No continuous FW log collection
5. **No resource dump** -- No ability to dump internal FW/HW state
6. **No data path auto-recovery** -- TX hangs trigger reset, not
   per-queue recovery
7. **No severity levels** -- Binary healthy/unhealthy state
8. **No multi-PF reset coordination** -- No semaphore-based SW reset
9. **No FW assertion data** -- No crash variables, call stack, PC
10. **No VNIC diagnostics** -- No queue overrun, steering discard,
    ICM consumption metrics

## Gap Analysis

| Capability | mlx5 | ice | Gap Severity |
|-----------|------|-----|-------------|
| Health buffer polling | Timer-based with jitter | None | **Critical** |
| Syndrome codes | 12+ specific FW syndromes | ~25 link/port codes | **High** |
| FW crash diagnostics | assert_var, callra, exit_ptr, irisc | None | **Critical** |
| Severity levels | 8 levels (EMERG-DEBUG) | None | **High** |
| Automated recovery | fw_fatal recover + data path | None ("no auto recovering") | **Critical** |
| FW tracer | Continuous trace collection | None | **High** |
| Resource dump | CR-dump binary collection | None | **High** |
| Data path reporters | TX + RX per-queue | TX hang only (dump-only) | **High** |
| VNIC diagnostics | Queue counters, steering, ICM | None | **Medium** |
| Multi-PF coordination | SW reset semaphore | None | **Medium** |
| Grace periods | Role-based (ECPF/PF/VF) | None | **Medium** |
| Per-queue recovery | SQ error->reset->ready | Full reset only | **High** |

## Recommendations for fdice_ Health Monitoring

### 1. Adopt mlx5's Multi-Layer Health Architecture

Implement `struct fdice_health` with:
- Timer-based health buffer polling (E810 has similar MMIO health registers)
- Dedicated workqueue for health processing
- Separate fatal vs non-fatal work items

### 2. Build Syndrome-Based Diagnostics

Combine both approaches:
- Map E810's FW health status codes (from ice's AQ events) to a richer
  diagnostic framework
- Add E810-specific hardware sensor checking (PCI liveness, FW heartbeat)
- Implement severity classification for all error types
- Store diagnostic context (not just status codes) for post-mortem analysis

### 3. Implement Five Devlink Health Reporters

```
fdice_health_reporters:
  fw:        FW health status + E810 AQ health events (diagnose + dump)
  fw_fatal:  Fatal errors requiring recovery (diagnose + dump + recover)
  tx:        Per-queue TX data path errors (diagnose + dump + recover)
  rx:        Per-queue RX data path errors (diagnose + dump + recover)
  port:      Port/link health from FW events (diagnose + dump)
```

### 4. Implement Automated Recovery

This is the **single biggest improvement** over ice:
- `fw_fatal` reporter with `recover` callback that performs device reload
- TX reporter with per-SQ recovery (error -> reset -> ready via AQ)
- RX reporter with per-RQ recovery
- Channel-level recovery (reopen channels without full reset)
- Grace periods based on function role (PF vs VF)

### 5. Add FW Diagnostic Data Collection

Even though E810 may not expose assertion variables like ConnectX,
fdice_ should:
- Collect all available AQ error information
- Capture register state snapshots on error
- Maintain error history ring buffer
- Implement structured diagnostic dump format via devlink fmsg

### 6. Implement Health-Aware Reset Coordination

Improve over ice's simple reset by:
- Pre-reset diagnostic capture
- Reset type escalation (per-queue -> PF -> core -> global)
- Post-reset health validation before declaring success
- Reset failure tracking with escalation policy

### 7. Design Error History & Metrics

Neither driver does this well. fdice_ should track:
- Error frequency and patterns (rate limiting, burst detection)
- Recovery success/failure statistics
- Time-to-recovery metrics
- Error correlation across subsystems

### Module Layout

```
fdice_health_core.c    -- health init/cleanup, timer, workqueue, sensor checks
fdice_health_fw.c      -- fw + fw_fatal reporters, AQ health event processing
fdice_health_tx.c      -- TX data path reporter with per-queue recovery
fdice_health_rx.c      -- RX data path reporter with per-queue recovery
fdice_health_port.c    -- port reporter, link health events
fdice_health_diag.c    -- diagnostic data collection, register snapshots
fdice_health.h         -- struct fdice_health, reporter ops, syndrome defs
```

### Priority

This is a **Tier 1 improvement vector**. ice's lack of automated recovery
and per-queue health reporting means users experience full device resets
for problems that could be handled locally. mlx5's architecture proves
that fine-grained health monitoring with targeted recovery dramatically
improves reliability and availability.
