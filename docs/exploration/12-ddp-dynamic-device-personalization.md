<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration Q12: DDP (Dynamic Device Personalization)

## Overview

Dynamic Device Personalization (DDP) is E810's mechanism for loading pipeline
configuration packages into the hardware at runtime. These packages define how
the E810 parser pipeline identifies protocols, extracts fields, and classifies
packets. Without a valid DDP package, the device enters "Safe Mode" with only
basic L2 functionality.

DDP is unique to the E810 platform and must be reimplemented correctly --
firmware compatibility depends on getting the package format, signing
verification, and AQ download sequence exactly right.

## Source Files Explored

- `ice_ddp.h` -- Package format structures, segment types, section IDs, block/section enums
- `ice_ddp.c` -- Package verification, download, initialization (~2500 lines)
- `ice_main.c` -- `ice_init_ddp_config()`, `ice_load_pkg()`, firmware request
- `ice_flex_pipe.c` -- `ice_fill_blk_tbls()` populating hardware pipeline tables
- `ice_flex_type.h` -- Extraction sequence and field vector types
- `ice_adminq_cmd.h` -- `ice_aqc_download_pkg`, `ice_pkg_ver` AQ structures
- `ice_parser.c` -- `ice_pkg_sect_hdr` for parser table sections

## DDP Package Format

### Package Header (`ice_pkg_hdr`)

The top-level container for a DDP file:

```
struct ice_pkg_hdr {
    struct ice_pkg_ver pkg_format_ver;  // Must be 1.0.0.0
    __le32 seg_count;                   // Number of segments
    __le32 seg_offset[];                // Variable-length offset array
};
```

Format version is strictly checked: major=1, minor=0, update=0, draft=0.
The minimum supported package version is 1.3.

### Package Version (`ice_pkg_ver`)

Used throughout for version tracking (format version, segment version, package
content version):

```
struct ice_pkg_ver {
    u8 major;
    u8 minor;
    u8 update;
    u8 draft;
};
```

## Segment Types

A DDP package contains multiple segments, each identified by type:

| Segment Type Constant          | Value        | Purpose                              |
|-------------------------------|-------------|--------------------------------------|
| `SEGMENT_TYPE_METADATA`       | 0x00000001  | Global package metadata (name, ver)  |
| `SEGMENT_TYPE_ICE_E810`       | 0x00000010  | E810-specific pipeline configuration |
| `SEGMENT_TYPE_ICE_RUN_TIME_CFG` | 0x00000020 | Runtime configuration (Tx topology)  |
| `SEGMENT_TYPE_SIGNING`        | 0x00001001  | Cryptographic signature segment      |
| `SEGMENT_TYPE_ICE_E830`       | 0x00000017  | E830-specific segment                |

### Generic Segment Header (`ice_generic_seg_hdr`)

Every segment starts with this header:

```
struct ice_generic_seg_hdr {
    __le32 seg_type;
    struct ice_pkg_ver seg_format_ver;
    __le32 seg_size;
    char seg_id[ICE_PKG_NAME_SIZE];
};
```

### Ice-Specific Segment (`ice_seg`)

The E810 pipeline segment extends the generic header with a device table:

```
struct ice_seg {
    struct ice_generic_seg_hdr hdr;
    __le32 device_table_count;
    struct ice_device_id_entry device_table[];  // PCI device ID matching
};
```

This enables the driver to find the correct segment for the current hardware.

### Signing Segment (`ice_sign_seg`)

Cryptographic signature segments for secure boot:

```
struct ice_sign_seg {
    struct ice_generic_seg_hdr hdr;
    __le32 seg_id;
    __le32 sign_type;           // RSA2K, RSA3K, RSA3K_SBB, RSA3K_E825
    __le32 signed_seg_idx;      // Index of the segment being signed
    __le32 signed_buf_start;
    __le32 signed_buf_count;
    __le32 flags;               // VALID and LAST flags
    u8 reserved[40];
    struct ice_buf_table buf_tbl;
};
```

