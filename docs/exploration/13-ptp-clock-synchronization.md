<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration Q13: PTP/Clock Synchronization

## Overview

The E810 has sophisticated PTP (Precision Time Protocol) hardware support
including a PHC (PTP Hardware Clock), per-port TX/RX timestamping in the PHY,
cross-timestamping support, configurable GPIO pins for PPS/external timestamps,
and Vernier calibration for sub-nanosecond accuracy. The ice driver implements
this through approximately 3400 lines in `ice_ptp.c` and 5600 lines in
`ice_ptp_hw.c`.

## Source Files Explored

- `ice_ptp.h` -- PTP data structures, state machine, API declarations
- `ice_ptp.c` -- PTP clock registration, PHC operations, timestamp processing
- `ice_ptp_hw.h` -- Hardware-level PTP types (serdes, link speed, FEC, timer commands)
- `ice_ptp_hw.c` -- PHY register access, timer operations, Vernier calibration
- `ice_tspll.c` -- Timestamp PLL initialization (`ice_tspll_init`)
- `ice_main.c` -- PTP integration points (`ice_init_features`, `ice_misc_intr_thread_fn`)

## PTP State Machine

```
enum ice_ptp_state {
    ICE_PTP_UNINIT = 0,    // Not initialized
    ICE_PTP_INITIALIZING,  // Init in progress
    ICE_PTP_READY,         // Operational
    ICE_PTP_RESETTING,     // Reset in progress
    ICE_PTP_ERROR,         // Error state
};
```

State transitions:
- `UNINIT -> INITIALIZING -> READY` (normal init path)
- `READY -> RESETTING -> READY` (reset recovery)
- `Any -> ERROR` (unrecoverable failure)
- `Any -> UNINIT` (on init failure, avoids duplicate cleanup)

## Core Data Structures

### `struct ice_ptp` (Top-Level PTP State)

Embedded in `struct ice_pf`, this aggregates all PTP state:

- **state** -- Current state machine position
- **tx_interrupt_mode** -- NONE, SELF, or ALL (how TX timestamp interrupts work)
- **port** -- Per-port PTP data (`ice_ptp_port`)
- **work** -- Delayed kthread work for periodic PHC cache updates
- **cached_phc_time / cached_phc_jiffies** -- Cached PHC time for RX timestamp extension
- **kworker** -- Dedicated kernel worker thread
- **pin_desc / ice_pin_desc** -- GPIO pin configuration
- **perout_rqs / extts_rqs** -- Cached periodic output and external timestamp requests
- **info** -- `ptp_clock_info` ops structure (registered with PTP subsystem)
- **clock** -- The `ptp_clock` device pointer
- **tstamp_config** -- Current hardware timestamping configuration
- **Statistics counters**: tx_hwtstamp_good, skipped, timeouts, flushed, discarded

### `struct ice_ptp_port` (Per-Port State)

- **tx** -- `ice_ptp_tx` for TX timestamp tracking
- **ov_work** -- Delayed work for PHY offset validation
- **ps_lock** -- Mutex protecting PHY start procedure
- **link_up** -- Link state
- **tx_fifo_busy_cnt** -- TX FIFO busy counter
- **port_num** -- Physical port number

### `struct ice_ptp_tx` (TX Timestamp Tracking)

Per-port tracking for outstanding TX timestamp requests:

- **lock** -- Spinlock protecting the bitmap
- **tstamps** -- Array of `ice_tx_tstamp` (SKB pointer + start jiffies + cached value)
- **in_use / stale** -- Bitmaps tracking active and stale slots
- **block / offset / len** -- PHY timestamp register block parameters
- **init / calibrating / has_ready_bitmap** -- Status bitfields
- **last_ll_ts_idx_read** -- Last low-latency timestamp index read

E810 vs E822 timestamp block layout differs significantly:
- **E810**: Each port has its own dedicated block of 64 timestamp registers
- **E822**: Shared register blocks split across ports in quads (16 per port)

## PTP Initialization Sequence

### `ice_ptp_init()` -- Main Entry

Called from `ice_init_features()` during probe. Sequence:

1. **`ice_ptp_init_hw()`** -- Initialize PHY-level PTP hardware, detect
   capabilities based on MAC type (E810/E822/E830)

2. **`ice_ptp_init_tx_interrupt_mode()`** -- Configure TX timestamp interrupt
   handling mode based on hardware capabilities

3. **Source timer owner check** -- Only the PF that owns the source timer AND
   is the primary function initializes the clock:
   - `ice_ptp_setup_adapter()` -- Adapter-level setup
   - `ice_ptp_init_owner()` -- Full owner initialization

4. **`ice_ptp_setup_pf()`** -- PF-level setup (all PFs, not just owner)

