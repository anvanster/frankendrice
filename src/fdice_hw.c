// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_hw.c - Hardware initialization and AQ command wrappers
 *
 * Phase 3.2 implementation: control queue bring-up, firmware version
 * handshake, driver version announcement, capability discovery, and
 * port info initialization.
 *
 * Design rationale:
 *   - function_setup orchestrates the full firmware handshake sequence
 *     (init AQ -> get FW version -> send driver version -> discover caps)
 *   - Each AQ command has a type-safe wrapper that builds the descriptor,
 *     calls fdice_aq_send_cmd(), and parses the response into domain structs
 *   - MAC type detection maps PCI device IDs to generation identifiers
 *   - The hw_ops vtable exposes the core subsystem wrappers; other
 *     subsystems are NULL stubs until later phases
 *
 * All functions use fdice_ prefix. Binary layouts of AQ parameter structs
 * match E810 firmware expectations (below-AQ boundary), but naming and
 * higher-level abstractions are original.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/string.h>
#include <linux/ctype.h>
#include <linux/etherdevice.h>

#include "fdice_dev.h"

/* Driver version constants for opcode 0x0002 */
#define FDICE_DRV_VER_MAJOR	0
#define FDICE_DRV_VER_MINOR	1
#define FDICE_DRV_VER_BUILD	0
#define FDICE_DRV_VER_SUBBUILD	0

static const char fdice_drv_string[] = "frankendrice";

/* ---- MAC type detection ---- */

/**
 * fdice_detect_mac_type - Map PCI device ID to MAC generation
 * @hw: hardware instance with device_id already populated
 *
 * Sets hw->mac_type based on the PCI device ID. Currently all
 * supported IDs are E810 variants. Returns -ENODEV for unrecognized
 * device IDs to prevent operating on unsupported hardware.
 *
 * Return: 0 on success, -ENODEV for unrecognized device ID.
 */
static int fdice_detect_mac_type(struct fdice_hw *hw)
{
	switch (hw->device_id) {
	case FDICE_DEV_ID_E810C_BACKPLANE:
	case FDICE_DEV_ID_E810C_QSFP:
	case FDICE_DEV_ID_E810C_SFP:
	case FDICE_DEV_ID_E810_XXV_BACKPLANE:
	case FDICE_DEV_ID_E810_XXV_QSFP:
	case FDICE_DEV_ID_E810_XXV_SFP:
		hw->mac_type = FDICE_MAC_E810;
		break;
	default:
		hw->mac_type = FDICE_MAC_UNKNOWN;
		dev_err(&hw->pdev->dev,
			"unrecognized device ID 0x%04x, cannot determine MAC type\n",
			hw->device_id);
		return -ENODEV;
	}

	return 0;
}

/* ---- AQ command wrappers ---- */

