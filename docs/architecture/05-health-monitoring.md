<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Architecture 05: Health Monitoring

**References**: Exploration 07 (Devlink Integration), Exploration 08 (Health
Monitoring & Error Recovery)

## Design Philosophy

Health monitoring is a **Tier 1 improvement vector** over ice. The ice driver
explicitly states "we do not do auto recovering" -- all four of its health
reporters are observe-only with no recovery callbacks. FrankenDrICE adopts
mlx5's multi-layered health architecture and adapts it for E810 hardware,
providing automated recovery at every level from per-queue data path errors
to fatal firmware failures.

The system is organized into **six layers**, each addressing a different
failure domain. This exceeds both reference drivers: mlx5 has five effective
layers (health buffer polling, syndrome diagnostics, devlink reporters, data
path recovery, firmware tracing), while ice has two (devlink reporters and
the basic reset mechanism).

## 6-Layer Health System

### Layer 1: Hardware Health Buffer Polling

A timer-based poller reads E810 health registers via MMIO at jittered intervals,
detecting firmware heartbeat loss and hardware sensor failures before any
software-visible error occurs.

```
fdice_health_poll_timer() [jittered interval, ~2 seconds]
  -> fdice_health_check_sensors()
     -> fdice_sensor_pci_liveness()      -- ioread32 != 0xFFFFFFFF
     -> pci_channel_offline()            -- PCI AER check
     -> fdice_sensor_fw_heartbeat()      -- health counter increment check
     -> fdice_sensor_aq_responsive()     -- AQ command timeout detection
  -> fdice_health_check_fw_status()
     -> read FW health status via AQ (opcode 0xFF22)
     -> compare against cached state for new events
  -> if fatal sensor triggered:
     -> schedule fdice_health_fatal_work()
  -> if non-fatal event:
     -> schedule fdice_health_fw_work()
```

**Heartbeat mechanism**: The E810 firmware maintains a health counter that
increments periodically. The driver reads this counter each poll cycle and
tracks missed increments. Three consecutive misses trigger a firmware health
report. This is adapted from mlx5's `miss_counter` pattern, applied to E810's
AQ-based health status rather than mlx5's MMIO health buffer.

### Layer 2: Syndrome-Based Diagnostics

FrankenDrICE maps E810 firmware health status codes to a structured diagnostic
framework combining ice's ~25 link/port status codes with mlx5-inspired
severity classification.

```c
/**
 * struct fdice_health_syndrome - Structured error diagnostic
 */
struct fdice_health_syndrome {
    u32 code;                          /* E810 health status code */
    enum fdice_health_severity severity;
    const char *description;           /* Human-readable description */
    const char *solution;              /* Suggested remediation */
    const char *data_labels[2];        /* Labels for diagnostic data fields */
    u32 data[2];                       /* Diagnostic data values */
    u64 timestamp;                     /* Kernel timestamp of detection */
};

enum fdice_health_severity {
    FDICE_HEALTH_SEV_DEBUG = 0,        /* Informational only */
    FDICE_HEALTH_SEV_INFO,             /* Normal operational event */
    FDICE_HEALTH_SEV_NOTICE,           /* Noteworthy condition */
    FDICE_HEALTH_SEV_WARNING,          /* Degraded but operational */
    FDICE_HEALTH_SEV_ERROR,            /* Recoverable error */
    FDICE_HEALTH_SEV_CRITICAL,         /* Service-affecting error */
    FDICE_HEALTH_SEV_EMERGENCY,        /* Immediate action required */
    FDICE_HEALTH_SEV_FATAL,            /* Unrecoverable, reset needed */
};
```

### Syndrome Lookup Table

The syndrome table covers E810-specific health codes from ice's firmware health
status event processing, augmented with hardware sensor conditions:

| Code Range | Domain | Examples |
|------------|--------|----------|
| 0x0001-0x000F | Module errors | Unsupported module, wrong type, not qualified |
| 0x0010-0x001F | Link errors | Invalid config, parallel fault, PHY limited |
| 0x0020-0x002F | Netlist errors | Topology corruption, unrecoverable |
| 0x0030-0x003F | Flash/NVM errors | Access failure, authentication failure |
| 0x0040-0x004F | DDP/OROM errors | Authentication, compatibility |
| 0x1000-0x100F | Sensor errors | PCI liveness, FW heartbeat, AQ timeout |
| 0x2000-0x200F | Data path errors | TX timeout, RX stall, CQ overrun |

