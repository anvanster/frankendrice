// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_DDP_H_
#define _FDICE_DDP_H_

/**
 * DOC: FrankenDrICE DDP (Dynamic Device Personalization)
 *
 * DDP packages configure the E810 parser pipeline at runtime, defining
 * how the device identifies protocols, extracts fields, and classifies
 * packets. Without a valid DDP package, the device enters Safe Mode
 * with only basic L2 functionality.
 *
 * The binary package format is a hardware interface defined by E810
 * firmware -- our structs must match the on-disk layout exactly.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.6)
 * Exploration reference: docs/exploration/12-ddp-dynamic-device-personalization.md
 */

#include <linux/types.h>
#include <linux/firmware.h>

/* Forward declarations */
struct fdice_hw;
struct fdice_dev;

/* -----------------------------------------------------------------------
 * DDP State -- Result codes for package loading operations
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_ddp_state - DDP package loading result codes
 * @FDICE_DDP_PKG_SUCCESS: Package loaded successfully
 * @FDICE_DDP_PKG_ALREADY_LOADED: Generic already-loaded (mapped to specific)
 * @FDICE_DDP_PKG_SAME_VER_LOADED: Same version loaded by another PF
 * @FDICE_DDP_PKG_LOADED_NOT_SUPPORTED: Loaded package not supported by driver
 * @FDICE_DDP_PKG_COMPAT_LOADED: Compatible but different package loaded
 * @FDICE_DDP_PKG_FW_MISMATCH: Firmware version incompatible with package
 * @FDICE_DDP_PKG_INVALID_FILE: Package file is malformed or corrupt
 * @FDICE_DDP_PKG_VER_TOO_HIGH: Package version newer than driver supports
 * @FDICE_DDP_PKG_VER_TOO_LOW: Package version older than driver supports
 * @FDICE_DDP_PKG_SIGNATURE_INVALID: Cryptographic signature verification failed
 * @FDICE_DDP_PKG_REVISION_TOO_LOW: Security revision below firmware minimum
 * @FDICE_DDP_PKG_LOAD_ERROR: Firmware encountered error during download
 * @FDICE_DDP_PKG_ERR: Unclassified error
 *
 * These states map to firmware AQ return codes and enable structured
 * error reporting for DDP loading failures. ALREADY_LOADED variants
 * are non-fatal -- the driver continues with the existing package.
 */
enum fdice_ddp_state {
	FDICE_DDP_PKG_SUCCESS			= 0,
	FDICE_DDP_PKG_ALREADY_LOADED		= -1,
	FDICE_DDP_PKG_SAME_VER_LOADED		= -2,
	FDICE_DDP_PKG_LOADED_NOT_SUPPORTED	= -3,
	FDICE_DDP_PKG_COMPAT_LOADED		= -4,
	FDICE_DDP_PKG_FW_MISMATCH		= -5,
	FDICE_DDP_PKG_INVALID_FILE		= -6,
	FDICE_DDP_PKG_VER_TOO_HIGH		= -7,
	FDICE_DDP_PKG_VER_TOO_LOW		= -8,
	FDICE_DDP_PKG_SIGNATURE_INVALID		= -9,
	FDICE_DDP_PKG_REVISION_TOO_LOW		= -10,
	FDICE_DDP_PKG_LOAD_ERROR		= -11,
	FDICE_DDP_PKG_ERR			= -12,
};

/* -----------------------------------------------------------------------
 * Package Format Constants -- hardware interface
 * ----------------------------------------------------------------------- */

/* Package format version (strictly checked: must be 1.0.0.0) */
#define FDICE_PKG_FMT_VER_MAJ		1
#define FDICE_PKG_FMT_VER_MNR		0
#define FDICE_PKG_FMT_VER_UPD		0
#define FDICE_PKG_FMT_VER_DFT		0

/* Minimum supported package content version */
#define FDICE_PKG_SUPP_VER_MAJ		1
#define FDICE_PKG_SUPP_VER_MNR		3

/* Package name and segment ID size constants */
#define FDICE_PKG_NAME_SIZE		32
#define FDICE_SEG_ID_SIZE		28
#define FDICE_SEG_NAME_SIZE		28