/**
 * fdice_hw_get_fw_ver - Query firmware and API version
 * @hw: hardware instance
 * @ver: output structure for version data
 *
 * Issues opcode 0x0001 (Get Version) as a direct command. The firmware
 * returns version info in the descriptor params -- no indirect buffer
 * needed. Parsed fields are written into the caller's fdice_fw_ver.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_hw_get_fw_ver(struct fdice_hw *hw, struct fdice_fw_ver *ver)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_get_ver *resp;
	int err;

	if (!ver)
		return -EINVAL;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_GET_VER);
	resp = (struct fdice_aqp_get_ver *)desc.params.raw;

	err = fdice_aq_send_cmd(hw, &desc, NULL, 0);
	if (err)
		return err;

	ver->fw_branch = resp->fw_branch;
	ver->fw_major  = resp->fw_major;
	ver->fw_minor  = resp->fw_minor;
	ver->fw_patch  = resp->fw_patch;
	ver->fw_build  = le32_to_cpu(resp->fw_build);
	ver->api_branch = resp->api_branch;
	ver->api_major  = resp->api_major;
	ver->api_minor  = resp->api_minor;
	ver->api_patch  = resp->api_patch;

	return 0;
}

/**
 * fdice_hw_clear_pf_cfg - Clear all PF-owned firmware state
 * @hw: hardware instance
 *
 * Issues opcode 0x0205 (Clear PF Config). This is a direct command
 * with no parameters that instructs the E810 firmware to release all
 * resources (VSIs, queues, switch elements) previously allocated to
 * this PF. It MUST be called immediately after admin queue init and
 * before any resource allocation — otherwise the firmware may refuse
 * new allocations with EACCES if it still holds stale state from a
 * previous driver load.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_hw_clear_pf_cfg(struct fdice_hw *hw)
{
	struct fdice_aq_desc desc;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_CLEAR_PF_CFG);
	return fdice_aq_send_cmd(hw, &desc, NULL, 0);
}

/**
 * fdice_hw_send_driver_ver - Announce driver version to firmware
 * @hw: hardware instance
 *
 * Issues opcode 0x0002 (Send Driver Version) with the FrankenDrICE
 * version string. This is an indirect command -- the driver string
 * is passed in a DMA-addressable buffer. The firmware logs this
 * for diagnostics but the response carries no meaningful data.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_hw_send_driver_ver(struct fdice_hw *hw)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_driver_ver *cmd;
	u16 slen;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_DRIVER_VER);
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD);

	cmd = (struct fdice_aqp_driver_ver *)desc.params.raw;
	cmd->major_ver    = FDICE_DRV_VER_MAJOR;
	cmd->minor_ver    = FDICE_DRV_VER_MINOR;
	cmd->build_ver    = FDICE_DRV_VER_BUILD;
	cmd->subbuild_ver = FDICE_DRV_VER_SUBBUILD;

	/*
	 * Compute the length of the ASCII driver string to send.
	 * Only ASCII characters are accepted by the firmware; stop
	 * at the first non-ASCII or NUL byte.
	 */
	for (slen = 0; slen < sizeof(fdice_drv_string) - 1; slen++) {
		if (!isascii(fdice_drv_string[slen]) ||
		    !fdice_drv_string[slen])
			break;
	}

	return fdice_aq_send_cmd(hw, &desc,
				 (void *)fdice_drv_string, slen);
}

/**
 * fdice_aq_list_caps - Issue a list-capabilities AQ command
 * @hw: hardware instance
 * @buf: caller-allocated buffer for capability elements
 * @buf_size: size of @buf in bytes
 * @cap_count: on output, number of capability elements returned
 * @opc: opcode -- FDICE_AQ_OPC_LIST_FUNC_CAPS or FDICE_AQ_OPC_LIST_DEV_CAPS
 *
 * Low-level wrapper for opcodes 0x000A and 0x000B. The indirect buffer
 * receives an array of fdice_aq_caps_elem structures. The count field
 * in the descriptor params is populated by firmware on completion.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_aq_list_caps(struct fdice_hw *hw, void *buf, u16 buf_size,
			      u32 *cap_count, enum fdice_aq_opc opc)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_list_caps *cmd;
	int err;

	if (opc != FDICE_AQ_OPC_LIST_FUNC_CAPS &&
	    opc != FDICE_AQ_OPC_LIST_DEV_CAPS)
		return -EINVAL;

	fdice_aq_prep_desc(&desc, opc);
	cmd = (struct fdice_aqp_list_caps *)desc.params.raw;

	err = fdice_aq_send_cmd(hw, &desc, buf, buf_size);

	if (cap_count)
		*cap_count = le32_to_cpu(cmd->count);

	return err;
}

/**
 * fdice_parse_caps - Parse capability elements into a caps structure
 * @caps: output capability summary
 * @buf: buffer containing fdice_aq_caps_elem array
 * @count: number of elements in @buf
 *
 * Iterates the capability element array and extracts recognized
 * capabilities into the aggregated fdice_caps structure. Unrecognized
 * capability IDs are silently skipped -- firmware may report more
 * capabilities than the driver cares about.
 */
static void fdice_parse_caps(struct fdice_caps *caps,
			     struct fdice_aq_caps_elem *buf, u32 count)
{
	u32 i;

