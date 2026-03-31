// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_AQ_CMD_H_
#define _FDICE_AQ_CMD_H_

/**
 * fdice_aq_cmd.h - Admin Queue command parameter structs
 *
 * These structs define the 16-byte parameter layouts embedded in each
 * AQ descriptor's params union. The field layouts are dictated by E810
 * firmware and must match the hardware protocol exactly (below-AQ boundary).
 *
 * Naming uses fdice_ prefix per project convention, but binary layout
 * matches the E810 firmware interface specification.
 *
 * All multi-byte hardware fields use __le16/__le32/__le64 for endianness
 * safety. All structs are __packed to prevent compiler padding.
 */

#include <linux/types.h>
#include <linux/build_bug.h>

/* ---- AQ Opcodes (firmware-defined constants) ---- */

enum fdice_aq_opc {
	/* General admin commands */
	FDICE_AQ_OPC_GET_VER			= 0x0001,
	FDICE_AQ_OPC_DRIVER_VER		= 0x0002,
	FDICE_AQ_OPC_Q_SHUTDOWN		= 0x0003,

	/* Capability discovery */
	FDICE_AQ_OPC_LIST_FUNC_CAPS		= 0x000A,
	FDICE_AQ_OPC_LIST_DEV_CAPS		= 0x000B,

	/* PHY / Link management */
	FDICE_AQ_OPC_GET_LINK_STATUS		= 0x0607,
	FDICE_AQ_OPC_SET_EVENT_MASK		= 0x0613,
};

/* ---- Generic 16-byte parameter block ---- */

/**
 * struct fdice_aqp_generic - Raw 16-byte AQ parameter block
 * @param0: first 32-bit generic parameter
 * @param1: second 32-bit generic parameter
 * @addr_high: high 32 bits of DMA address for indirect commands
 * @addr_low: low 32 bits of DMA address for indirect commands
 *
 * Used for commands without a dedicated parameter struct, or as a
 * raw overlay when constructing descriptors manually.
 */
struct fdice_aqp_generic {
	__le32	param0;
	__le32	param1;
	__le32	addr_high;
	__le32	addr_low;
} __packed;
static_assert(sizeof(struct fdice_aqp_generic) == 16,
	      "AQ generic params must be exactly 16 bytes");

/* ---- Get Version (direct 0x0001) ---- */

/**
 * struct fdice_aqp_get_ver - Firmware version response parameters
 * @rom_ver: ROM version from NVM
 * @fw_build: firmware build number
 * @fw_branch: firmware branch identifier
 * @fw_major: firmware major version
 * @fw_minor: firmware minor version
 * @fw_patch: firmware patch level
 * @api_branch: API branch identifier
 * @api_major: API major version
 * @api_minor: API minor version
 * @api_patch: API patch level
 *
 * Response to opcode 0x0001 (Get Version). This is a direct command
 * with no indirect buffer -- all fields returned in the descriptor
 * params overlay. Firmware populates these fields on completion.
 */
struct fdice_aqp_get_ver {
	__le32	rom_ver;
	__le32	fw_build;
	u8	fw_branch;
	u8	fw_major;
	u8	fw_minor;
	u8	fw_patch;
	u8	api_branch;
	u8	api_major;
	u8	api_minor;
	u8	api_patch;
} __packed;
static_assert(sizeof(struct fdice_aqp_get_ver) == 16,
	      "AQ get_ver params must be exactly 16 bytes");

/* ---- Send Driver Version (indirect 0x0002) ---- */

/**
 * struct fdice_aqp_driver_ver - Driver version command parameters
 * @major_ver: driver major version
 * @minor_ver: driver minor version
 * @build_ver: driver build version
 * @subbuild_ver: driver sub-build version
 * @rsvd: reserved padding (firmware ignores)
 * @addr_high: high 32 bits of DMA address for driver string buffer
 * @addr_low: low 32 bits of DMA address for driver string buffer
 *
 * Parameters for opcode 0x0002 (Send Driver Version). This is an
 * indirect command -- the driver string is passed in a DMA buffer
 * pointed to by addr_high:addr_low, with the RD flag set.
 */