Signing algorithm types:
- `SEGMENT_SIGN_TYPE_RSA2K` (0x01)
- `SEGMENT_SIGN_TYPE_RSA3K` (0x02)
- `SEGMENT_SIGN_TYPE_RSA3K_SBB` (0x03) -- Secure Boot Block
- `SEGMENT_SIGN_TYPE_RSA3K_E825` (0x05)

### Global Metadata Segment (`ice_global_metadata_seg`)

Contains the human-readable package name and content version:

```
struct ice_global_metadata_seg {
    struct ice_generic_seg_hdr hdr;
    struct ice_pkg_ver pkg_ver;
    __le32 rsvd;
    char pkg_name[ICE_PKG_NAME_SIZE];
};
```

## Buffer and Section Architecture

### 4K Buffer Structure

All package data is organized into 4096-byte buffers (`ICE_PKG_BUF_SIZE`):

```
struct ice_buf {
    u8 buf[ICE_PKG_BUF_SIZE];
};

struct ice_buf_table {
    __le32 buf_count;
    struct ice_buf buf_array[];
};
```

Each buffer contains a header describing its sections:

```
struct ice_buf_hdr {
    __le16 section_count;
    __le16 data_end;
    struct ice_section_entry section_entry[];
};
```

### Section Types (Pipeline Stages)

Sections are identified by numeric IDs organized by pipeline block:

**Pipeline Blocks** (`ice_block`):
- `ICE_BLK_SW` (0) -- Switch block
- `ICE_BLK_ACL` (1) -- ACL block
- `ICE_BLK_FD` (2) -- Flow Director block
- `ICE_BLK_RSS` (3) -- RSS block
- `ICE_BLK_PE` (4) -- Protocol Engine block

**Per-Block Section Types** (`ice_sect`):
Each block has 9 section types (XLT0, XLT_KB, XLT1, XLT2, PROF_TCAM,
PROF_REDIR, VEC_TBL, CDID_KB, CDID_REDIR), with IDs spaced 10 apart:

| Block | Base SID | Example: XLT0 | XLT1 | PROF_TCAM | FLD_VEC |
|-------|----------|---------------|------|-----------|---------|
| SW    | 10       | 10            | 12   | 14        | 16      |
| ACL   | 20       | 20            | 22   | 24        | 26      |
| FD    | 30       | 30            | 32   | 34        | 36      |
| RSS   | 40       | 40            | 42   | 44        | 46      |
| PE    | 80       | 80            | 82   | 84        | 86      |

**Parser Sections** (RX/TX parser tables):
- `ICE_SID_RXPARSER_CAM` (50) -- Parser CAM
- `ICE_SID_RXPARSER_NOMATCH_CAM` (51) -- No-match CAM
- `ICE_SID_RXPARSER_IMEM` (52) -- Instruction memory
- `ICE_SID_RXPARSER_MARKER_PTYPE` (55) -- Marker to PTYPE mapping
- `ICE_SID_RXPARSER_BOOST_TCAM` (56) -- Boost TCAM entries
- `ICE_SID_RXPARSER_PROTO_GRP` (57) -- Protocol groups
- `ICE_SID_RXPARSER_METADATA_INIT` (58) -- Metadata initialization
- `ICE_SID_TXPARSER_BOOST_TCAM` (66) -- TX Boost TCAM

**Field Vectors** (`ice_fv`):
The core extraction mechanism -- 48 field vector words per profile, each
specifying a protocol ID and offset:

```
struct ice_fv_word {
    u8 prot_id;
    u16 off;        // Offset within protocol header
    u8 resvrd;
};
```

## DDP Loading Sequence

### Top-Level Call Chain

```
ice_probe()
  -> ice_init_dev_hw()
    -> ice_init_ddp_config()
      -> ice_request_fw()           // Request firmware file
      -> ice_init_tx_topology()     // Configure TX topology from pkg
      -> ice_load_pkg()             // Main package load entry
        -> ice_copy_and_init_pkg()  // Copy buffer, call ice_init_pkg
        -> ice_init_pkg()           // OR reload from hw->pkg_copy
      -> ice_init_supported_rxdids()
```

### `ice_load_pkg()` -- Driver Entry Point

Called from `ice_init_ddp_config()` with the firmware blob. Two paths:

1. **First load**: Calls `ice_copy_and_init_pkg()` which copies the firmware
   buffer (so the original can be released) and then calls `ice_init_pkg()`.