	memset(caps, 0, sizeof(*caps));

	for (i = 0; i < count; i++) {
		u16 cap_id = le16_to_cpu(buf[i].cap);
		u32 number = le32_to_cpu(buf[i].number);
		u32 logical_id = le32_to_cpu(buf[i].logical_id);
		u32 phys_id = le32_to_cpu(buf[i].phys_id);

		switch (cap_id) {
		case FDICE_AQ_CAP_VALID_FUNCTIONS:
			caps->valid_functions = number;
			break;
		case FDICE_AQ_CAP_VSI:
			caps->num_vsis = (u16)number;
			break;
		case FDICE_AQ_CAP_RXQS:
			caps->num_rxq = number;
			break;
		case FDICE_AQ_CAP_TXQS:
			caps->num_txq = number;
			break;
		case FDICE_AQ_CAP_MSIX:
			caps->num_msix = number;
			break;
		case FDICE_AQ_CAP_MAX_MTU:
			caps->max_mtu = number;
			break;
		case FDICE_AQ_CAP_RSS:
			caps->rss_table_size = number;
			caps->rss_table_entry_width = logical_id;
			break;
		case FDICE_AQ_CAP_SRIOV:
			/* number unused; VF count comes from VF cap */
			break;
		case FDICE_AQ_CAP_VF:
			caps->num_vfs = number;
			break;
		case FDICE_AQ_CAP_FD:
			caps->fd_fltr_guar = number;
			caps->fd_fltr_best_effort = logical_id;
			break;
		case FDICE_AQ_CAP_DCB:
			caps->dcb_capable = (number > 0);
			break;
		case FDICE_AQ_CAP_RDMA:
			caps->rdma_capable = (number > 0);
			break;
		case FDICE_AQ_CAP_1588:
			caps->ieee_1588 = (number > 0);
			break;
		default:
			/* Unknown capability -- skip silently */
			break;
		}

		/* Suppress unused-variable warnings for completeness */
		(void)phys_id;
	}
}

/**
 * fdice_discover_caps - Discover and parse capabilities for a given scope
 * @hw: hardware instance
 * @opc: FDICE_AQ_OPC_LIST_FUNC_CAPS or FDICE_AQ_OPC_LIST_DEV_CAPS
 * @caps: output capability summary
 *
 * Allocates a max-size buffer, queries firmware for capability elements,
 * parses them into the aggregated caps structure, and frees the buffer.
 * The max buffer (4096 bytes) is always used to avoid the firmware
 * returning ENOMEM for truncated responses.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_discover_caps(struct fdice_hw *hw, enum fdice_aq_opc opc,
			struct fdice_caps *caps)
{
	struct fdice_aq_caps_elem *cbuf;
	u32 cap_count = 0;
	int err;

	cbuf = kzalloc(FDICE_AQ_MAX_BUF_LEN, GFP_KERNEL);
	if (!cbuf)
		return -ENOMEM;

	cap_count = FDICE_AQ_MAX_BUF_LEN / sizeof(*cbuf);

	err = fdice_aq_list_caps(hw, cbuf, FDICE_AQ_MAX_BUF_LEN,
				 &cap_count, opc);
	if (!err)
		fdice_parse_caps(caps, cbuf, cap_count);

	kfree(cbuf);
	return err;
}

/* ---- Hardware init parameter defaults ---- */

/**
 * fdice_hw_init_params - Set default AQ sizes, timeouts, and MAC type
 * @hw: hardware instance with device_id already populated
 *
 * Called early in function_setup before AQ initialization. Sets
 * conservative default parameters for the admin queue rings and
 * detects the MAC generation from the PCI device ID.
 *
 * Return: 0 on success, -ENODEV for unrecognized device.
 */