struct fdice_aqp_driver_ver {
	u8	major_ver;
	u8	minor_ver;
	u8	build_ver;
	u8	subbuild_ver;
	u8	rsvd[4];
	__le32	addr_high;
	__le32	addr_low;
} __packed;
static_assert(sizeof(struct fdice_aqp_driver_ver) == 16,
	      "AQ driver_ver params must be exactly 16 bytes");

/* ---- List Capabilities (indirect 0x000A / 0x000B) ---- */

/**
 * struct fdice_aqp_list_caps - Capability list query parameters
 * @cmd_flags: command flags (reserved, set to 0)
 * @pf_index: PF index to query (for device caps, identifies target PF)
 * @rsvd: reserved padding
 * @count: on response, number of capability elements returned
 * @addr_high: high 32 bits of DMA buffer address
 * @addr_low: low 32 bits of DMA buffer address
 *
 * Used for both function capabilities (0x000A) and device capabilities
 * (0x000B). The response is an indirect buffer containing an array of
 * fdice_aq_caps_elem entries. The @count field is populated by firmware
 * on completion to indicate how many elements were written.
 */
struct fdice_aqp_list_caps {
	u8	cmd_flags;
	u8	pf_index;
	u8	rsvd[2];
	__le32	count;
	__le32	addr_high;
	__le32	addr_low;
} __packed;
static_assert(sizeof(struct fdice_aqp_list_caps) == 16,
	      "AQ list_caps params must be exactly 16 bytes");

/**
 * struct fdice_aq_caps_elem - Single capability element in response buffer
 * @cap: capability identifier (FDICE_AQ_CAP_* constants)
 * @major_ver: capability major version
 * @minor_ver: capability minor version
 * @number: number of resources of this type
 * @logical_id: logical identifier (meaning varies by cap type)
 * @phys_id: physical identifier (meaning varies by cap type)
 * @rsvd1: reserved
 * @rsvd2: reserved
 *
 * Repeated per reported capability in the indirect response buffer
 * for opcodes 0x000A and 0x000B. Each element is 32 bytes.
 */
struct fdice_aq_caps_elem {
	__le16	cap;
	u8	major_ver;
	u8	minor_ver;
	__le32	number;
	__le32	logical_id;
	__le32	phys_id;
	__le64	rsvd1;
	__le64	rsvd2;
} __packed;
static_assert(sizeof(struct fdice_aq_caps_elem) == 32,
	      "AQ caps element must be exactly 32 bytes");

/* Capability identifiers used in fdice_aq_caps_elem.cap */
#define FDICE_AQ_CAP_VALID_FUNCTIONS	0x0005
#define FDICE_AQ_CAP_SRIOV		0x0012
#define FDICE_AQ_CAP_VF		0x0013
#define FDICE_AQ_CAP_VSI		0x0017
#define FDICE_AQ_CAP_DCB		0x0018
#define FDICE_AQ_CAP_RSS		0x0040
#define FDICE_AQ_CAP_RXQS		0x0041
#define FDICE_AQ_CAP_TXQS		0x0042
#define FDICE_AQ_CAP_MSIX		0x0043
#define FDICE_AQ_CAP_FD			0x0045
#define FDICE_AQ_CAP_1588		0x0046
#define FDICE_AQ_CAP_MAX_MTU		0x0047
#define FDICE_AQ_CAP_RDMA		0x0051

/* ---- Get Link Status (indirect 0x0607) ---- */

/**
 * struct fdice_aqp_get_link_status - Link status query parameters
 * @lport_num: logical port number to query
 * @rsvd: reserved
 * @cmd_flags: command flags controlling link status event subscription
 * @rsvd2: reserved padding
 * @addr_high: high 32 bits of indirect response buffer address
 * @addr_low: low 32 bits of indirect response buffer address
 *
 * Parameters for opcode 0x0607 (Get Link Status). Also used by the
 * firmware for asynchronous Link Status Event notifications on the ARQ.
 * The indirect buffer receives a fdice_link_status_data structure.
 */
