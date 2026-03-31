<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 11: E810 Admin Queue Command Interface

> **This is THE hardware abstraction boundary.**
> Everything below the AQ interface must match E810 firmware expectations exactly.
> Everything above it can be redesigned freely using mlx5-inspired patterns.

## 1. Overview

The E810 Admin Queue (AQ) is the primary control-plane interface between the
driver and the firmware. It consists of paired ring buffers: an Admin Transmit
Queue (ATQ) for sending commands and an Admin Receive Queue (ARQ) for receiving
asynchronous events and command completions.

**Key files explored:**
- `kernel/include/linux/net/intel/libie/adminq.h` -- shared AQ descriptor and error codes (libie layer)
- `kernel/drivers/net/ethernet/intel/ice/ice_adminq_cmd.h` -- ice-specific opcodes and parameter structs
- `kernel/drivers/net/ethernet/intel/ice/ice_controlq.h` -- control queue ring structures
- `kernel/drivers/net/ethernet/intel/ice/ice_controlq.c` -- `ice_sq_send_cmd`, `ice_clean_rq_elem`, `ice_fill_dflt_direct_cmd_desc`
- `kernel/drivers/net/ethernet/intel/ice/ice_common.c` -- `ice_aq_send_cmd` wrapper with retry and global cfg lock
- `kernel/drivers/net/ethernet/intel/ice/ice_main.c` -- `__ice_clean_ctrlq` async event processing
- `kernel/drivers/net/ethernet/mellanox/mlx5/core/cmd.c` -- mlx5 command interface for comparison

## 2. AQ Descriptor Format (Hardware-Fixed)

The descriptor is 32 bytes, defined as `struct libie_aq_desc` in the shared
libie library (used by both ice and idpf drivers):

```
Offset  Size  Field          Description
------  ----  -----          -----------
0x00    2     flags          Control flags (DD, CMP, ERR, VFE, LB, RD, VFC, BUF, SI, EI, FE)
0x02    2     opcode         AQ command opcode
0x04    2     datalen        Length of indirect/external data buffer
0x06    2     retval         Firmware return value (libie_aq_err)
0x08    4     cookie_high    Opaque data (driver tracking, e.g., completion token)
0x0C    4     cookie_low     Opaque data (driver tracking)
0x10    16    params         Command-specific parameters (union)
```

### 2.1 Descriptor Flags (Bit Layout)

```
Bit  Name  Direction    Meaning
---  ----  ---------    -------
0    DD    FW->Driver   Descriptor Done -- FW has processed this descriptor
1    CMP   FW->Driver   Command Completion -- response is ready
2    ERR   FW->Driver   Error occurred (check retval)
3    VFE   FW->Driver   VF Error
9    LB    Driver->FW   Large Buffer (data > 512 bytes)
10   RD    Driver->FW   Read direction -- buffer contains data for FW to read
11   VFC   Driver->FW   VF Command
12   BUF   Driver->FW   Buffer present -- indirect command with DMA buffer
13   SI    Driver->FW   Security flag (firmware internal)
14   EI    Driver->FW   Error Interrupt enable
15   FE    Driver->FW   Fatal Error
```

### 2.2 Command Types

- **Direct commands**: All 16 parameter bytes are in the descriptor itself
- **Indirect commands**: BUF flag set, `addr_high`/`addr_low` in params point to DMA buffer,
  `datalen` gives buffer size. Max buffer: 4096 bytes (ICE_AQ_MAX_BUF_LEN)

### 2.3 Error Codes (libie_aq_err)

| Code | Name           | Meaning                              |
|------|----------------|--------------------------------------|
| 0    | RC_OK          | Success                              |
| 1    | RC_EPERM       | Operation not permitted              |
| 2    | RC_ENOENT      | No such element                      |
| 3    | RC_ESRCH       | Bad opcode                           |
| 5    | RC_EIO         | I/O error                            |
| 8    | RC_EAGAIN      | Try again                            |
| 9    | RC_ENOMEM      | Out of memory                        |
| 10   | RC_EACCES      | Permission denied                    |
| 12   | RC_EBUSY       | Device or resource busy              |
| 13   | RC_EEXIST      | Object already exists                |
| 14   | RC_EINVAL      | Invalid argument                     |
| 16   | RC_ENOSPC      | No space / allocation failure        |
| 17   | RC_ENOSYS      | Function not implemented             |
| 21   | RC_EMODE       | Op not allowed in current dev mode   |
| 24   | RC_ENOSEC      | Missing security manifest            |
| 25   | RC_EBADSIG     | Bad RSA signature                    |
| 26   | RC_ESVN        | SVN number prohibits this package    |
| 27   | RC_EBADMAN     | Manifest hash mismatch               |
| 28   | RC_EBADBUF     | Buffer hash mismatches manifest      |

