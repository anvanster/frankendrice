// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_DEV_H_
#define _FDICE_DEV_H_

#include <linux/types.h>
#include <linux/pci.h>
#include <linux/netdevice.h>
#include <linux/bitfield.h>
#include <linux/idr.h>
#include <linux/xarray.h>
#include <net/devlink.h>

#include "fdice_aq.h"
#include "fdice_eq.h"
#include "fdice_health_core.h"

/* E810 PCI Device IDs -- hardware constants from E810 datasheet */
#define FDICE_DEV_ID_E810C_BACKPLANE	0x1591
#define FDICE_DEV_ID_E810C_QSFP	0x1592
#define FDICE_DEV_ID_E810C_SFP		0x1593
#define FDICE_DEV_ID_E810_XXV_BACKPLANE	0x1599
#define FDICE_DEV_ID_E810_XXV_QSFP	0x159A
#define FDICE_DEV_ID_E810_XXV_SFP	0x159B

/* Maximum AQ buffer size and large buffer threshold */
#define FDICE_AQ_MAX_BUF_LEN		4096
#define FDICE_AQ_LG_BUF_THRESH		512

/* Default AQ ring sizes */
#define FDICE_AQ_NUM_SQ_ENTRIES		32
#define FDICE_AQ_NUM_RQ_ENTRIES		32
#define FDICE_AQ_SQ_BUF_SIZE		4096
#define FDICE_AQ_RQ_BUF_SIZE		4096
#define FDICE_AQ_TIMEOUT_MS		1000

/**
 * enum fdice_mac_type - Hardware MAC generation identifier
 * @FDICE_MAC_UNKNOWN: unrecognized device ID
 * @FDICE_MAC_E810: E810 generation (100GbE)
 *
 * Used to gate generation-specific init paths. Currently only E810
 * is supported; E830 support would add a new variant here.
 */
enum fdice_mac_type {
	FDICE_MAC_UNKNOWN = 0,
	FDICE_MAC_E810,
};

/**
 * struct fdice_fw_ver - Firmware and API version information
 * @fw_branch: firmware branch identifier
 * @fw_major: firmware major version
 * @fw_minor: firmware minor version
 * @fw_patch: firmware patch level
 * @fw_build: firmware build number
 * @api_branch: API branch identifier
 * @api_major: API major version
 * @api_minor: API minor version
 * @api_patch: API patch level
 *
 * Populated by fdice_hw_get_fw_ver() from the opcode 0x0001 response.
 */
struct fdice_fw_ver {
	u8	fw_branch;
	u8	fw_major;
	u8	fw_minor;
	u8	fw_patch;
	u32	fw_build;
	u8	api_branch;
	u8	api_major;
	u8	api_minor;
	u8	api_patch;
};

/**
 * struct fdice_caps - Device/function capability summary
 * @valid_functions: bitmask of valid PF numbers
 * @num_vsis: maximum number of VSIs
 * @num_rxq: maximum receive queues
 * @num_txq: maximum transmit queues
 * @num_msix: maximum MSI-X vectors
 * @max_mtu: maximum supported MTU
 * @rss_table_size: RSS indirection table entries
 * @rss_table_entry_width: bits per RSS table entry
 * @num_vfs: maximum VFs for SR-IOV
 * @fd_fltr_guar: guaranteed FD filter count
 * @fd_fltr_best_effort: best-effort FD filter count
 * @dcb_capable: DCB support present
 * @rdma_capable: RDMA support present
 * @ieee_1588: 1588 PTP support present
 *
 * Aggregated from capability elements returned by opcodes 0x000A/0x000B.
 */
struct fdice_caps {
	u32	valid_functions;
	u16	num_vsis;
	u32	num_rxq;
	u32	num_txq;
	u32	num_msix;
	u32	max_mtu;
	u32	rss_table_size;
	u32	rss_table_entry_width;
	u32	num_vfs;
	u32	fd_fltr_guar;
	u32	fd_fltr_best_effort;
	bool	dcb_capable;
	bool	rdma_capable;
	bool	ieee_1588;
};

/**
 * struct fdice_port_info - Per-port state
 * @hw: backpointer to parent hw struct
 * @lport: logical port number
 * @link_up: current link state
 * @link_speed: current link speed (FDICE_AQ_LINK_SPEED_*)
 * @phy_type_low: low 64-bit PHY type bitmap
 * @phy_type_high: high 64-bit PHY type bitmap
 *
 * Allocated during function_setup, populated by link status queries.
 */