struct fdice_aqp_get_link_status {
	u8	lport_num;
	u8	rsvd;
	__le16	cmd_flags;
	__le32	rsvd2;
	__le32	addr_high;
	__le32	addr_low;
} __packed;
static_assert(sizeof(struct fdice_aqp_get_link_status) == 16,
	      "AQ get_link_status params must be exactly 16 bytes");

/* Link status event subscription flags for cmd_flags */
#define FDICE_AQ_LSE_M			0x3
#define FDICE_AQ_LSE_NOP		0x0
#define FDICE_AQ_LSE_DIS		0x2
#define FDICE_AQ_LSE_ENA		0x3
#define FDICE_AQ_LSE_IS_ENABLED		0x1	/* response only */

/**
 * struct fdice_link_status_data - Link status indirect response buffer
 * @topo_media_conflict: topology and media conflict flags
 * @link_cfg_err: link configuration error flags
 * @link_info: link state flags (up/down, fault, media)
 * @an_info: autonegotiation state flags
 * @ext_info: extended info (temp alarm, excessive errors)
 * @rsvd2: reserved
 * @max_frame_size: current maximum frame size
 * @cfg: FEC and pacing configuration
 * @power_desc: external device power class
 * @link_speed: link speed bitmap
 * @rsvd3: alignment padding
 * @ext_fec_status: extended FEC status
 * @rsvd4: reserved
 * @phy_type_low: low 64-bit PHY type bitmap
 * @phy_type_high: high 64-bit PHY type bitmap
 *
 * Indirect response buffer for opcode 0x0607. Fields below phy_type_high
 * are v2 extensions for link partner data (not included here for Phase 3.2).
 */
struct fdice_link_status_data {
	u8	topo_media_conflict;
#define FDICE_AQ_LINK_TOPO_CONFLICT	BIT(0)
#define FDICE_AQ_LINK_MEDIA_CONFLICT	BIT(1)
	u8	link_cfg_err;
	u8	link_info;
#define FDICE_AQ_LINK_UP		BIT(0)
#define FDICE_AQ_LINK_FAULT		BIT(1)
#define FDICE_AQ_LINK_FAULT_TX		BIT(2)
#define FDICE_AQ_LINK_FAULT_RX		BIT(3)
#define FDICE_AQ_LINK_FAULT_REMOTE	BIT(4)
#define FDICE_AQ_LINK_UP_PORT		BIT(5)
#define FDICE_AQ_MEDIA_AVAILABLE	BIT(6)
#define FDICE_AQ_SIGNAL_DETECT		BIT(7)
	u8	an_info;
#define FDICE_AQ_AN_COMPLETED		BIT(0)
#define FDICE_AQ_FEC_EN			BIT(3)
#define FDICE_AQ_LINK_PAUSE_TX		BIT(5)
#define FDICE_AQ_LINK_PAUSE_RX		BIT(6)
	u8	ext_info;
	u8	rsvd2;
	__le16	max_frame_size;
	u8	cfg;
#define FDICE_AQ_FEC_MASK		GENMASK(2, 0)
	u8	power_desc;
	__le16	link_speed;
#define FDICE_AQ_LINK_SPEED_10MB	BIT(0)
#define FDICE_AQ_LINK_SPEED_100MB	BIT(1)
#define FDICE_AQ_LINK_SPEED_1000MB	BIT(2)
#define FDICE_AQ_LINK_SPEED_2500MB	BIT(3)
#define FDICE_AQ_LINK_SPEED_5GB		BIT(4)
#define FDICE_AQ_LINK_SPEED_10GB	BIT(5)
#define FDICE_AQ_LINK_SPEED_25GB	BIT(7)
#define FDICE_AQ_LINK_SPEED_40GB	BIT(8)
#define FDICE_AQ_LINK_SPEED_50GB	BIT(9)
#define FDICE_AQ_LINK_SPEED_100GB	BIT(10)
	__le16	rsvd3;
	u8	ext_fec_status;
	u8	rsvd4;
	__le64	phy_type_low;
	__le64	phy_type_high;
} __packed;

/* ---- Set Event Mask (direct 0x0613) ---- */