## 3. Full AQ Opcode Map by Category

All opcodes from `enum ice_adminq_opc` in `ice_adminq_cmd.h`:

### 3.1 Core / Resource Management (0x0001-0x000B)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0001 | get_ver                 | direct   | Get firmware/API version       |
| 0x0002 | send_driver_ver         | indirect | Send driver version to FW      |
| 0x0003 | q_shutdown              | direct   | Queue shutdown                 |
| 0x0008 | req_res                 | direct   | Request resource ownership     |
| 0x0009 | release_res             | direct   | Release resource ownership     |
| 0x000A | list_func_caps          | indirect | Get function capabilities      |
| 0x000B | list_dev_caps           | indirect | Get device capabilities        |

**Resource IDs** (for req_res/release_res): NVM (1), SDP (2), Change Lock (3), Global Lock (4)

### 3.2 MAC Management (0x0107-0x0110)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0107 | manage_mac_read         | indirect | Read MAC addresses             |
| 0x0108 | manage_mac_write        | direct   | Write MAC address              |
| 0x0110 | clear_pxe_mode          | direct   | Clear PXE boot mode            |

### 3.3 Internal Switch (0x0200-0x02A4)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0200 | get_sw_cfg              | indirect | Get switch configuration       |
| 0x0203 | set_port_params         | direct   | Set port parameters            |
| 0x0208 | alloc_res               | indirect | Allocate switch resources      |
| 0x0209 | free_res                | indirect | Free switch resources          |
| 0x020B | share_res               | indirect | Share resource with other PF   |
| 0x020C | set_vlan_mode_params    | indirect | Set VLAN mode parameters       |
| 0x020D | get_vlan_mode_params    | indirect | Get VLAN mode parameters       |

### 3.4 VSI Management (0x0210-0x0213)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0210 | add_vsi                 | indirect | Create VSI                     |
| 0x0211 | update_vsi              | indirect | Update VSI properties          |
| 0x0213 | free_vsi                | indirect | Destroy VSI                    |

### 3.5 Recipe / Flow Classification (0x0290-0x0293)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0290 | add_recipe              | indirect | Add flow classification recipe |
| 0x0291 | recipe_to_profile       | direct   | Bind recipe to profile         |
| 0x0292 | get_recipe              | indirect | Get recipe definition          |
| 0x0293 | get_recipe_to_profile   | direct   | Get recipe-profile binding     |

### 3.6 Switch Rules (0x02A0-0x02A4)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x02A0 | add_sw_rules            | indirect | Add switch forwarding rules    |
| 0x02A1 | update_sw_rules         | indirect | Update switch rules            |
| 0x02A2 | remove_sw_rules         | indirect | Remove switch rules            |
| 0x02A4 | clear_pf_cfg            | direct   | Clear PF configuration         |

### 3.7 DCB / PFC (0x0302-0x0303)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0302 | query_pfc_mode          | direct   | Query PFC mode                 |
| 0x0303 | set_pfc_mode            | direct   | Set PFC mode                   |

### 3.8 Transmit Scheduler (0x0400-0x0418)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0400 | get_dflt_topo           | indirect | Get default scheduler topology |
| 0x0401 | add_sched_elems         | indirect | Add scheduler elements         |
| 0x0403 | cfg_sched_elems         | indirect | Configure scheduler elements   |
| 0x0404 | get_sched_elems         | indirect | Get scheduler elements         |
| 0x0408 | move_sched_elems        | indirect | Move scheduler elements        |
| 0x0409 | suspend_sched_elems     | indirect | Suspend scheduler nodes        |
| 0x040A | resume_sched_elems      | indirect | Resume scheduler nodes         |
| 0x040E | query_port_ets          | indirect | Query port ETS config          |
| 0x040F | delete_sched_elems      | indirect | Delete scheduler elements      |
| 0x0410 | add_rl_profiles         | indirect | Add rate limiter profiles      |
| 0x0412 | query_sched_res         | indirect | Query scheduler resources      |
| 0x0415 | remove_rl_profiles      | indirect | Remove rate limiter profiles   |
| 0x0417 | set_tx_topo             | indirect | Set Tx topology                |
| 0x0418 | get_tx_topo             | indirect | Get Tx topology                |