int fdice_hw_init_params(struct fdice_hw *hw)
{
	int err;

	err = fdice_detect_mac_type(hw);
	if (err)
		return err;

	/* Admin queue ring parameters -- conservative defaults */
	hw->num_sq_entries = FDICE_AQ_NUM_SQ_ENTRIES;
	hw->num_rq_entries = FDICE_AQ_NUM_RQ_ENTRIES;
	hw->sq_buf_size    = FDICE_AQ_SQ_BUF_SIZE;
	hw->rq_buf_size    = FDICE_AQ_RQ_BUF_SIZE;
	hw->aq_timeout_ms  = FDICE_AQ_TIMEOUT_MS;

	/* Attach the hw_ops vtable */
	hw->ops = &fdice_hw_ops;

	return 0;
}

/* ---- Function setup / teardown (Layer 3) ---- */

/**
 * fdice_function_setup - Initialize hardware interface via admin queue
 * @fdev: fdice device
 *
 * Third init layer. Performs the full firmware handshake sequence:
 *   1. Detect MAC type and set AQ parameters
 *   2. Initialize admin control queues (DMA rings)
 *   3. Get firmware version (opcode 0x0001)
 *   4. Send driver version (opcode 0x0002)
 *   5. Discover function capabilities (opcode 0x000A)
 *   6. Discover device capabilities (opcode 0x000B)
 *   7. Allocate and initialize port info
 *
 * Uses goto-chain cleanup on failure to ensure all resources allocated
 * up to the failure point are properly released.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_function_setup(struct fdice_dev *fdev)
{
	struct fdice_hw *hw = &fdev->hw;
	int err;

	/* Step 1: Detect MAC type and configure AQ parameters */
	err = fdice_hw_init_params(hw);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"hardware parameter init failed: %d\n", err);
		return err;
	}

	/* Step 2: Bring up admin control queues */
	err = fdice_ctlq_init(hw, &hw->adminq, FDICE_CTL_Q_ADMIN);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"admin queue init failed: %d\n", err);
		return err;
	}
	hw->aq_initialized = true;

	/*
	 * Step 2b: Clear all PF-owned firmware resources (opcode 0x0205).
	 * This MUST be called before any resource allocation. The E810
	 * firmware retains VSIs, queues, and switch elements across driver
	 * reloads. Without this, a second probe (after rmmod) will see
	 * stale state and refuse Add VSI with EACCES (AQ RC 10).
	 */
	err = fdice_hw_clear_pf_cfg(hw);
	if (err) {
		dev_warn(&fdev->pdev->dev,
			 "clear PF config failed: %d (continuing)\n", err);
		/*
		 * Non-fatal: first-ever load may have no resources to clear;
		 * firmware may return OK or a benign error in that case.
		 */
	}

	/* Step 3: Get firmware version */
	err = fdice_hw_get_fw_ver(hw, &hw->fw);
	if (err) {
		dev_warn(&fdev->pdev->dev,
			 "get FW version failed: %d, continuing\n", err);
		/*
		 * Non-fatal during early bring-up while AQ ring submission
		 * is still stubbed. In production this would be fatal.
		 */
	} else {
		dev_info(&fdev->pdev->dev,
			 "FW version %u.%u.%u build %u, API %u.%u.%u\n",
			 hw->fw.fw_major, hw->fw.fw_minor, hw->fw.fw_patch,
			 hw->fw.fw_build,
			 hw->fw.api_major, hw->fw.api_minor,
			 hw->fw.api_patch);
	}

	/* Step 4: Send driver version to firmware */
	err = fdice_hw_send_driver_ver(hw);
	if (err) {
		dev_warn(&fdev->pdev->dev,
			 "send driver version failed: %d, continuing\n", err);
		/* Non-fatal -- firmware logs it but doesn't gate on it */
	}

	/* Step 5: Discover function capabilities */
	err = fdice_discover_caps(hw, FDICE_AQ_OPC_LIST_FUNC_CAPS,
				  &hw->func_caps);
	if (err) {
		dev_warn(&fdev->pdev->dev,
			 "function capability discovery failed: %d\n", err);
		/* Non-fatal during stub phase */
	}

	/* Step 6: Discover device capabilities */
	err = fdice_discover_caps(hw, FDICE_AQ_OPC_LIST_DEV_CAPS,
				  &hw->dev_caps);
	if (err) {
		dev_warn(&fdev->pdev->dev,
			 "device capability discovery failed: %d\n", err);
		/* Non-fatal during stub phase */
	}

	/* Step 7: Read default MAC address from NVM */
	err = fdice_hw_read_mac_addr(hw);
	if (err) {
		dev_warn(&fdev->pdev->dev,
			 "MAC address read failed: %d\n", err);
		/* Non-fatal: random MAC fallback is always provided */
	}

	/* Step 8: Allocate port info */
	if (!hw->port_info) {
		hw->port_info = devm_kzalloc(&fdev->pdev->dev,
					     sizeof(*hw->port_info),
					     GFP_KERNEL);
		if (!hw->port_info) {
			err = -ENOMEM;
			goto err_shutdown_ctlq;
		}
	}

	hw->port_info->hw = hw;
	hw->port_info->lport = 0; /* default; updated by link status query */

	return 0;