5. **`ice_ptp_init_port()`** -- Initialize per-port TX timestamp tracking

6. **`ice_ptp_reset_phy_timestamping()`** -- Reset PHY timestamp block

7. **`ice_ptp_cfg_tx_interrupt()`** -- Configure TX timestamp interrupts

8. **Set state to `ICE_PTP_READY`**

9. **`ice_ptp_init_work()`** -- Create kthread worker for periodic tasks

### `ice_ptp_init_owner()` -- Clock Owner Initialization

Only runs on the PF that owns the source timer:

1. **`ice_tspll_init()`** -- Initialize the Timestamp PLL (CGU)

2. **Acquire global PTP lock** (`ice_ptp_lock()`) -- Semaphore-based hardware
   lock in PFTSYN registers

3. **`ice_ptp_write_incval()`** -- Write the timer increment value to PHY and
   LAN. The increment value determines timer tick rate (base_incval from
   `ice_base_incval()` which is PLL-frequency dependent).

4. **`ice_ptp_write_init()`** -- Write initial time (current wall clock) to
   PHY and LAN registers

5. **Release PTP lock**

6. **`ice_ptp_cfg_phy_interrupt()`** -- Configure PHY interrupt for timestamp
   ready notifications

7. **`ice_ptp_create_clock()`** -- Register PTP clock device

## PHC (PTP Hardware Clock) Operations

### Clock Registration (`ice_ptp_create_clock`)

Calls `ice_ptp_set_caps()` to populate `ptp_clock_info`, then registers with
`ptp_clock_register()`.

### Capability Setup (`ice_ptp_set_caps`)

Common capabilities set for all MAC types:
- **owner** -- THIS_MODULE
- **max_adj** -- 100000000 (100 MHz max frequency adjustment)
- **adjfine** -- `ice_ptp_adjfine`
- **adjtime** -- `ice_ptp_adjtime`
- **gettimex64** -- `ice_ptp_gettimex64`
- **settime64** -- `ice_ptp_settime64`
- **n_per_out** -- 4 (GLTSYN_TGT_H_IDX_MAX)
- **n_ext_ts** -- 3 (GLTSYN_EVNT_H_IDX_MAX)
- **enable** -- `ice_ptp_gpio_enable`
- **verify** -- `ice_verify_pin`
- **supported_extts_flags** -- PTP_RISING_EDGE | PTP_FALLING_EDGE | PTP_STRICT_FLAGS
- **supported_perout_flags** -- PTP_PEROUT_PHASE

Then dispatches to MAC-specific setup:

#### E810 (`ice_ptp_set_funcs_e810`)
- Configures GPIO pins based on feature detection:
  - If DPLL feature: uses `ice_pin_desc_dpll`
  - Otherwise: uses `ice_pin_desc_e810`
  - If SDP (Signal Delivery Protocol) entries available: parses from NVM
- Sets up pin configuration and SDP entry parsing
- Reads SDP access control (`ice_ptp_read_sdp_ac`)

#### E822 (`ice_ptp_set_funcs_e82x`)
- Different pin descriptors for the generic MAC

#### E830 (`ice_ptp_set_funcs_e830`)
- Newest generation with potentially different capabilities

### PHC Operations Detail

**`ice_ptp_adjfine()`** -- Fine frequency adjustment using scaled_ppm.
Modifies the increment value to adjust clock frequency.

**`ice_ptp_adjtime()`** -- Time offset adjustment. Applies a signed
nanosecond offset to the hardware clock. Uses the PTP hardware lock
to ensure atomicity across PHY and LAN timers.

**`ice_ptp_gettimex64()`** -- Read current PHC time with system timestamp
correlation for `PTP_SYS_OFFSET_EXTENDED` support.

**`ice_ptp_settime64()`** -- Set the PHC to an absolute time value.
Requires PTP lock for atomic PHY+LAN update.

### Cross-Timestamping (`ice_ptp_getcrosststamp`)

E810 and E830 support `get_device_system_crosststamp()` for precise
correlation between device time and system time:

- Uses `ice_capture_crosststamp` callback
- E810: Standard cross-timestamp configuration
- E830: Enhanced cross-timestamp via `CONFIG_ICE_HWTS` with dedicated config
- Returns `-EOPNOTSUPP` for unsupported MAC types

This enables `PTP_SYS_OFFSET_PRECISE` for applications needing the highest
accuracy system-to-device time correlation.

## TX Timestamp Mechanism

### Request Flow

1. **`ice_ptp_request_ts()`** -- Called from TX path when a packet needs
   timestamping. Allocates an index from the `in_use` bitmap under spinlock.
   Returns the index (or -1 if no slots available).