### 3.9 PHY / Link (0x0600-0x06EE)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0600 | get_phy_caps            | indirect | Get PHY capabilities           |
| 0x0601 | set_phy_cfg             | indirect | Set PHY configuration          |
| 0x0603 | set_mac_cfg             | direct   | Set MAC configuration          |
| 0x0605 | restart_an              | direct   | Restart auto-negotiation       |
| 0x0607 | get_link_status         | indirect | Get link status (also event)   |
| 0x0613 | set_event_mask          | direct   | Set link event mask            |
| 0x0620 | set_mac_lb              | direct   | Set MAC loopback               |
| 0x0630 | set_phy_rec_clk_out     | direct   | Set PHY recovered clock output |
| 0x0631 | get_phy_rec_clk_out     | direct   | Get PHY recovered clock output |
| 0x0632 | get_sensor_reading      | direct   | Get sensor reading             |
| 0x0682 | dnl_call                | direct   | DNL call                       |
| 0x06E0 | get_link_topo           | direct   | Get link topology              |
| 0x06E2 | read_i2c                | direct   | Read I2C device                |
| 0x06E3 | write_i2c               | direct   | Write I2C device               |
| 0x06E9 | set_port_id_led         | direct   | Set port identification LED    |
| 0x06EA | get_port_options        | direct   | Get port options               |
| 0x06EB | set_port_option         | direct   | Set port option                |
| 0x06EC | set_gpio                | direct   | Set GPIO pin                   |
| 0x06ED | get_gpio                | direct   | Get GPIO pin                   |
| 0x06EE | sff_eeprom              | direct   | Read/Write SFF EEPROM          |

### 3.10 NVM Management (0x0701-0x070B)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0701 | nvm_read                | indirect | Read NVM                       |
| 0x0702 | nvm_erase               | indirect | Erase NVM sector               |
| 0x0703 | nvm_write               | indirect | Write NVM                      |
| 0x0706 | nvm_checksum            | direct   | Verify NVM checksum            |
| 0x0707 | nvm_write_activate      | direct   | Activate NVM write             |
| 0x0709 | nvm_update_empr         | direct   | NVM update with EMP reset      |
| 0x070A | nvm_pkg_data            | indirect | NVM package data               |
| 0x070B | nvm_pass_component_tbl  | indirect | Pass component table for FW update |

### 3.11 PF/VF Mailbox (0x0801-0x0802)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0801 | send_msg_to_pf          | indirect | VF sends message to PF         |
| 0x0802 | send_msg_to_vf          | indirect | PF sends message to VF         |

### 3.12 LLDP / DCB (0x0A00-0x0A0B)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0A00 | lldp_get_mib            | indirect | Get LLDP MIB                   |
| 0x0A01 | lldp_set_mib_change     | direct   | Enable/disable MIB change event|
| 0x0A05 | lldp_stop               | direct   | Stop LLDP agent                |
| 0x0A06 | lldp_start              | direct   | Start LLDP agent               |
| 0x0A07 | get_cee_dcb_cfg         | indirect | Get CEE DCBX config            |
| 0x0A08 | lldp_set_local_mib      | indirect | Set local LLDP MIB             |
| 0x0A09 | lldp_stop_start_agent   | direct   | Stop/start specific LLDP agent |
| 0x0A0A | lldp_filter_ctrl        | direct   | LLDP filter control            |
| 0x0A0B | lldp_execute_pending    | direct   | Execute pending MIB            |

### 3.13 RSS (0x0B02-0x0B05)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0B02 | set_rss_key             | indirect | Set RSS hash key               |
| 0x0B03 | set_rss_lut             | indirect | Set RSS lookup table           |
| 0x0B04 | get_rss_key             | indirect | Get RSS hash key               |
| 0x0B05 | get_rss_lut             | indirect | Get RSS lookup table           |