struct fdice_port_info {
	struct fdice_hw		*hw;
	u8			lport;
	bool			link_up;
	u16			link_speed;
	u64			phy_type_low;
	u64			phy_type_high;
};

/* Forward declaration for hw_ops vtable */
struct fdice_hw;

/**
 * struct fdice_hw_ops - Hardware operation vtable grouped by subsystem
 *
 * Each subsystem provides type-safe wrappers around raw AQ commands.
 * Phase 3.2 populates the core subsystem; others are NULL stubs
 * filled in by later phases.
 */
struct fdice_hw_ops {
	/* Core / Resource Management */
	struct {
		int (*get_fw_ver)(struct fdice_hw *hw,
				  struct fdice_fw_ver *ver);
		int (*send_driver_ver)(struct fdice_hw *hw);
		int (*get_func_caps)(struct fdice_hw *hw,
				     struct fdice_caps *caps);
		int (*get_dev_caps)(struct fdice_hw *hw,
				    struct fdice_caps *caps);
	} core;

	/* PHY / Link -- populated in Phase 3.7 */
	struct {
		int (*get_link_status)(struct fdice_hw *hw,
				       struct fdice_link_status_data *status);
		int (*set_event_mask)(struct fdice_hw *hw, u16 mask);
	} phy;
};

/**
 * enum fdice_state_bits - Device state flags
 * @FDICE_STATE_UP: Interface fully initialized and operational
 * @FDICE_STATE_RECOVERY: Device is undergoing health recovery
 * @FDICE_STATE_REMOVING: Device is being removed (PCI remove path)
 * @FDICE_STATE_RESET_IN_PROGRESS: Reset sequence active
 *
 * State bits gate teardown paths and prevent double-free on partial
 * init (mlx5 MLX5_INTERFACE_STATE_UP pattern, Exploration 03).
 */
enum fdice_state_bits {
	FDICE_STATE_UP,
	FDICE_STATE_RECOVERY,
	FDICE_STATE_REMOVING,
	FDICE_STATE_RESET_IN_PROGRESS,
};

/**
 * struct fdice_hw - Hardware interface state
 * @hw_addr: PCI BAR0 MMIO base address
 * @pdev: PCI device backpointer
 * @vendor_id: PCI vendor ID
 * @device_id: PCI device ID
 * @subsystem_vendor_id: PCI subsystem vendor ID
 * @subsystem_device_id: PCI subsystem device ID
 * @revision_id: PCI revision ID
 * @pf_id: Physical function number
 * @mac_type: MAC generation detected from device ID
 * @fw: Firmware and API version info (from opcode 0x0001)
 * @func_caps: Function-level capabilities cache
 * @dev_caps: Device-level capabilities cache
 * @port_info: Per-port state (allocated in function_setup)
 * @ops: Hardware operation vtable
 * @aq_initialized: True after control queues are fully operational
 * @num_sq_entries: Admin send queue depth
 * @num_rq_entries: Admin receive queue depth
 * @sq_buf_size: Admin send queue buffer size
 * @rq_buf_size: Admin receive queue buffer size
 * @aq_timeout_ms: AQ command timeout in milliseconds
 *
 * Populated during fdice_pci_init() (PCI identity fields) and
 * fdice_function_setup() (AQ, FW version, capabilities).
 */
struct fdice_hw {
	u8 __iomem		*hw_addr;
	struct pci_dev		*pdev;

	u16			vendor_id;
	u16			device_id;
	u16			subsystem_vendor_id;
	u16			subsystem_device_id;
	u8			revision_id;
	u8			pf_id;

	/* MAC type detected from device ID */
	enum fdice_mac_type	mac_type;

	/* Admin Queue instance */
	struct fdice_aq		adminq;

	/* Firmware version (populated by get_fw_ver) */
	struct fdice_fw_ver	fw;

	/* Capability caches */
	struct fdice_caps	func_caps;
	struct fdice_caps	dev_caps;

	/* Port state */
	struct fdice_port_info	*port_info;

	/* Hardware operation vtable */
	const struct fdice_hw_ops *ops;