/* DDP buffer size -- all package data organized in 4K buffers */
#define FDICE_PKG_BUF_SIZE		4096

/* Metadata buffer flag in section type */
#define FDICE_METADATA_BUF		0x80000000

/* Section bounds validation */
#define FDICE_MIN_S_OFF			12
#define FDICE_MAX_S_OFF			4095
#define FDICE_MIN_S_SZ			1
#define FDICE_MAX_S_SZ			4084
#define FDICE_MIN_S_COUNT		1
#define FDICE_MAX_S_COUNT		511
#define FDICE_MIN_S_DATA_END		12
#define FDICE_MAX_S_DATA_END		4096

/* Maximum field vector words per profile */
#define FDICE_MAX_FV_WORDS		48

/* Maximum number of profiles */
#define FDICE_MAX_NUM_PROFILES		256

/* Firmware file name for E810 DDP package */
#define FDICE_DDP_PKG_FILE		"intel/ice/ddp/ice.pkg"

/* -----------------------------------------------------------------------
 * Segment Type Constants -- firmware-defined
 * ----------------------------------------------------------------------- */

#define FDICE_SEG_TYPE_INVALID		0x00000000
#define FDICE_SEG_TYPE_METADATA		0x00000001
#define FDICE_SEG_TYPE_ICE_E810		0x00000010
#define FDICE_SEG_TYPE_RUN_TIME_CFG	0x00000020
#define FDICE_SEG_TYPE_ICE_E830		0x00000017
#define FDICE_SEG_TYPE_SIGNING		0x00001001

/* -----------------------------------------------------------------------
 * Signing Type Constants -- firmware-defined
 * ----------------------------------------------------------------------- */

#define FDICE_SIGN_TYPE_INVALID		0x00000000
#define FDICE_SIGN_TYPE_RSA2K		0x00000001
#define FDICE_SIGN_TYPE_RSA3K		0x00000002
#define FDICE_SIGN_TYPE_RSA3K_SBB	0x00000003
#define FDICE_SIGN_TYPE_RSA3K_E825	0x00000005

/* Signing segment flags */
#define FDICE_SIGN_SEG_FLAGS_VALID	0x80000000
#define FDICE_SIGN_SEG_FLAGS_LAST	0x00000001
#define FDICE_SIGN_SEG_RESERVED_COUNT	40

/* -----------------------------------------------------------------------
 * Package Format Structures -- hardware interface, layouts must match
 * firmware exactly. All multi-byte fields are little-endian.
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_pkg_ver - Package version quadruplet
 * @major: major version number
 * @minor: minor version number
 * @update: update revision number
 * @draft: draft indicator (0 for release builds)
 *
 * Used for format version, segment version, and content version
 * fields throughout the DDP package format.
 */
struct fdice_pkg_ver {
	u8	major;
	u8	minor;
	u8	update;
	u8	draft;
};

/**
 * struct fdice_pkg_hdr - Top-level DDP package header
 * @pkg_format_ver: package format version (must be 1.0.0.0)
 * @seg_count: number of segments in the package
 * @seg_offset: variable-length array of byte offsets to each segment
 *
 * The entry point for parsing any DDP file. Format version is strictly
 * validated before accessing any segment data.
 */
struct fdice_pkg_hdr {
	struct fdice_pkg_ver	pkg_format_ver;
	__le32			seg_count;
	__le32			seg_offset[];
};

/**
 * struct fdice_seg_hdr - Generic segment header
 * @seg_type: segment type identifier (FDICE_SEG_TYPE_*)
 * @seg_format_ver: segment format version
 * @seg_size: total size of this segment in bytes
 * @seg_id: human-readable segment identifier string
 *
 * Every segment in a DDP package begins with this header.
 * The seg_type field determines how the remainder is interpreted.
 */
struct fdice_seg_hdr {
	__le32			seg_type;
	struct fdice_pkg_ver	seg_format_ver;
	__le32			seg_size;
	char			seg_id[FDICE_SEG_ID_SIZE];
};

/**
 * union fdice_device_id - PCI device ID for segment matching
 * @dev_vend_id: structured access to device and vendor IDs
 * @dev_vend_id.device_id: PCI device ID
 * @dev_vend_id.vendor_id: PCI vendor ID
 * @id: raw 32-bit access for comparison
 */