### Layer 3: Devlink Health Reporters

Five distinct devlink health reporters provide user-space visibility and
automated recovery through the standard `devlink health` interface.

#### fw Reporter

**Scope**: Non-fatal firmware health events (status code changes, AQ errors).
**Callbacks**: `diagnose`, `dump`
**Auto-recovery**: No (events are informational)

Reports firmware health status codes received via AQ health status events
(opcode 0xFF22). The diagnose callback reads current firmware state and
recent syndrome history. The dump callback collects extended diagnostic data
including AQ error counters and firmware version information.

```
fdice_health_fw_report()
  -> devlink_health_report(fw_reporter, ...)
     -> diagnose: fdice_health_fw_diagnose()
        -> report current syndrome via devlink fmsg
        -> report FW version, AQ stats, health counter
     -> dump: fdice_health_fw_dump()
        -> collect AQ error log
        -> collect recent syndrome ring buffer
```

#### fw_fatal Reporter

**Scope**: Fatal firmware errors requiring device recovery.
**Callbacks**: `diagnose`, `dump`, `recover`
**Auto-recovery**: Yes, with configurable grace period

This is the **single biggest improvement** over ice. When a fatal error is
detected (PCI liveness failure, persistent FW heartbeat loss, critical AQ
failure), the fw_fatal reporter triggers automated recovery.

```
fdice_health_fatal_work()
  -> fdice_health_enter_error_state()
     -> flush AQ command interface
     -> notify subsystems (RDMA pre-reset event)
  -> devlink_health_report(fw_fatal_reporter, ...)
     -> dump: fdice_health_fw_fatal_dump()
        -> capture register snapshot
        -> capture AQ state
        -> capture health buffer contents
     -> recover: fdice_health_fw_fatal_recover()
        -> fdice_health_try_recover()
           -> wait for PCI to become accessible
           -> fdice_recover_device()  -- full reload cycle
              -> fdice_unload()
              -> fdice_function_setup()
              -> fdice_load()
              -> fdice_register_device()
```

**Grace period**: Configurable per function role:
- PF: 60 seconds (default)
- VF: 30 seconds (default)

During the grace period, the reporter waits for user-space acknowledgment
via `devlink health recover`. If no acknowledgment arrives and auto-recovery
is enabled, recovery proceeds automatically.

#### tx Reporter (Per-Queue)

**Scope**: Per-queue TX data path errors (timeouts, descriptor errors).
**Callbacks**: `diagnose`, `dump`, `recover`
**Auto-recovery**: Yes, per-queue granularity

Unlike ice's device-level `tx_hang` reporter that can only observe, the
fdice_ tx reporter performs **per-queue recovery** without a full device reset.

```
fdice_health_tx_timeout_detect()  [from NAPI poll or watchdog]
  -> devlink_health_report(tx_reporter, ...)
     -> diagnose: fdice_health_tx_diagnose()
        -> report SQ state (head, tail, consumer/producer counters)
        -> report CQ state (consumer index, error flags)
        -> report pending DMA mappings
     -> dump: fdice_health_tx_dump()
        -> dump full TX descriptor ring
        -> dump pending SKB info
        -> dump associated CQ entries
     -> recover: fdice_health_tx_recover()
        -> fdice_health_sq_to_ready()
           -> disable TX queue via AQ (dis_txqs, 0x0C31)
           -> flush pending completions
           -> reset queue state (head=tail=0)
           -> re-enable TX queue via AQ (add_txqs, 0x0C30)
           -> resume NAPI processing
```

**State machine for SQ recovery**:
```
ACTIVE -> ERROR (timeout/CQE error detected)
  -> RESETTING (disable queue via AQ)
    -> READY (re-enable queue via AQ, resume traffic)
```

If per-queue recovery fails, the reporter escalates to channel-level recovery
(reopen the full channel), and if that fails, to PF-level reset.

#### rx Reporter (Per-Queue)