/**
 * struct fdice_aqp_set_event_mask - Event mask configuration parameters
 * @lport_num: logical port number
 * @rsvd: reserved padding (7 bytes to align event_mask)
 * @event_mask: bitmask of enabled link events
 * @rsvd1: reserved trailing padding
 *
 * Parameters for opcode 0x0613 (Set Event Mask). Controls which
 * asynchronous link events the firmware sends to the ARQ.
 * This is a direct command -- no indirect buffer.
 */
struct fdice_aqp_set_event_mask {
	u8	lport_num;
	u8	rsvd[7];
	__le16	event_mask;
	u8	rsvd1[6];
} __packed;
static_assert(sizeof(struct fdice_aqp_set_event_mask) == 16,
	      "AQ set_event_mask params must be exactly 16 bytes");

/* Event mask bit definitions for fdice_aqp_set_event_mask.event_mask */
#define FDICE_AQ_LINK_EVENT_UPDOWN		BIT(1)
#define FDICE_AQ_LINK_EVENT_MEDIA_NA		BIT(2)
#define FDICE_AQ_LINK_EVENT_LINK_FAULT		BIT(3)
#define FDICE_AQ_LINK_EVENT_PHY_TEMP_ALARM	BIT(4)
#define FDICE_AQ_LINK_EVENT_EXCESSIVE_ERRORS	BIT(5)
#define FDICE_AQ_LINK_EVENT_SIGNAL_DETECT	BIT(6)
#define FDICE_AQ_LINK_EVENT_AN_COMPLETED	BIT(7)
#define FDICE_AQ_LINK_EVENT_MODULE_QUAL_FAIL	BIT(8)
#define FDICE_AQ_LINK_EVENT_PORT_TX_SUSPENDED	BIT(9)
#define FDICE_AQ_LINK_EVENT_PHY_FW_LOAD_FAIL	BIT(12)

/* ---- AQ Descriptor (hardware-fixed, 32 bytes) ---- */

/**
 * struct fdice_aq_desc - Admin Queue descriptor
 * @flags: control flags (DD, CMP, ERR, BUF, LB, RD, etc.)
 * @opcode: AQ command opcode (firmware-defined)
 * @datalen: length of indirect data buffer
 * @retval: firmware return value
 * @cookie_high: opaque driver data (completion token high)
 * @cookie_low: opaque driver data (completion token low)
 * @params: command-specific parameters (16-byte union)
 *
 * Binary layout dictated by E810 firmware. The cookie fields carry
 * fdice_ completion tokens for matching responses to commands.
 */
struct fdice_aq_desc {
	__le16	flags;
	__le16	opcode;
	__le16	datalen;
	__le16	retval;
	__le32	cookie_high;
	__le32	cookie_low;
	union {
		u8				raw[16];
		struct fdice_aqp_generic	generic;
	} params;
};
static_assert(sizeof(struct fdice_aq_desc) == 32,
	      "AQ descriptor must be exactly 32 bytes");

/* Descriptor flags -- bit positions are hardware-fixed */
#define FDICE_AQ_FLAG_DD	BIT(0)	 /* Descriptor Done (FW->Driver) */
#define FDICE_AQ_FLAG_CMP	BIT(1)	 /* Completion (FW->Driver) */
#define FDICE_AQ_FLAG_ERR	BIT(2)	 /* Error (FW->Driver) */
#define FDICE_AQ_FLAG_VFE	BIT(3)	 /* VF Error (FW->Driver) */
#define FDICE_AQ_FLAG_LB	BIT(9)	 /* Large Buffer (Driver->FW) */
#define FDICE_AQ_FLAG_RD	BIT(10)	 /* Read direction (Driver->FW) */
#define FDICE_AQ_FLAG_VFC	BIT(11)	 /* VF Command (Driver->FW) */
#define FDICE_AQ_FLAG_BUF	BIT(12)	 /* Buffer present (Driver->FW) */
#define FDICE_AQ_FLAG_SI	BIT(13)	 /* Security (Driver->FW) */
#define FDICE_AQ_FLAG_EI	BIT(14)	 /* Error Interrupt (Driver->FW) */
#define FDICE_AQ_FLAG_FE	BIT(15)	 /* Fatal Error (Driver->FW) */

#endif /* _FDICE_AQ_CMD_H_ */