err_shutdown_ctlq:
	fdice_ctlq_shutdown(hw, &hw->adminq);
	hw->aq_initialized = false;
	return err;
}

/**
 * fdice_function_teardown - Tear down hardware interface
 * @fdev: fdice device
 *
 * Reverse of fdice_function_setup(). Frees port info and shuts down
 * the admin control queues. Port info is managed by devres (devm_kzalloc)
 * so explicit free is not needed, but we clear the pointer for safety.
 */
void fdice_function_teardown(struct fdice_dev *fdev)
{
	struct fdice_hw *hw = &fdev->hw;

	/*
	 * Port info was allocated with devm_kzalloc -- it will be freed
	 * automatically when the PCI device is released. Clear the pointer
	 * to prevent dangling references during partial-teardown scenarios.
	 */
	hw->port_info = NULL;

	/* Shut down admin control queues */
	if (hw->aq_initialized) {
		fdice_ctlq_shutdown(hw, &hw->adminq);
		hw->aq_initialized = false;
	}

	/* Clear capability caches */
	memset(&hw->func_caps, 0, sizeof(hw->func_caps));
	memset(&hw->dev_caps, 0, sizeof(hw->dev_caps));
	memset(&hw->fw, 0, sizeof(hw->fw));
}

/* ---- MAC Address Read ---- */

/**
 * fdice_hw_read_mac_addr - Read default MAC address from NVM via AQ
 * @hw: hardware instance with AQ initialized
 *
 * Issues opcode 0x0107 (Manage MAC Read) to retrieve the factory-programmed
 * LAN MAC address from the E810 NVM. The indirect response buffer contains
 * one or more fdice_aqp_manage_mac_read_resp entries; we scan for the LAN
 * type entry. On success, stores the address in hw->mac_addr and sets
 * hw->mac_addr_valid.
 *
 * If the AQ command fails (e.g., during early bring-up), falls back to
 * generating a locally administered random MAC address.
 *
 * Return: 0 on success (always succeeds due to random fallback).
 */
int fdice_hw_read_mac_addr(struct fdice_hw *hw)
{
	struct fdice_aqp_manage_mac_read_resp resp[4];
	struct fdice_aq_desc desc;
	struct fdice_aqp_manage_mac_read *cmd;
	u16 resp_sz = sizeof(resp);
	u16 flags;
	int err;
	u8 i;

	memset(resp, 0, resp_sz);
	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_MANAGE_MAC_READ);
	cmd = (struct fdice_aqp_manage_mac_read *)desc.params.raw;

	err = fdice_aq_send_cmd(hw, &desc, resp, resp_sz);
	if (err)
		goto fallback;

	flags = le16_to_cpu(cmd->flags) & FDICE_AQ_MAC_READ_VALID_M;
	if (!(flags & FDICE_AQ_MAC_LAN_ADDR_VALID))
		goto fallback;

	/* Scan response entries for the LAN address */
	for (i = 0; i < cmd->num_addr && i < ARRAY_SIZE(resp); i++) {
		if (resp[i].addr_type == FDICE_AQ_MAC_ADDR_TYPE_LAN) {
			ether_addr_copy(hw->mac_addr, resp[i].mac_addr);
			if (is_valid_ether_addr(hw->mac_addr)) {
				hw->mac_addr_valid = true;
				dev_info(&hw->pdev->dev,
					 "MAC from NVM: %pM\n", hw->mac_addr);
				return 0;
			}
		}
	}