**Scope**: Per-queue RX data path errors (stalls, buffer allocation failures).
**Callbacks**: `diagnose`, `dump`, `recover`
**Auto-recovery**: Yes, per-queue granularity

```
fdice_health_rx_stall_detect()  [from NAPI poll]
  -> devlink_health_report(rx_reporter, ...)
     -> diagnose: fdice_health_rx_diagnose()
        -> report RQ state (head, tail, fill level)
        -> report page pool stats (alloc failures, recycling)
        -> report associated CQ state
     -> dump: fdice_health_rx_dump()
        -> dump RX descriptor ring
        -> dump buffer allocation state
     -> recover: fdice_health_rx_recover()
        -> flush RX queue
        -> reallocate buffers
        -> reset queue pointers
        -> re-enable queue
```

#### port Reporter

**Scope**: Port-level and link health events from firmware.
**Callbacks**: `diagnose`, `dump`
**Auto-recovery**: No (link events are infrastructure-dependent)

Reports link-related health codes: module errors, PHY problems, link
configuration issues. Uses ice's extensive health status lookup table
for human-readable descriptions and suggested solutions.

```
fdice_health_port_event()  [from AQ health status event]
  -> devlink_health_report(port_reporter, ...)
     -> diagnose: fdice_health_port_diagnose()
        -> report link state, speed, FEC mode
        -> report module type and status
        -> report recent link event history
     -> dump: fdice_health_port_dump()
        -> PHY capability dump
        -> link topology dump
        -> module EEPROM data (if accessible)
```

### Layer 4: Per-Queue Data Path Recovery

The per-queue recovery state machine operates independently of the devlink
health reporter interface, providing the low-level mechanics that the tx and
rx reporters invoke.

```c
/**
 * enum fdice_queue_health_state - Per-queue health state machine
 */
enum fdice_queue_health_state {
    FDICE_QH_ACTIVE,          /* Normal operation */
    FDICE_QH_ERROR,           /* Error detected, pending recovery */
    FDICE_QH_RECOVERING,      /* Recovery in progress */
    FDICE_QH_READY,           /* Recovery complete, resuming */
    FDICE_QH_FAILED,          /* Recovery failed, escalate */
};
```

**Recovery flow**:
```
Per-queue error detected (CQE error, timeout, buffer exhaustion)
  |
  v
ACTIVE -> ERROR
  |
  +-- Per-queue recovery attempt (disable/reset/enable via AQ)
  |     |
  |     +-- Success -> READY -> ACTIVE
  |     |
  |     +-- Failure -> FAILED
  |
  +-- Escalation: channel recovery (reopen channel)
  |     |
  |     +-- Success -> ACTIVE
  |     |
  |     +-- Failure -> FAILED
  |
  +-- Escalation: PF reset (fdice_schedule_reset, ICE_RESET_PFR)
  |     |
  |     +-- Success -> ACTIVE (after rebuild)
  |     |
  |     +-- Failure -> FAILED
  |
  +-- Escalation: Core reset (CORER)
  |     |
  |     +-- Success -> ACTIVE (after rebuild)
  |     |
  |     +-- Failure -> Global reset (GLOBR) -> device offline
```

### Layer 5: Error History and Reset Escalation

FrankenDrICE maintains a persistent error history that tracks patterns across
time, enabling intelligent escalation decisions.

```c
/**
 * struct fdice_health_history - Error history tracking
 */
struct fdice_health_history {
    /* Ring buffer of recent events */
    struct fdice_health_event events[FDICE_HEALTH_HISTORY_SIZE];
    unsigned int head;
    unsigned int count;
    spinlock_t lock;

    /* Escalation tracking */
    unsigned int queue_recoveries;      /* per-queue recoveries in window */
    unsigned int channel_recoveries;    /* channel recoveries in window */
    unsigned int pf_resets;             /* PF resets in window */
    unsigned int core_resets;           /* core resets in window */
    unsigned long window_start;         /* jiffies of current window */

    /* Recovery statistics */
    u64 total_recoveries;
    u64 total_recovery_failures;
    u64 total_escalations;
    u64 recovery_time_sum_ns;          /* for average calculation */
};

struct fdice_health_event {
    u64 timestamp;
    u32 syndrome_code;
    enum fdice_health_severity severity;
    enum fdice_reset_type recovery_type;  /* what recovery was attempted */
    bool recovery_success;
};
```