### 3.14 Sideband / Tx Queue Management (0x0C00-0x0C35)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0C00 | neighbour_device_req    | indirect | Sideband neighbor device req   |
| 0x0C30 | add_txqs                | indirect | Add Tx LAN queues              |
| 0x0C31 | dis_txqs                | indirect | Disable Tx queues              |
| 0x0C32 | cfg_txqs                | indirect | Configure Tx queues            |
| 0x0C33 | add_rdma_qset           | indirect | Add RDMA queue set             |
| 0x0C35 | set_txtimeqs            | indirect | Set Tx time queues             |

### 3.15 DDP Package Management (0x0C40-0x0C43)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0C40 | download_pkg            | indirect | Download DDP package           |
| 0x0C41 | upload_section          | indirect | Upload package section         |
| 0x0C42 | update_pkg              | indirect | Update package                 |
| 0x0C43 | get_pkg_info_list       | indirect | Get package info list          |

### 3.16 1588 / SyncE / CGU (0x0C59-0x0C6A)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0C59 | get_cgu_input_measure   | direct   | Get CGU input measurement      |
| 0x0C61 | get_cgu_abilities       | direct   | Get CGU abilities              |
| 0x0C62 | set_cgu_input_config    | direct   | Set CGU input configuration    |
| 0x0C63 | get_cgu_input_config    | direct   | Get CGU input configuration    |
| 0x0C64 | set_cgu_output_config   | direct   | Set CGU output configuration   |
| 0x0C65 | get_cgu_output_config   | direct   | Get CGU output configuration   |
| 0x0C66 | get_cgu_dpll_status     | direct   | Get CGU DPLL status            |
| 0x0C67 | set_cgu_dpll_config     | direct   | Set CGU DPLL configuration     |
| 0x0C68 | set_cgu_ref_prio        | direct   | Set CGU reference priority     |
| 0x0C69 | get_cgu_ref_prio        | direct   | Get CGU reference priority     |
| 0x0C6A | get_cgu_info            | direct   | Get CGU info                   |

### 3.17 Misc / Driver Shared (0x0C90)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x0C90 | driver_shared_params    | direct   | Driver shared parameters       |

### 3.18 Events (0x1001)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0x1001 | event_lan_overflow      | event    | LAN overflow event             |

### 3.19 Health / Diagnostics (0xFF20-0xFF22)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0xFF20 | set_health_status_cfg   | direct   | Configure health status        |
| 0xFF22 | get_health_status       | event    | Health status event            |

### 3.20 FW Logging (0xFF30-0xFF33)

| Opcode | Name                    | Type     | Description                    |
|--------|-------------------------|----------|--------------------------------|
| 0xFF30 | fw_logs_config          | indirect | Configure FW logging           |
| 0xFF31 | fw_logs_register        | indirect | Register for FW log events     |
| 0xFF32 | fw_logs_query           | indirect | Query FW logging state         |
| 0xFF33 | fw_logs_event           | event    | FW log event (async)           |

**Total: ~100 unique opcodes across 20 categories.**

## 4. Control Queue Architecture

### 4.1 Three Control Queue Types

The ice driver manages three independent control queue pairs, all using the
same descriptor format and ring mechanics:

| Queue Type      | Enum               | Purpose                        |
|-----------------|--------------------|--------------------------------|
| Admin Queue     | ICE_CTL_Q_ADMIN    | Primary FW command interface   |
| Mailbox Queue   | ICE_CTL_Q_MAILBOX  | PF-VF communication           |
| Sideband Queue  | ICE_CTL_Q_SB       | Sideband control interface     |

Each queue pair has:
- **Send Queue (SQ)**: Driver writes commands, FW reads and writes back completion
- **Receive Queue (RQ)**: FW writes async events, driver reads and processes

### 4.2 Ring Structure (ice_ctl_q_ring)

```
struct ice_ctl_q_ring:
  dma_head          -- VA to DMA head
  desc_buf          -- DMA memory for descriptor ring
  r.sq_bi/rq_bi    -- per-descriptor DMA buffers for indirect data
  count             -- number of descriptors in ring
  next_to_use       -- producer index (driver writes here)
  next_to_clean     -- consumer index (driver reads here)
  head/tail/len     -- MMIO register offsets for this ring
  bah/bal           -- base address register offsets
  len_mask          -- length field mask
  len_ena_mask      -- enable bit mask
  len_crit_mask     -- critical error bit mask
  head_mask         -- head register mask
```

### 4.3 Buffer Sizes