2. **Packet transmission** -- The allocated index is written into the TX
   descriptor so the hardware knows where to store the timestamp.

3. **Interrupt** -- When the PHY captures the TX timestamp:
   - `ice_misc_intr_thread_fn()` detects the PTP timestamp interrupt
   - Calls `ice_ptp_process_ts()`

### Processing Flow (`ice_ptp_process_ts`)

Based on `tx_interrupt_mode`:

- **`ICE_PTP_TX_INTERRUPT_SELF`**: Calls `ice_ptp_process_tx_tstamp()` for
  the local port's TX timestamps
- **`ICE_PTP_TX_INTERRUPT_ALL`**: Calls `ice_ptp_tx_tstamp_owner()` which
  iterates all ports (multi-PF timestamp ownership)

### `ice_ptp_process_tx_tstamp()`

The core TX timestamp completion handler:

1. Iterates the `in_use` bitmap
2. Reads the timestamp value from PHY registers
3. For PHYs without ready bitmaps: compares against `cached_tstamp` to detect
   new values
4. Converts the raw 40/64-bit timestamp to `skb_tstamp_ns`
5. Delivers the timestamp to the networking stack via `skb_tstamp_tx()`
6. Clears the `in_use` bit
7. 2-second timeout: discards SKBs that never got timestamps

### Single Timestamp Mode

For low-latency timestamping:
- `ice_ptp_req_tx_single_tstamp()` -- Request a single timestamp
- `ice_ptp_complete_tx_single_tstamp()` -- Complete a single timestamp

## RX Timestamp Mechanism

### `ice_ptp_get_rx_hwts()`

RX timestamps are extracted from the flexible RX descriptor:

- The descriptor contains a 40-bit timestamp in its flex fields
- This partial timestamp is extended to 64-bit using the cached PHC time
- The cached PHC time is periodically updated by `ice_ptp_periodic_work`
- Extension handles wrap-around of the 40-bit value

### PHC Time Caching

A kthread worker (`ice_ptp_periodic_work`) runs periodically to:
1. Read the current PHC time
2. Store it in `ptp->cached_phc_time` with `ptp->cached_phc_jiffies`
3. This cached value is used for RX timestamp extension without needing
   an expensive PHC read per packet

## GPIO Pin Support

### Pin Types

```
enum ice_ptp_pin {
    SDP0 = 0, SDP1, SDP2, SDP3,
    TIME_SYNC,
    ONE_PPS
};

enum ice_ptp_pin_nvm {
    GNSS = 0, SMA1, UFL1, SMA2, UFL2,
    NUM_PTP_PINS_NVM,
    GPIO_NA = 9
};
```

### Pin Configuration

Up to `ICE_N_PINS_MAX` (6) pins supported. Each pin described by:

```
struct ice_ptp_pin_desc {
    int name_idx;       // Index into name table
    int gpio[2];        // Associated GPIO input/output pins
    unsigned int delay[2];  // Input/output signal delays (ns)
};
```

### Per-Channel Registers

Extensive per-channel register macros for configuring timer targets, clock
outputs, auxiliary I/O, and event timestamps:

- `GLTSYN_AUX_OUT(chan, idx)` -- Auxiliary output configuration
- `GLTSYN_AUX_IN(chan, idx)` -- Auxiliary input configuration
- `GLTSYN_CLKO(chan, idx)` -- Clock output
- `GLTSYN_TGT_L/H(chan, idx)` -- Target time (low/high)
- `GLTSYN_EVNT_L/H(chan, idx)` -- Event timestamp (low/high)

### Supported Features

- **Periodic output** (4 channels): 1PPS, frequency synthesis
- **External timestamps** (3 channels): Capture external events
- **Rising/falling edge** detection
- **Phase-based periodic output** (`PTP_PEROUT_PHASE`)

## E810-Specific PTP Hardware Features

### Timer Architecture

- **GLTSYN registers**: Global timestamp synchronization registers shared
  across PFs
- **Source timer ownership**: Only one PF controls the master timer
- **PTP lock**: Hardware semaphore (`PFTSYN_SEM`) for atomic timer operations
  across multiple PFs. 4-byte semaphore with retry logic in `ice_ptp_lock()`.

### PHY Timestamp Blocks

E810 has dedicated per-port timestamp register blocks (64 entries each),
unlike E822 which shares blocks across quads.

### Timer Commands (`ice_ptp_tmr_cmd`)

```
ICE_PTP_INIT_TIME       -- Initialize timer to absolute value
ICE_PTP_INIT_INCVAL     -- Initialize increment value
ICE_PTP_ADJ_TIME        -- Adjust time by offset
ICE_PTP_ADJ_TIME_AT_TIME -- Adjust at specific time
ICE_PTP_READ_TIME       -- Read current time
ICE_PTP_NOP             -- No operation
```