fallback:
	/*
	 * Fallback: generate a locally administered random MAC.
	 * eth_random_addr() sets the local-admin bit to avoid collisions
	 * with real hardware addresses on the network.
	 */
	eth_random_addr(hw->mac_addr);
	hw->mac_addr_valid = true;
	dev_info(&hw->pdev->dev,
		 "using random MAC %pM (NVM read unavailable: %d)\n",
		 hw->mac_addr, err);

	return 0;
}

/* ---- VSI Allocation / Free ---- */

/**
 * fdice_hw_alloc_vsi - Allocate a VSI via admin queue
 * @hw: hardware instance with AQ initialized
 * @vsi_type: VSI type (FDICE_AQ_VSI_TYPE_PF, _VF, _VMDQ2, _EMP_MNG)
 * @vsi_num: on success, returns the allocated VSI number
 *
 * Issues opcode 0x0210 (Add VSI) to firmware with an empty VSI properties
 * context. The firmware allocates a VSI of the requested type and returns
 * the assigned VSI number in the response descriptor.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_hw_alloc_vsi_aq(struct fdice_hw *hw, u16 vsi_type,
				 u16 *vsi_num)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_alloc_free_vsi *cmd;
	struct fdice_aqp_alloc_free_vsi_resp *resp;
	struct fdice_vsi_props *props;
	int err;

	if (!vsi_num)
		return -EINVAL;

	props = kzalloc(sizeof(*props), GFP_KERNEL);
	if (!props)
		return -ENOMEM;

	/*
	 * Populate the VSI properties indirect buffer with minimal
	 * configuration. The firmware requires at least the switch and
	 * VLAN sections to be marked valid. Without this buffer the
	 * Add VSI command returns EINVAL.
	 */
	props->valid_sections = cpu_to_le16(FDICE_AQ_VSI_PROP_SW_VALID |
					    FDICE_AQ_VSI_PROP_VLAN_VALID);

	/* Enable source pruning (default) */
	props->sw_flags = FDICE_AQ_VSI_SW_FLAG_SRC_PRUNE;

	/* Allow traffic from this VSI to reach the LAN */
	props->sw_flags2 = FDICE_AQ_VSI_SW_FLAG_LAN_ENA;

	/* Accept both tagged and untagged frames on TX */
	props->inner_vlan_flags = (FDICE_AQ_VSI_INNER_VLAN_TX_MODE_ALL <<
				   FDICE_AQ_VSI_INNER_VLAN_TX_MODE_S);

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_ADD_VSI);

	/*
	 * RD flag: we are sending data TO firmware (the props buffer).
	 * BUF flag: an indirect buffer is attached to this descriptor.
	 */
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD | FDICE_AQ_FLAG_BUF);

	cmd = (struct fdice_aqp_alloc_free_vsi *)desc.params.raw;
	cmd->vsi_flags = cpu_to_le16(vsi_type & 0x3);

	err = fdice_aq_send_cmd(hw, &desc, props, sizeof(*props));
	if (err) {
		dev_err(&hw->pdev->dev,
			"AQ add VSI (type %u) failed: %d\n", vsi_type, err);
		goto out_free;
	}

	resp = (struct fdice_aqp_alloc_free_vsi_resp *)desc.params.raw;
	*vsi_num = le16_to_cpu(resp->vsi_num) & FDICE_AQ_VSI_NUM_M;

	dev_dbg(&hw->pdev->dev,
		"allocated VSI %u (type %u, used=%u, free=%u)\n",
		*vsi_num, vsi_type,
		le16_to_cpu(resp->vsi_used),
		le16_to_cpu(resp->vsi_free));