- Admin Queue: max 4096 bytes per descriptor buffer
- Mailbox Queue: max 4096 bytes
- Sideband Queue: max 512 bytes
- Large buffer threshold: 512 bytes (LIBIE_AQ_LG_BUF)

## 5. Command Submission and Completion Model

### 5.1 Command Submission Flow

```
ice_aq_send_cmd()                    -- public API, acquires global cfg lock
  |
  +-> ice_sq_send_cmd_retry()        -- retry wrapper for EBUSY
       |
       +-> ice_sq_send_cmd()         -- actual ring submission
            |
            1. Check !hw->reset_ongoing
            2. mutex_lock(&cq->sq_lock)
            3. Validate buffer params
            4. Set BUF/LB flags if indirect
            5. Read head register, check for overrun
            6. ice_clean_sq() -- reclaim completed descriptors
            7. Copy descriptor to ring slot
            8. If indirect: copy buffer to DMA, set addr_high/addr_low
            9. Bump next_to_use, write tail register
            10. ice_flush() -- ensure MMIO write lands
            11. Poll ice_sq_done() for completion (busy-wait)
            12. Copy writeback descriptor and data back to caller
            13. Extract retval, translate to errno
            14. mutex_unlock(&cq->sq_lock)
```

### 5.2 Completion Model

The E810 AQ uses **synchronous polling** for command completion:

- After writing the tail register, the driver busy-waits polling the head
  register via `ice_sq_done()` until the head advances past the submitted
  descriptor
- Timeout: 1 second (ICE_CTL_Q_SQ_CMD_TIMEOUT = USEC_PER_SEC)
- FW writes back the descriptor in-place with DD and CMP flags set
- The retval field contains the firmware error code
- For indirect commands, the DMA buffer is also written back with response data

### 5.3 Retry Logic

`ice_sq_send_cmd_retry()` retries commands that return EBUSY:

- Only certain opcodes are retryable (checked by `ice_should_retry_sq_send_cmd`)
- Saves a copy of the original descriptor and buffer before first attempt
- On EBUSY, restores the original and retries after ICE_SQ_SEND_DELAY_TIME_MS
- Max retries: ICE_SQ_SEND_MAX_EXECUTE

### 5.4 Global Configuration Lock

`ice_aq_send_cmd()` enforces serialization during package downloads:

- When the Global Configuration Lock is held, only a whitelist of opcodes
  may proceed without waiting (download_pkg, get_ver, upload_section, etc.)
- All other commands block on `ice_global_cfg_lock_sw` mutex
- This prevents command conflicts during DDP package and firmware updates

### 5.5 Async Event Processing (ARQ)

`__ice_clean_ctrlq()` in ice_main.c processes receive queue events:

```
__ice_clean_ctrlq(pf, q_type):
  1. Check error flags in RQ/SQ length registers
  2. Allocate temporary event buffer (rq_buf_size bytes)
  3. Loop: ice_clean_rq_elem() to dequeue events
  4. Dispatch by opcode:
     - get_link_status     -> ice_handle_link_event()
     - event_lan_overflow  -> ice_vf_lan_overflow_event()
     - send_msg_to_pf      -> ice_vc_process_vf_msg()
     - fw_logs_event        -> libie_get_fwlog_data()
     - lldp_set_mib_change -> ice_dcb_process_lldp_set_mib_change()
     - get_health_status   -> ice_process_health_status_event()
  5. Also: ice_aq_check_events() for waiters on specific opcodes
```

## 6. Comparison: ice AQ vs mlx5 Command Interface

### 6.1 Architecture Comparison

| Aspect                | ice AQ                           | mlx5 Command Interface           |
|-----------------------|----------------------------------|----------------------------------|
| **Transport**         | Paired ring buffers (SQ+RQ)      | Command mailbox slots (HCA)      |
| **Descriptor size**   | 32 bytes fixed                   | Variable (mailbox pages)         |
| **Command format**    | 32B desc + optional DMA buffer   | in/out buffers (mlx5_ifc format) |
| **Submission**        | Write to tail register           | Write doorbell to command slot   |
| **Completion**        | Synchronous polling (busy-wait)  | Workqueue + completion events    |
| **Async model**       | Separate receive queue (ARQ)     | Event Queue (EQ) + callbacks     |
| **Concurrency**       | Single outstanding (mutex)       | Multiple slots (semaphore-gated) |
| **Retry**             | Driver-level retry for EBUSY     | No automatic retry               |
| **Opcode encoding**   | 16-bit in descriptor             | 16-bit in input mailbox header   |
| **Error reporting**   | 16-bit retval in descriptor      | Status + syndrome in output      |
| **Statistics**        | None built-in                    | Per-opcode stats (xarray)        |
| **Throttling**        | Global mutex serialization       | Semaphore per category           |