### Vernier Calibration (E822)

The `ice_vernier_info_e82x` structure contains per-speed calibration data:
- TX/RX parallel clock frequencies
- TX/RX PCS clock frequencies
- Deskew/Reed-Solomon gearbox clock frequencies
- Fixed TX/RX delays (in 1/100th nanoseconds)
- PMD alignment divisors

This enables sub-nanosecond timestamp accuracy through Vernier offset
calibration tracked by `ice_ptp_port.ov_work`.

### Link Speed Dependencies

PTP calibration varies by link speed:

```
enum ice_ptp_link_spd {
    ICE_PTP_LNK_SPD_1G, ICE_PTP_LNK_SPD_10G,
    ICE_PTP_LNK_SPD_25G, ICE_PTP_LNK_SPD_25G_RS,
    ICE_PTP_LNK_SPD_40G, ICE_PTP_LNK_SPD_50G,
    ICE_PTP_LNK_SPD_50G_RS, ICE_PTP_LNK_SPD_100G_RS,
};
```

### FEC Mode Impact

```
enum ice_ptp_fec_mode {
    ICE_PTP_FEC_MODE_NONE,
    ICE_PTP_FEC_MODE_CLAUSE74,
    ICE_PTP_FEC_MODE_RS_FEC
};
```

FEC mode affects timestamp calibration parameters and deskew calculations.

## Reset and Rebuild

### `ice_ptp_prepare_for_reset()`

Called before reset:
- Sets state to `ICE_PTP_RESETTING`
- Cancels pending work
- Saves clock state

### `ice_ptp_rebuild()`

Called after reset:
- Reinitializes PTP hardware
- Restores timer state
- Reconfigures interrupts
- Returns to `ICE_PTP_READY`

### `ice_ptp_release()`

Cleanup on driver unload:
- Unregisters PTP clock
- Cancels kworker
- Frees TX timestamp tracking resources
- Destroys mutexes

## fdice_ Implementation Approach

### Architecture Decisions

1. **Separate HW abstraction layer**: Create `fdice_ptp_hw.c` as a clean
   hardware abstraction that hides E810-specific register details behind
   a function pointer table. This allows future E830 support without
   touching the core PTP logic.

2. **Simplified state machine**: The ice state machine works well but can
   be made more robust with explicit transition validation.

3. **Unified timestamp processing**: Rather than separate code paths for
   SELF vs ALL interrupt modes, use a single processing path with
   configurable scope.

### Implementation Plan

```
fdice_ptp.h / fdice_ptp.c:
  - fdice_ptp_state enum and state machine
  - fdice_ptp_init() / fdice_ptp_release()
  - fdice_ptp_rebuild() / fdice_ptp_prepare_for_reset()
  - PHC ops: fdice_ptp_adjfine(), fdice_ptp_adjtime(),
    fdice_ptp_gettimex64(), fdice_ptp_settime64()
  - fdice_ptp_getcrosststamp()
  - TX timestamp: fdice_ptp_request_ts(), fdice_ptp_process_ts()
  - RX timestamp: fdice_ptp_get_rx_hwts()
  - GPIO: fdice_ptp_gpio_enable(), fdice_verify_pin()

fdice_ptp_hw.h / fdice_ptp_hw.c:
  - fdice_ptp_init_hw() -- Hardware detection and initialization
  - fdice_ptp_init_phc() -- PHC register setup
  - fdice_ptp_lock() / fdice_ptp_unlock() -- Hardware semaphore
  - fdice_ptp_write_incval() / fdice_ptp_write_init()
  - fdice_ptp_read_time() / fdice_ptp_adj_clock()
  - PHY timestamp register access functions
  - Vernier calibration (if supporting E822)
```

### Improvements Over ice

1. **Cleaner lock hierarchy**: Document and enforce the lock ordering between
   PTP hardware lock, port ps_lock, and TX spinlock to prevent deadlocks.

2. **Better timeout handling**: Replace the 2-second hardcoded TX timestamp
   timeout with a configurable value exposed via ethtool/debugfs.

3. **Structured statistics**: Expose PTP statistics through a dedicated
   interface rather than scattered counters.

4. **Testable PHC operations**: Design the PHC ops layer so that timestamp
   conversion and extension logic can be unit-tested independently.

5. **Improved cross-timestamp support**: Make cross-timestamping the default
   path when hardware supports it, with automatic fallback.

6. **Pin configuration validation**: Add comprehensive validation of pin
   configurations at setup time rather than relying on hardware error returns.

7. **Cached PHC update reliability**: Track and report `late_cached_phc_updates`
   more prominently, as stale cached time directly impacts RX timestamp accuracy.