out_free:
	kfree(props);
	return err;
}

/**
 * fdice_hw_alloc_vsi - Public wrapper for VSI allocation
 * @hw: hardware instance
 * @vsi_idx: output VSI index on success
 *
 * Allocates a default PF VSI. Wrapper around the AQ-level function.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_hw_alloc_vsi(struct fdice_hw *hw, u16 *vsi_idx)
{
	return fdice_hw_alloc_vsi_aq(hw, FDICE_AQ_VSI_TYPE_PF, vsi_idx);
}

/**
 * fdice_hw_free_vsi_aq - Free a VSI via admin queue
 * @hw: hardware instance
 * @vsi_num: VSI number to free
 *
 * Issues opcode 0x0213 (Free VSI) to release the specified VSI back
 * to firmware. The VSI must not have any active queues.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_hw_free_vsi_aq(struct fdice_hw *hw, u16 vsi_num)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_alloc_free_vsi *cmd;
	int err;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_FREE_VSI);

	cmd = (struct fdice_aqp_alloc_free_vsi *)desc.params.raw;
	cmd->vsi_num = cpu_to_le16(vsi_num | FDICE_AQ_VSI_IS_VALID);

	err = fdice_aq_send_cmd(hw, &desc, NULL, 0);
	if (err) {
		dev_err(&hw->pdev->dev,
			"AQ free VSI %u failed: %d\n", vsi_num, err);
		return err;
	}

	dev_dbg(&hw->pdev->dev, "freed VSI %u\n", vsi_num);
	return 0;
}

/**
 * fdice_hw_free_vsi - Public wrapper for VSI deallocation
 * @hw: hardware instance
 * @vsi_idx: VSI index to release
 */
void fdice_hw_free_vsi(struct fdice_hw *hw, u16 vsi_idx)
{
	fdice_hw_free_vsi_aq(hw, vsi_idx);
}

/* ---- TX Queue Management ---- */

/**
 * fdice_hw_add_txqs - Enable TX queues via AQ
 * @hw: hardware instance
 * @num_qgrps: number of queue groups
 * @qg_buf: buffer containing fdice_aq_add_tx_qgrp array
 * @buf_size: total size of @qg_buf in bytes
 *
 * Issues opcode 0x0C30 (Add TX LAN Queues) to enable TX queues with
 * the specified scheduling parameters. Each queue group contains a parent
 * TEID, queue context, and scheduler node configuration.
 *
 * The caller must pre-populate the fdice_aq_add_tx_qgrp entries with valid
 * parent TEIDs, queue contexts, and scheduler element info.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_hw_add_txqs_aq(struct fdice_hw *hw, u8 num_qgrps,
				void *qg_buf, u16 buf_size)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_add_txqs *cmd;
	int err;

	if (!qg_buf || !num_qgrps)
		return -EINVAL;

	if (num_qgrps > FDICE_LAN_TXQ_MAX_QGRPS)
		return -EINVAL;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_ADD_TXQS);
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD);

	cmd = (struct fdice_aqp_add_txqs *)desc.params.raw;
	cmd->num_qgrps = num_qgrps;

	err = fdice_aq_send_cmd(hw, &desc, qg_buf, buf_size);
	if (err)
		dev_err(&hw->pdev->dev,
			"AQ add TXQs (%u groups) failed: %d\n",
			num_qgrps, err);

	return err;
}

/**
 * fdice_hw_dis_txqs - Disable TX queues via AQ
 * @hw: hardware instance
 * @num_entries: number of queue group entries in buffer
 * @qg_buf: buffer containing fdice_aq_dis_txq_item array
 * @buf_size: total size of @qg_buf in bytes
 * @rst_type: reset type (FDICE_AQ_Q_DIS_CMD_* flags)
 * @vmvf_num: VM/VF number for reset context
 *
 * Issues opcode 0x0C31 (Disable TX LAN Queues) to stop the specified
 * TX queues. The firmware drains in-flight packets before completing.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_hw_dis_txqs_aq(struct fdice_hw *hw, u8 num_entries,
				void *qg_buf, u16 buf_size,
				u8 rst_type, u16 vmvf_num)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_dis_txqs *cmd;
	int err;

	if (!qg_buf || !num_entries)
		return -EINVAL;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_DIS_TXQS);
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD);

	cmd = (struct fdice_aqp_dis_txqs *)desc.params.raw;
	cmd->cmd_type = rst_type & FDICE_AQ_Q_DIS_CMD_M;
	cmd->num_entries = num_entries;
	cmd->vmvf_and_timeout = cpu_to_le16(
		(vmvf_num & FDICE_AQ_Q_DIS_VMVF_NUM_M) |
		((5 << FDICE_AQ_Q_DIS_TIMEOUT_S) & FDICE_AQ_Q_DIS_TIMEOUT_M));

	err = fdice_aq_send_cmd(hw, &desc, qg_buf, buf_size);
	if (err)
		dev_err(&hw->pdev->dev,
			"AQ disable TXQs (%u entries, rst=%u) failed: %d\n",
			num_entries, rst_type, err);

	return err;
}

/* ---- Set Port Parameters ---- */