2. **Rebuild after reset**: Calls `ice_init_pkg()` directly with `hw->pkg_copy`.

On failure, the driver enters Safe Mode (clears `ICE_FLAG_ADV_FEATURES`).

### `ice_init_pkg()` -- Core Initialization

The central function (complexity: 14, 69 LOC). Sequence:

1. **Verify package** (`ice_verify_pkg`) -- Validates format version, segment
   count, and that all segments fit within the buffer length.

2. **Initialize package info** (`ice_init_pkg_info`) -- Extracts metadata
   segment to populate `hw->pkg_ver` and `hw->pkg_name`. Determines if the
   package has signing segments and records `hw->pkg_seg_id` and
   `hw->pkg_sign_type`.

3. **Match signing segment** (`ice_match_signing_seg`) -- If the package has
   signing segments, verifies that a matching signature segment exists for the
   expected segment ID and signing type.

4. **Check compatibility** (`ice_chk_pkg_compat`) -- Compares the package
   version against driver-supported version range and verifies the device
   table against the current PCI device ID. Returns the matching `ice_seg`
   pointer.

5. **Initialize hints** (`ice_init_pkg_hints`) -- Scans label sections in the
   package for protocol-related hints used during runtime classification.

6. **Download package** (`ice_download_pkg`) -- Sends the package to firmware
   via Admin Queue commands. Two paths based on signing:
   - `ice_download_pkg_with_sig_seg()` -- For signed packages
   - `ice_download_pkg_without_sig_seg()` -- For unsigned packages

7. **Get package info** (`ice_get_pkg_info`) -- After download, queries firmware
   for the active package information via AQ.

8. **Determine final state** (`ice_get_ddp_pkg_state`) -- Maps the loaded
   package state to a `ice_ddp_state` enum value.

9. **Post-load setup** (on success):
   - `ice_init_pkg_regs()` -- Configure hardware registers for the package
   - `ice_fill_blk_tbls()` -- Populate pipeline block tables (XLT, TCAM, etc.)
   - `ice_fill_hw_ptype()` -- Fill hardware packet type mapping
   - `ice_get_prof_index_max()` -- Determine maximum profile index

### Download to Firmware

#### Signed Package Path (`ice_download_pkg_with_sig_seg`)

1. Acquire global config lock (`ice_acquire_global_cfg_lock`)
2. Iterate over all segments, find signing segments matching `hw->pkg_seg_id`
   and `hw->pkg_sign_type`
3. For each matching signing segment:
   - `ice_dwnld_sign_and_cfg_segs()` -- Downloads the signing segment buffers
     followed by the configuration buffers of the signed segment
4. `ice_post_dwnld_pkg_actions()` -- Post-download finalization
5. Release global config lock

#### The DDP Send Mechanism

Buffers are sent to firmware via the `ice_ddp_send_hunk` function which
uses the `ice_aq_download_pkg` Admin Queue command. The download is chunked
into hunks that fit the AQ buffer size. Each hunk includes:
- Whether it is the last hunk
- The segment offset tracking via `ice_ddp_send_ctx`

### Error Handling

The `ice_ddp_state` enum provides comprehensive error classification:

| State | Value | Meaning |
|-------|-------|---------|
| `ICE_DDP_PKG_SUCCESS` | 0 | Successful load |
| `ICE_DDP_PKG_ALREADY_LOADED` | -1 | Generic already-loaded |
| `ICE_DDP_PKG_SAME_VERSION_ALREADY_LOADED` | -2 | Same version by another PF |
| `ICE_DDP_PKG_ALREADY_LOADED_NOT_SUPPORTED` | -3 | Loaded but unsupported |
| `ICE_DDP_PKG_COMPATIBLE_ALREADY_LOADED` | -4 | Compatible but different |
| `ICE_DDP_PKG_FW_MISMATCH` | -5 | Firmware incompatible |
| `ICE_DDP_PKG_INVALID_FILE` | -6 | Invalid package file |
| `ICE_DDP_PKG_FILE_VERSION_TOO_HIGH` | -7 | Version too new for driver |
| `ICE_DDP_PKG_FILE_VERSION_TOO_LOW` | -8 | Version too old for driver |
| `ICE_DDP_PKG_FILE_SIGNATURE_INVALID` | -9 | Bad signature |
| `ICE_DDP_PKG_FILE_REVISION_TOO_LOW` | -10 | Security revision too low |
| `ICE_DDP_PKG_LOAD_ERROR` | -11 | Firmware load error |
| `ICE_DDP_PKG_ERR` | -12 | Other errors |