	/* Admin Queue parameters */
	bool			aq_initialized;
	u16			num_sq_entries;
	u16			num_rq_entries;
	u16			sq_buf_size;
	u16			rq_buf_size;
	u32			aq_timeout_ms;
};

/* Forward declarations for queue management */
struct fdice_channel;

/* Forward declaration for flow steering */
struct fdice_flow_steering;

/**
 * struct fdice_priv - Networking / data-path private data
 * @fdev: back-pointer to the core fdice_dev
 * @netdev: network device (NULL until Phase 3.11 netdev registration)
 * @num_channels: number of active channels (mirrors fdice_dev.num_channels)
 *
 * Provides the netdev-facing context that channels and queues reference.
 * Analogous to mlx5e_priv. Embedded in fdice_dev so that channel code
 * can reach both the hardware (via fdev) and the netdev in a cache-
 * friendly manner.
 */
struct fdice_priv {
	struct fdice_dev	*fdev;
	struct net_device	*netdev;
	u16			num_channels;
};

/**
 * struct fdice_dev - Core FrankenDrICE device
 * @devlink: Devlink instance (container_of target)
 * @pdev: PCI device
 * @hw: Hardware interface state
 * @state: Bitmask of &enum fdice_state_bits
 * @devlink_lock: Serializes devlink reload vs other operations
 *
 * Embedded in devlink allocation — use fdice_dev_from_devlink() and
 * fdice_devlink_from_dev() for conversion. This is the top-level
 * per-device structure, analogous to mlx5_core_dev.
 *
 * Design: devlink-first allocation (mlx5 pattern from Exploration 01).
 * The init layers attach subsystems progressively:
 *   Layer 1: fdice_mdev_init()    — mutexes, debug, health timer
 *   Layer 2: fdice_pci_init()     — PCI enable, BAR map, hw struct
 *   Layer 3: fdice_function_setup() — AQ init, FW handshake, caps
 *   Layer 4: fdice_init_once()    — one-time SW subsystem alloc
 *   Layer 5: fdice_load()         — HW activation, queue setup
 *   Layer 6: fdice_register_device() — netdev, devlink, auxiliary
 */
struct fdice_dev {
	struct pci_dev		*pdev;
	struct fdice_hw		hw;
	unsigned long		state;
	struct mutex		devlink_lock;

	/* Networking / data-path private data */
	struct fdice_priv	priv;

	/* Event queue table (MSI-X vectors, interrupt management) */
	struct fdice_eq_table	eq_table;

	/* Health monitoring subsystem (Phase 3.4) */
	struct fdice_health	health;

	/* Flow steering subsystem (Phase 3.5) */
	struct fdice_flow_steering *flow_steering;

	/* Channel array and count (Phase 3.3 queue management) */
	struct fdice_channel	*channels;
	u16			num_channels;
};

/**
 * fdice_dev_from_devlink - Get fdice_dev from devlink instance
 * @devlink: devlink pointer
 *
 * Return: pointer to the containing fdice_dev
 */
static inline struct fdice_dev *fdice_dev_from_devlink(struct devlink *devlink)
{
	return devlink_priv(devlink);
}

/**
 * fdice_devlink_from_dev - Get devlink from fdice_dev
 * @fdev: fdice device pointer
 *
 * Return: pointer to the containing devlink instance
 */
static inline struct devlink *fdice_devlink_from_dev(struct fdice_dev *fdev)
{
	return priv_to_devlink(fdev);
}

/* ---- Load / unload (fdice_main.c) ---- */

int fdice_load(struct fdice_dev *fdev);
void fdice_unload(struct fdice_dev *fdev);

/* ---- Hardware init / teardown (fdice_hw.c) ---- */

int fdice_hw_init_params(struct fdice_hw *hw);
int fdice_function_setup(struct fdice_dev *fdev);
void fdice_function_teardown(struct fdice_dev *fdev);

int fdice_hw_get_fw_ver(struct fdice_hw *hw, struct fdice_fw_ver *ver);
int fdice_hw_send_driver_ver(struct fdice_hw *hw);
int fdice_discover_caps(struct fdice_hw *hw, enum fdice_aq_opc opc,
			struct fdice_caps *caps);

extern const struct fdice_hw_ops fdice_hw_ops;

#endif /* _FDICE_DEV_H_ */