**Escalation policy**: Within a configurable time window (default: 5 minutes):
- >3 per-queue recoveries on the same queue -> escalate to channel recovery
- >2 channel recoveries on the same channel -> escalate to PF reset
- >2 PF resets -> escalate to core reset
- >1 core reset -> escalate to global reset and mark device offline

### Layer 6: Devlink Health Reporter Integration

All five reporters are registered with devlink during `fdice_load()` and
attached to the appropriate devlink port (per-port granularity for tx/rx
reporters, device-level for fw/fw_fatal/port reporters).

```c
/**
 * struct fdice_health - Top-level health monitoring state
 */
struct fdice_health {
    struct fdice_dev *dev;

    /* Health polling timer */
    struct timer_list poll_timer;
    unsigned long poll_jiffies;        /* jittered interval */

    /* Work items */
    struct work_struct fatal_work;
    struct work_struct fw_work;
    struct workqueue_struct *wq;       /* dedicated health workqueue */

    /* Devlink health reporters */
    struct devlink_health_reporter *fw_reporter;
    struct devlink_health_reporter *fw_fatal_reporter;
    struct devlink_health_reporter *port_reporter;
    /* tx/rx reporters are per-channel, stored in fdice_channel */

    /* Syndrome state */
    struct fdice_health_syndrome last_syndrome;
    u32 fw_health_counter;             /* last observed FW heartbeat */
    unsigned int miss_count;           /* consecutive heartbeat misses */

    /* Error history */
    struct fdice_health_history history;

    /* Configuration */
    unsigned int fatal_grace_period_ms;
    bool auto_recover_enabled;
};

/**
 * struct fdice_health_reporter - Per-reporter configuration
 */
struct fdice_health_reporter {
    struct devlink_health_reporter *dl_reporter;
    const struct devlink_health_reporter_ops *ops;
    const char *name;
    u64 report_count;
    u64 recover_count;
    u64 recover_fail_count;
};
```

## Module Layout

```
src/health/
  fdice_health_core.c   -- Health init/cleanup, timer, workqueue, sensor checks
  fdice_health_core.h   -- fdice_health, fdice_health_syndrome, history structs
  fdice_health_fw.c     -- fw + fw_fatal reporters, AQ health event processing
  fdice_health_tx.c     -- TX reporter with per-queue recovery
  fdice_health_rx.c     -- RX reporter with per-queue recovery
  fdice_health_port.c   -- Port reporter, link health events
```

## Key Structures Summary

| Structure | Role |
|-----------|------|
| `fdice_health` | Top-level health state: timer, workqueue, reporters, history |
| `fdice_health_reporter` | Per-reporter statistics and configuration |
| `fdice_health_syndrome` | Structured diagnostic for a specific error event |
| `fdice_health_history` | Ring buffer of recent events with escalation tracking |
| `fdice_health_event` | Single event record in the history buffer |
| `fdice_queue_health_state` | Per-queue recovery state machine |

## Comparison with Reference Drivers

| Capability | mlx5 | ice | fdice_ |
|-----------|------|-----|--------|
| Health buffer polling | Timer with jitter | None | Timer with jitter (adapted for E810) |
| Syndrome codes | 12+ FW syndromes | ~25 link/port codes | Combined: FW + link + sensor + data path |
| Severity levels | 8 levels | None | 8 levels (matching kernel log levels) |
| Auto-recovery | fw_fatal + data path | None | fw_fatal + tx + rx (all with recover) |
| Per-queue recovery | SQ error->reset->ready | Full reset only | SQ/RQ error->reset->ready via AQ |
| Data path reporters | TX + RX per-port | TX hang only (dump-only) | TX + RX per-queue with recover |
| Error history | Not tracked | Not tracked | Ring buffer with escalation policy |
| Reset escalation | PF -> core | PF -> core -> global | Per-queue -> channel -> PF -> core -> global |
| Grace periods | Role-based (ECPF/PF/VF) | None | Role-based (PF/VF) |
| FW crash diagnostics | assert_var, callra | None | Register snapshot, AQ state capture |