union fdice_device_id {
	struct {
		__le16	device_id;
		__le16	vendor_id;
	} dev_vend_id;
	__le32	id;
};

/**
 * struct fdice_device_id_entry - Device table entry for segment matching
 * @device: primary PCI device/vendor ID pair
 * @sub_device: subsystem PCI device/vendor ID pair
 *
 * Used within the ice-specific segment to match against the current
 * hardware's PCI identity during compatibility checking.
 */
struct fdice_device_id_entry {
	union fdice_device_id	device;
	union fdice_device_id	sub_device;
};

/**
 * struct fdice_ice_seg - E810-specific pipeline segment
 * @hdr: generic segment header
 * @device_table_count: number of entries in the device table
 * @device_table: variable-length device ID matching table
 *
 * Extends the generic segment header with a device compatibility
 * table. The driver scans this table to find the segment that
 * matches the current hardware.
 */
struct fdice_ice_seg {
	struct fdice_seg_hdr		hdr;
	__le32				device_table_count;
	struct fdice_device_id_entry	device_table[];
};

/**
 * struct fdice_metadata_seg - Global metadata segment
 * @hdr: generic segment header (type = FDICE_SEG_TYPE_METADATA)
 * @pkg_ver: package content version
 * @rsvd: reserved field
 * @pkg_name: human-readable package name string
 *
 * Contains the package identity information exposed via devlink info.
 */
struct fdice_metadata_seg {
	struct fdice_seg_hdr	hdr;
	struct fdice_pkg_ver	pkg_ver;
	__le32			rsvd;
	char			pkg_name[FDICE_PKG_NAME_SIZE];
};

/**
 * struct fdice_sign_seg - Cryptographic signing segment
 * @hdr: generic segment header (type = FDICE_SEG_TYPE_SIGNING)
 * @seg_id: identifier of the signed segment
 * @sign_type: signing algorithm (FDICE_SIGN_TYPE_*)
 * @signed_seg_idx: index of the segment being signed
 * @signed_buf_start: first buffer index in the signed range
 * @signed_buf_count: number of buffers in the signed range
 * @flags: validity and ordering flags
 * @reserved: reserved padding
 * @buf_tbl: signature data buffer table
 *
 * Provides RSA signature verification for secure package loading.
 * The driver must find a signing segment matching the expected
 * sign_type for the device before downloading.
 */
struct fdice_sign_seg {
	struct fdice_seg_hdr	hdr;
	__le32			seg_id;
	__le32			sign_type;
	__le32			signed_seg_idx;
	__le32			signed_buf_start;
	__le32			signed_buf_count;
	__le32			flags;
	u8			reserved[FDICE_SIGN_SEG_RESERVED_COUNT];
	/* Followed by buffer table -- variable length */
};

/**
 * struct fdice_buf - Single 4K package buffer
 * @buf: raw buffer data
 *
 * All package content is organized into fixed-size 4K buffers
 * for alignment with the AQ DMA transfer mechanism.
 */
struct fdice_buf {
	u8	buf[FDICE_PKG_BUF_SIZE];
};

/**
 * struct fdice_buf_table - Table of package buffers
 * @buf_count: number of buffers in the array
 * @buf_array: variable-length array of 4K buffers
 */
struct fdice_buf_table {
	__le32		buf_count;
	struct fdice_buf buf_array[];
};

/**
 * struct fdice_section_entry - Section descriptor within a buffer
 * @type: section type identifier (pipeline block + section kind)
 * @offset: byte offset within the buffer to section data
 * @size: size of section data in bytes
 */
struct fdice_section_entry {
	__le32	type;
	__le16	offset;
	__le16	size;
};

/**
 * struct fdice_buf_hdr - Header at the start of each 4K buffer
 * @section_count: number of sections in this buffer
 * @data_end: offset to end of data within the buffer
 * @section_entry: variable-length section descriptor array
 *
 * Each 4K buffer within a segment contains this header followed
 * by the section data. The section_entry array describes where
 * each section's data begins and its size.
 */
struct fdice_buf_hdr {
	__le16			section_count;
	__le16			data_end;
	struct fdice_section_entry section_entry[];
};