AQ errors are mapped to DDP states via `ice_map_aq_err_to_ddp_state()`.

The `ALREADY_LOADED` state is not a fatal error -- the driver continues with
the previously loaded package but still verifies compatibility.

### VLAN Mode Post-Processing

After package download, `ice_post_pkg_dwnld_vlan_mode_cfg()` is called to
configure VLAN mode based on the DDP package capabilities.

## Package Enumeration API

The `ice_pkg_enum` structure provides an iterator for walking sections within
a package:

```
struct ice_pkg_enum {
    struct ice_buf_table *buf_table;
    u32 buf_idx;
    u32 type;
    const struct ice_buf_hdr *buf;
    u32 sect_idx;
    void *sect;
    u32 sect_type;
    u32 entry_idx;
    void *(*handler)(...);
};
```

Key enumeration functions:
- `ice_pkg_enum_section()` -- Iterate over sections of a given type
- `ice_pkg_enum_entry()` -- Iterate over entries within sections

## TX Topology Configuration

Separate from the main DDP load, `ice_cfg_tx_topo()` handles TX topology
configuration from a runtime configuration segment (`SEGMENT_TYPE_ICE_RUN_TIME_CFG`).
This is loaded via `ice_init_tx_topology()` before the main DDP package download.

## Devlink Integration

DDP package information is exposed via devlink info:
- `ice_info_ddp_pkg_name()` -- Reports package name
- `ice_info_ddp_pkg_version()` -- Reports package version

## fdice_ Implementation Requirements

### Critical Design Decisions

1. **Package format is a hardware interface**: The binary format of DDP packages
   is defined by Intel firmware. Our implementation must parse this format
   exactly -- no creative reinterpretation.

2. **Signing verification is security-critical**: The signing segment handling
   must be correct to prevent loading unauthorized packages.

3. **Multi-PF awareness**: Multiple PFs share the same physical device. The
   already-loaded detection and global config lock are essential for correct
   multi-PF operation.

4. **Safe Mode fallback**: The driver must gracefully degrade to Safe Mode
   when DDP loading fails, not crash.

### Implementation Plan

```
fdice_ddp.h / fdice_ddp.c:
  - fdice_ddp_state enum (matching all ice error states)
  - fdice_verify_pkg() -- Package format validation
  - fdice_init_pkg() -- Core initialization orchestrator
  - fdice_download_pkg() -- AQ download dispatcher
  - fdice_download_pkg_signed() -- Signed package path
  - fdice_download_pkg_unsigned() -- Unsigned package path
  - fdice_pkg_enum_*() -- Section/entry iterators
  - fdice_init_pkg_info() -- Metadata extraction
  - fdice_chk_pkg_compat() -- Compatibility checking

fdice_main.c:
  - fdice_init_ddp_config() -- Top-level DDP config
  - fdice_load_pkg() -- Firmware request and load
  - fdice_log_pkg_init() -- Status logging

fdice_flex_pipe.c:
  - fdice_fill_blk_tbls() -- Pipeline table population
  - fdice_fill_hw_ptype() -- PTYPE mapping
```

### Improvements Over ice

1. **Cleaner error propagation**: Use a unified result type rather than mixing
   enum states with errno values.

2. **Better separation of concerns**: Split the monolithic `ice_init_pkg()` into
   smaller, testable stages with clear interfaces.

3. **Structured logging**: Replace ad-hoc debug prints with structured tracing
   for easier debugging of DDP issues in the field.

4. **Validation hardening**: Add additional bounds checks on section sizes and
   entry counts to prevent firmware-triggered buffer overflows.

5. **Package caching improvements**: Better lifetime management of `pkg_copy`
   to avoid unnecessary re-downloads after minor resets.