### 6.2 mlx5 Command Interface Details

mlx5 uses a fundamentally different approach:

- **`mlx5_cmd_exec(dev, in, in_size, out, out_size)`**: The primary API takes
  raw input/output buffers defined by mlx5_ifc auto-generated structures
- **Slot-based**: The HCA has a fixed number of command slots (typically 32).
  Commands are dispatched to free slots via a workqueue
- **Asynchronous capable**: `mlx5_cmd_exec_cb()` supports callback-based
  async completion. Regular commands use `wait_for_completion()`
- **Token-based tracking**: Each command gets a unique 8-bit token for
  matching completions
- **Completion handler**: `mlx5_cmd_comp_handler()` processes EQ events
  signaling command completion
- **Privileged/unprivileged separation**: Semaphore-based separation for
  privileged vs regular commands
- **Per-opcode statistics**: Execution time tracking via xarray-indexed stats

### 6.3 Key Differences Relevant to fdice_

1. **ice is fundamentally synchronous** -- one command at a time per queue,
   busy-wait for completion. mlx5 supports true async with multiple in-flight.
2. **ice uses a shared descriptor format** (libie) that is hardware-fixed.
   mlx5 uses auto-generated structs from firmware specification (mlx5_ifc).
3. **ice separates async events to a receive queue**. mlx5 uses EQ (Event
   Queues) which also deliver CQ/EQ events for datapath.
4. **ice has no per-opcode statistics**. mlx5 tracks execution time per opcode.

## 7. fdice_ HAL Design Implications

### 7.1 The Hardware Boundary

The following MUST be preserved exactly as E810 firmware expects:
- `struct libie_aq_desc` format (32 bytes, exact field layout)
- All opcode values (they are firmware-defined protocol constants)
- Descriptor flag semantics (DD, CMP, ERR, BUF, LB, RD, etc.)
- Error code values (libie_aq_err)
- Per-opcode parameter struct layouts (ice_aqc_* structs)
- Ring buffer mechanics (head/tail register protocol)
- DMA buffer handling (addr_high/addr_low, datalen)

### 7.2 What CAN Be Redesigned Above the AQ

Everything above the ring submission layer is fair game:

1. **Command abstraction layer**: Instead of ice's `ice_fill_dflt_direct_cmd_desc()`
   pattern, use mlx5-inspired typed command helpers with structured input/output
2. **Completion model**: Replace busy-wait polling with workqueue-based
   completion (mlx5 pattern) for better CPU utilization
3. **Concurrency**: Support multiple outstanding commands using slot tracking
   (mlx5 pattern) instead of single-mutex serialization
4. **Async command support**: Add callback-based async API like
   `mlx5_cmd_exec_cb()` for non-blocking operations
5. **Per-opcode statistics**: Add execution time tracking per command type
6. **Error handling**: Richer error context with syndrome-like information
7. **Command categorization**: Group opcodes into subsystem objects
   (switch, phy, nvm, sched, etc.) instead of flat function namespace
8. **Retry policy**: Configurable per-opcode retry policy instead of
   hardcoded EBUSY retry
9. **Global lock management**: More granular locking (per-subsystem) instead
   of single global configuration lock

### 7.3 Proposed fdice_ AQ Architecture

```
                    +------------------------------------------+
                    |         Subsystem Command APIs            |
                    |  fdice_switch_*  fdice_phy_*  fdice_nvm_* |
                    +------------------+-----------------------+
                                       |
                    +------------------v-----------------------+
                    |        fdice_aq_cmd Layer                 |
                    |  - Typed command builders                 |
                    |  - Completion tracking (token-based)      |
                    |  - Per-opcode statistics                  |
                    |  - Configurable retry policy              |
                    |  - Async callback support                 |
                    +------------------+-----------------------+
                                       |
   ================== HARDWARE BOUNDARY =======================
                                       |
                    +------------------v-----------------------+
                    |        fdice_ctlq Layer                   |
                    |  - Ring buffer management                 |
                    |  - DMA descriptor handling                |
                    |  - Head/tail register protocol            |
                    |  - libie_aq_desc format (FIXED)           |
                    |  - Opcode values (FIXED)                  |
                    +------------------------------------------+
```