/**
 * struct fdice_meta_sect - Metadata section content
 * @ver: package content version from metadata
 * @name: human-readable package name
 * @track_id: package tracking identifier
 *
 * Found in buffers of the metadata segment. Provides the
 * canonical package identity for logging and devlink reporting.
 */
struct fdice_meta_sect {
	struct fdice_pkg_ver	ver;
#define FDICE_META_SECT_NAME_SIZE 28
	char			name[FDICE_META_SECT_NAME_SIZE];
	__le32			track_id;
};

/* -----------------------------------------------------------------------
 * Pipeline Block and Section Identifiers -- hardware-defined
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_block - Pipeline block identifiers
 * @FDICE_BLK_SW: Switch block
 * @FDICE_BLK_ACL: ACL block
 * @FDICE_BLK_FD: Flow Director block
 * @FDICE_BLK_RSS: RSS block
 * @FDICE_BLK_PE: Protocol Engine block
 * @FDICE_BLK_COUNT: Number of pipeline blocks
 */
enum fdice_block {
	FDICE_BLK_SW	= 0,
	FDICE_BLK_ACL,
	FDICE_BLK_FD,
	FDICE_BLK_RSS,
	FDICE_BLK_PE,
	FDICE_BLK_COUNT,
};

/**
 * enum fdice_sect - Per-block section type indices
 * @FDICE_XLT0: XLT0 translation table
 * @FDICE_XLT_KB: XLT key builder
 * @FDICE_XLT1: XLT1 translation table
 * @FDICE_XLT2: XLT2 translation table
 * @FDICE_PROF_TCAM: Profile TCAM entries
 * @FDICE_PROF_REDIR: Profile redirection table
 * @FDICE_VEC_TBL: Field vector table
 * @FDICE_CDID_KB: CDID key builder
 * @FDICE_CDID_REDIR: CDID redirection table
 * @FDICE_SECT_COUNT: Number of section types per block
 */
enum fdice_sect {
	FDICE_XLT0 = 0,
	FDICE_XLT_KB,
	FDICE_XLT1,
	FDICE_XLT2,
	FDICE_PROF_TCAM,
	FDICE_PROF_REDIR,
	FDICE_VEC_TBL,
	FDICE_CDID_KB,
	FDICE_CDID_REDIR,
	FDICE_SECT_COUNT,
};

/* Section ID constants (SID) -- base IDs per pipeline block */
#define FDICE_SID_METADATA		1

/* Switch block (base 10) */
#define FDICE_SID_XLT0_SW		10
#define FDICE_SID_XLT_KB_SW		11
#define FDICE_SID_XLT1_SW		12
#define FDICE_SID_XLT2_SW		13
#define FDICE_SID_PROFID_TCAM_SW	14
#define FDICE_SID_PROFID_REDIR_SW	15
#define FDICE_SID_FLD_VEC_SW		16
#define FDICE_SID_CDID_KB_SW		17
#define FDICE_SID_CDID_REDIR_SW	18

/* ACL block (base 20) */
#define FDICE_SID_XLT0_ACL		20
#define FDICE_SID_XLT_KB_ACL		21
#define FDICE_SID_XLT1_ACL		22
#define FDICE_SID_XLT2_ACL		23
#define FDICE_SID_PROFID_TCAM_ACL	24
#define FDICE_SID_PROFID_REDIR_ACL	25
#define FDICE_SID_FLD_VEC_ACL		26
#define FDICE_SID_CDID_KB_ACL		27
#define FDICE_SID_CDID_REDIR_ACL	28

/* Flow Director block (base 30) */
#define FDICE_SID_XLT0_FD		30
#define FDICE_SID_XLT_KB_FD		31
#define FDICE_SID_XLT1_FD		32
#define FDICE_SID_XLT2_FD		33
#define FDICE_SID_PROFID_TCAM_FD	34
#define FDICE_SID_PROFID_REDIR_FD	35
#define FDICE_SID_FLD_VEC_FD		36
#define FDICE_SID_CDID_KB_FD		37
#define FDICE_SID_CDID_REDIR_FD	38