/**
 * fdice_hw_set_port_params - Configure port-level switching parameters
 * @hw: hardware instance
 * @double_vlan: enable double VLAN tag handling
 *
 * Issues opcode 0x0203 (Set Port Parameters) to configure port-level
 * switching behavior. Currently supports double VLAN mode control.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_hw_set_port_params(struct fdice_hw *hw, bool double_vlan)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_set_port_params *cmd;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_SET_PORT_PARAMS);
	cmd = (struct fdice_aqp_set_port_params *)desc.params.raw;

	if (double_vlan)
		cmd->cmd_flags = cpu_to_le16(FDICE_AQ_SET_PORT_DOUBLE_VLAN_ENA);

	return fdice_aq_send_cmd(hw, &desc, NULL, 0);
}

/* ---- Set MAC Config ---- */

/**
 * fdice_hw_set_mac_cfg - Configure MAC frame handling
 * @hw: hardware instance
 * @max_frame_size: maximum frame size to accept
 *
 * Issues opcode 0x0603 (Set MAC Config) to configure the MAC-level
 * frame size limit. This must be called after link configuration to
 * ensure the MAC accepts frames up to the configured MTU.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_hw_set_mac_cfg(struct fdice_hw *hw, u16 max_frame_size)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_set_mac_cfg *cmd;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_SET_MAC_CFG);
	cmd = (struct fdice_aqp_set_mac_cfg *)desc.params.raw;

	cmd->max_frame_size = cpu_to_le16(max_frame_size);

	return fdice_aq_send_cmd(hw, &desc, NULL, 0);
}

/* ---- hw_ops vtable instance ---- */

/**
 * fdice_hw_ops - Default hardware operation vtable
 *
 * Phase 3.2 populates the core subsystem. PHY and other subsystems
 * are left NULL and will be populated by their respective phases.
 * Callers must check for NULL before invoking optional subsystem ops.
 */
const struct fdice_hw_ops fdice_hw_ops = {
	.core = {
		.get_fw_ver      = fdice_hw_get_fw_ver,
		.send_driver_ver = fdice_hw_send_driver_ver,
		.get_func_caps   = NULL, /* thin wrapper added when needed */
		.get_dev_caps    = NULL, /* thin wrapper added when needed */
		.read_mac_addr   = fdice_hw_read_mac_addr,
	},
	.vsi = {
		.alloc_vsi       = fdice_hw_alloc_vsi_aq,
		.free_vsi        = fdice_hw_free_vsi_aq,
	},
	.txq = {
		.add_txqs        = fdice_hw_add_txqs_aq,
		.dis_txqs        = fdice_hw_dis_txqs_aq,
	},
	.phy = {
		.get_link_status = NULL, /* Phase 3.7 */
		.set_event_mask  = NULL, /* Phase 3.7 */
	},
};