### 7.4 Priority Implementation Order for AQ

1. **fdice_ctlq**: Ring init/teardown, descriptor submission, completion polling
   (must match E810 exactly)
2. **fdice_aq_cmd**: Command dispatch with completion tracking and statistics
3. **Core subsystem commands**: get_ver, list_caps, req_res/release_res
4. **VSI/Switch commands**: add_vsi, sw_rules, port_params
5. **PHY/Link commands**: get_phy_caps, set_phy_cfg, get_link_status
6. **Scheduler commands**: add_sched_elems, cfg_sched_elems, rl_profiles
7. **NVM commands**: nvm_read, nvm_write, package management
8. **LLDP/DCB commands**: lldp_get_mib, lldp_stop/start
9. **RSS commands**: set/get_rss_key, set/get_rss_lut
10. **Tx queue commands**: add_txqs, dis_txqs, add_rdma_qset
11. **SyncE/PTP commands**: CGU configuration
12. **Diagnostics**: health status, FW logging

## 8. Command Parameter Structure Summary

Each AQ opcode has an associated 16-byte parameter structure that fits in the
descriptor's `params` union. For indirect commands, the last 8 bytes are
typically `addr_high`/`addr_low` pointing to the DMA buffer. Key parameter
structs include:

| Category    | Key Structs                                         |
|-------------|-----------------------------------------------------|
| Core        | libie_aqc_get_ver, libie_aqc_driver_ver, libie_aqc_req_res |
| Capabilities| libie_aqc_list_caps, libie_aqc_list_caps_elem (32B response) |
| Switch      | ice_aqc_get_sw_cfg, ice_aqc_set_port_params         |
| Resources   | ice_aqc_alloc_free_res_cmd, ice_aqc_alloc_free_res_elem |
| VSI         | ice_aqc_add_get_update_free_vsi, ice_aqc_vsi_props  |
| Recipes     | ice_aqc_add_get_recipe, ice_aqc_recipe_data_elem    |
| SW Rules    | ice_aqc_sw_rules, ice_sw_rule_lkup_rx_tx            |
| Scheduler   | ice_aqc_sched_elem_cmd, ice_aqc_txsched_elem_data   |
| PHY         | ice_aqc_get_phy_caps, ice_aqc_get_phy_caps_data     |
| Link        | ice_aqc_get_link_status, ice_aqc_get_link_status_data |
| NVM         | ice_aqc_nvm, ice_aqc_nvm_pass_comp_tbl              |
| LLDP        | ice_aqc_lldp_get_mib, ice_aqc_lldp_set_local_mib   |
| RSS         | ice_aqc_get_set_rss_key, ice_aqc_get_set_rss_lut    |
| Tx Queues   | ice_aqc_add_txqs, ice_aqc_add_txqs_perq             |
| FW Logging  | libie_aqc_fw_log, libie_aqc_fw_log_cfg_resp         |

All parameter structs are exactly 16 bytes (enforced by LIBIE_CHECK_STRUCT_LEN
and static_assert). Response buffer structs vary in size up to 4096 bytes.

## 9. Capability Discovery

The list_func_caps (0x000A) and list_dev_caps (0x000B) commands return a list
of `libie_aqc_list_caps_elem` (32 bytes each) describing hardware capabilities.
Over 50 capability IDs are defined, including:

- **Switching**: SWITCH_MODE, NPAR_ACTIVE, VALID_FUNCTIONS
- **Virtualization**: SRIOV, VF, VMDQ, VSI
- **Network features**: DCB, RSS, FD (flow director), 1588
- **Queue limits**: RXQS, TXQS, MSIX, VF_MSIX
- **Physical**: MAX_MTU, LED, SDP, MDIO
- **Storage**: FCOE, ISCSI
- **NVM/FW versions**: NVM_VER, OROM_VER, NET_VER (and pending versions)
- **Advanced**: RDMA, INLINE_IPSEC, LAG_SUPPORT
- **Topology**: EXT_TOPO_DEV_IMG0-3, NAC_TOPOLOGY, TX_SCHED_TOPO_COMP_MODE

This capability list is critical for fdice_ initialization -- it determines
which features the hardware supports and how many resources are available.