/* RSS block (base 40) */
#define FDICE_SID_XLT0_RSS		40
#define FDICE_SID_XLT_KB_RSS		41
#define FDICE_SID_XLT1_RSS		42
#define FDICE_SID_XLT2_RSS		43
#define FDICE_SID_PROFID_TCAM_RSS	44
#define FDICE_SID_PROFID_REDIR_RSS	45
#define FDICE_SID_FLD_VEC_RSS		46
#define FDICE_SID_CDID_KB_RSS		47
#define FDICE_SID_CDID_REDIR_RSS	48

/* Parser sections */
#define FDICE_SID_RXPARSER_CAM		50
#define FDICE_SID_RXPARSER_NOMATCH_CAM	51
#define FDICE_SID_RXPARSER_IMEM		52
#define FDICE_SID_RXPARSER_MARKER_PTYPE	55
#define FDICE_SID_RXPARSER_BOOST_TCAM	56
#define FDICE_SID_RXPARSER_PROTO_GRP	57
#define FDICE_SID_RXPARSER_METADATA_INIT 58
#define FDICE_SID_TXPARSER_BOOST_TCAM	66
#define FDICE_SID_RXPARSER_MARKER_GRP	72
#define FDICE_SID_RXPARSER_PG_SPILL	76
#define FDICE_SID_RXPARSER_NOMATCH_SPILL 78

/* Protocol Engine block (base 80) */
#define FDICE_SID_XLT0_PE		80
#define FDICE_SID_XLT_KB_PE		81
#define FDICE_SID_XLT1_PE		82
#define FDICE_SID_XLT2_PE		83
#define FDICE_SID_PROFID_TCAM_PE	84
#define FDICE_SID_PROFID_REDIR_PE	85
#define FDICE_SID_FLD_VEC_PE		86
#define FDICE_SID_CDID_KB_PE		87
#define FDICE_SID_CDID_REDIR_PE		88

#define FDICE_SID_RXPARSER_FLAG_REDIR	97

/* Label section ID range */
#define FDICE_SID_LBL_FIRST		0x80000010
#define FDICE_SID_LBL_RXPARSER_TMEM	0x80000018
#define FDICE_SID_LBL_LAST		0x80000038

/* -----------------------------------------------------------------------
 * Extraction Sequence (Field Vector) Types
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_fv_word - Single field vector extraction word
 * @prot_id: protocol identifier for extraction
 * @off: byte offset within the protocol header
 * @resvrd: reserved padding
 *
 * Each profile contains up to FDICE_MAX_FV_WORDS extraction words
 * that tell the hardware which protocol fields to extract.
 */
struct fdice_fv_word {
	u8	prot_id;
	u16	off;
	u8	resvrd;
} __packed;

/**
 * struct fdice_fv - Complete field vector (48 extraction words)
 * @ew: array of extraction words
 */
struct fdice_fv {
	struct fdice_fv_word ew[FDICE_MAX_FV_WORDS];
};

/* -----------------------------------------------------------------------
 * DDP Info -- Loaded package state tracked per-device
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_ddp_info - Loaded DDP package information
 * @pkg_name: human-readable name of the loaded package
 * @pkg_ver: version of the loaded package content
 * @track_id: package tracking identifier from metadata
 * @state: result of the most recent load operation
 * @has_signing_seg: package contains signing segments
 * @pkg_seg_id: segment identifier for signing verification
 * @pkg_sign_type: expected signing algorithm type
 * @pkg_copy: driver's copy of the package buffer (for reset reload)
 * @pkg_size: size of the package buffer copy
 * @fw: firmware reference from request_firmware()
 *
 * Populated during fdice_ddp_init() and exposed via devlink info.
 * The pkg_copy enables re-downloading after device reset without
 * re-requesting the firmware file.
 */
struct fdice_ddp_info {
	char			pkg_name[FDICE_PKG_NAME_SIZE];
	struct fdice_pkg_ver	pkg_ver;
	u32			track_id;
	enum fdice_ddp_state	state;
	bool			has_signing_seg;
	u32			pkg_seg_id;
	u32			pkg_sign_type;
	u8			*pkg_copy;
	u32			pkg_size;
	const struct firmware	*fw;
};

/* -----------------------------------------------------------------------
 * AQ Command Structures for DDP -- hardware interface
 * ----------------------------------------------------------------------- */

/* Download Package (indirect 0x0C40) */
#define FDICE_AQ_OPC_DOWNLOAD_PKG	0x0C40

/**
 * struct fdice_aqp_download_pkg - Download package AQ parameters
 * @flags: command flags (FDICE_AQP_DL_PKG_LAST_BUF for final buffer)
 * @reserved: reserved padding
 * @reserved1: reserved field
 * @addr_high: high 32 bits of DMA buffer address
 * @addr_low: low 32 bits of DMA buffer address
 */
struct fdice_aqp_download_pkg {
	u8	flags;
#define FDICE_AQP_DL_PKG_LAST_BUF	0x01
	u8	reserved[3];
	__le32	reserved1;
	__le32	addr_high;
	__le32	addr_low;
} __packed;
static_assert(sizeof(struct fdice_aqp_download_pkg) == 16,
	      "AQ download_pkg params must be exactly 16 bytes");

/**
 * struct fdice_aqp_download_pkg_resp - Download package AQ response
 * @error_offset: byte offset of error within package buffer
 * @error_info: firmware-specific error detail code
 * @addr_high: high 32 bits of DMA address
 * @addr_low: low 32 bits of DMA address
 */
struct fdice_aqp_download_pkg_resp {
	__le32	error_offset;
	__le32	error_info;
	__le32	addr_high;
	__le32	addr_low;
} __packed;
static_assert(sizeof(struct fdice_aqp_download_pkg_resp) == 16,
	      "AQ download_pkg_resp must be exactly 16 bytes");

/* Get Package Info List (indirect 0x0C43) */
#define FDICE_AQ_OPC_GET_PKG_INFO_LIST	0x0C43

/**
 * struct fdice_aqp_get_pkg_info_list - Get package info list AQ parameters
 * @reserved1: reserved field
 * @reserved2: reserved field
 * @addr_high: high 32 bits of DMA response buffer address
 * @addr_low: low 32 bits of DMA response buffer address
 */
struct fdice_aqp_get_pkg_info_list {
	__le32	reserved1;
	__le32	reserved2;
	__le32	addr_high;
	__le32	addr_low;
} __packed;
static_assert(sizeof(struct fdice_aqp_get_pkg_info_list) == 16,
	      "AQ get_pkg_info_list params must be exactly 16 bytes");

/**
 * struct fdice_aq_pkg_info - Single package info entry in response
 * @ver: package version
 * @name: package name string
 * @track_id: package tracking identifier
 * @is_in_nvm: package stored in NVM
 * @is_active: package is currently active
 * @is_active_at_boot: package activates at boot
 * @is_modified: package has been modified
 */
struct fdice_aq_pkg_info {
	struct fdice_pkg_ver	ver;
	char			name[FDICE_SEG_NAME_SIZE];
	__le32			track_id;
	u8			is_in_nvm;
	u8			is_active;
	u8			is_active_at_boot;
	u8			is_modified;
};

/**
 * struct fdice_aq_pkg_info_resp - Get package info list response buffer
 * @count: number of package info entries
 * @pkg_info: variable-length array of package info entries
 */
struct fdice_aq_pkg_info_resp {
	__le32				count;
	struct fdice_aq_pkg_info	pkg_info[];
};

/* Global Config Lock (indirect 0x0506 / 0x0507) */
#define FDICE_AQ_OPC_REQ_RES		0x0008
#define FDICE_AQ_OPC_REL_RES		0x0009

#define FDICE_AQ_RES_TYPE_GLOBAL_CFG	2
#define FDICE_AQ_RES_ACCESS_EXCLUSIVE	2

/* -----------------------------------------------------------------------
 * Function Declarations
 * ----------------------------------------------------------------------- */

int fdice_ddp_init(struct fdice_dev *fdev);
void fdice_ddp_cleanup(struct fdice_dev *fdev);
const char *fdice_ddp_get_state_string(enum fdice_ddp_state state);
bool fdice_ddp_is_load_successful(enum fdice_ddp_state state);

/* Flex pipe (pipeline table population) */
int fdice_flex_pipe_init(struct fdice_hw *hw, struct fdice_ice_seg *seg);
void fdice_flex_pipe_cleanup(struct fdice_hw *hw);

#endif /* _FDICE_DDP_H_ */
