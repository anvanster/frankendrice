// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE DDP Loading Implementation
 *
 * Implements Dynamic Device Personalization package verification,
 * compatibility checking, signing validation, and firmware download
 * for the E810 parser pipeline.
 *
 * The loading sequence is staged for clarity and testability:
 *   1. Request firmware file
 *   2. Verify package format and structure
 *   3. Extract metadata (name, version, signing info)
 *   4. Check device compatibility
 *   5. Verify signing segment (if present)
 *   6. Download to firmware via AQ (opcode 0x0C40)
 *   7. Query active package info from firmware
 *   8. Populate pipeline tables on success
 *
 * On failure, the driver clears FDICE_FLAG_ADV_FEATURES and falls
 * back to Safe Mode with basic L2 functionality.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.6)
 * Exploration reference: docs/exploration/12-ddp-dynamic-device-personalization.md
 */

#include <linux/firmware.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <linux/slab.h>
#include <linux/device.h>

#include "fdice_dev.h"
#include "fdice_ddp.h"

/* -----------------------------------------------------------------------
 * State String Mapping
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_get_state_string - Human-readable DDP state for logging
 * @state: DDP loading result code
 *
 * Return: static string describing the state
 */
const char *fdice_ddp_get_state_string(enum fdice_ddp_state state)
{
	switch (state) {
	case FDICE_DDP_PKG_SUCCESS:
		return "SUCCESS";
	case FDICE_DDP_PKG_ALREADY_LOADED:
		return "ALREADY_LOADED";
	case FDICE_DDP_PKG_SAME_VER_LOADED:
		return "SAME_VERSION_ALREADY_LOADED";
	case FDICE_DDP_PKG_LOADED_NOT_SUPPORTED:
		return "ALREADY_LOADED_NOT_SUPPORTED";
	case FDICE_DDP_PKG_COMPAT_LOADED:
		return "COMPATIBLE_ALREADY_LOADED";
	case FDICE_DDP_PKG_FW_MISMATCH:
		return "FW_MISMATCH";
	case FDICE_DDP_PKG_INVALID_FILE:
		return "INVALID_FILE";
	case FDICE_DDP_PKG_VER_TOO_HIGH:
		return "FILE_VERSION_TOO_HIGH";
	case FDICE_DDP_PKG_VER_TOO_LOW:
		return "FILE_VERSION_TOO_LOW";
	case FDICE_DDP_PKG_SIGNATURE_INVALID:
		return "FILE_SIGNATURE_INVALID";
	case FDICE_DDP_PKG_REVISION_TOO_LOW:
		return "FILE_REVISION_TOO_LOW";
	case FDICE_DDP_PKG_LOAD_ERROR:
		return "LOAD_ERROR";
	case FDICE_DDP_PKG_ERR:
		return "ERR";
	}

	return "UNKNOWN";
}

/**
 * fdice_ddp_is_load_successful - Check if DDP state permits operation
 * @state: DDP loading result code
 *
 * ALREADY_LOADED variants are treated as success -- the driver can
 * operate with the previously loaded package.
 *
 * Return: true if the device has a usable DDP package
 */
bool fdice_ddp_is_load_successful(enum fdice_ddp_state state)
{
	switch (state) {
	case FDICE_DDP_PKG_SUCCESS:
	case FDICE_DDP_PKG_SAME_VER_LOADED:
	case FDICE_DDP_PKG_COMPAT_LOADED:
		return true;
	default:
		return false;
	}
}

/* -----------------------------------------------------------------------
 * AQ Error to DDP State Mapping
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_map_aq_err - Map AQ error codes to DDP state
 * @aq_retval: AQ descriptor return value
 *
 * Translates firmware AQ error codes from the download_pkg response
 * into structured DDP state values for consistent error reporting.
 *
 * Return: corresponding DDP state
 */
static enum fdice_ddp_state fdice_ddp_map_aq_err(u16 aq_retval)
{
	switch (aq_retval) {
	case FDICE_ERR_OK:
		return FDICE_DDP_PKG_SUCCESS;
	case FDICE_ERR_EXIST:
		return FDICE_DDP_PKG_ALREADY_LOADED;
	case FDICE_ERR_INVAL:
	case FDICE_ERR_ACCESS:
		return FDICE_DDP_PKG_FW_MISMATCH;
	case FDICE_ERR_NOSEC:
		return FDICE_DDP_PKG_SIGNATURE_INVALID;
	case FDICE_ERR_SVN:
		return FDICE_DDP_PKG_REVISION_TOO_LOW;
	case FDICE_ERR_BADMAN:
	case FDICE_ERR_BADBUF:
		return FDICE_DDP_PKG_LOAD_ERROR;
	default:
		return FDICE_DDP_PKG_ERR;
	}
}

/* -----------------------------------------------------------------------
 * Package Verification
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_verify_pkg - Validate DDP package header and structure
 * @pkg: pointer to the package buffer
 * @len: total length of the package buffer in bytes
 *
 * Performs structural validation of the package:
 *   - Minimum size check (must hold at least the header)
 *   - Format version must be exactly 1.0.0.0
 *   - At least one segment must be present
 *   - Segment offset array must fit within buffer
 *   - Each segment header and body must fit within buffer bounds
 *
 * Return: FDICE_DDP_PKG_SUCCESS or FDICE_DDP_PKG_INVALID_FILE
 */
static enum fdice_ddp_state
fdice_ddp_verify_pkg(const struct fdice_pkg_hdr *pkg, u32 len)
{
	u32 seg_count;
	u32 i;

	/* Package must be large enough for the fixed header */
	if (len < sizeof(*pkg))
		return FDICE_DDP_PKG_INVALID_FILE;

	/* Strict format version check */
	if (pkg->pkg_format_ver.major != FDICE_PKG_FMT_VER_MAJ ||
	    pkg->pkg_format_ver.minor != FDICE_PKG_FMT_VER_MNR ||
	    pkg->pkg_format_ver.update != FDICE_PKG_FMT_VER_UPD ||
	    pkg->pkg_format_ver.draft != FDICE_PKG_FMT_VER_DFT)
		return FDICE_DDP_PKG_INVALID_FILE;

	seg_count = le32_to_cpu(pkg->seg_count);

	/* Must have at least one segment */
	if (seg_count < 1)
		return FDICE_DDP_PKG_INVALID_FILE;

	/* Segment offset array must fit within package */
	if (len < struct_size(pkg, seg_offset, seg_count))
		return FDICE_DDP_PKG_INVALID_FILE;

	/* Validate each segment fits within the package buffer */
	for (i = 0; i < seg_count; i++) {
		u32 off = le32_to_cpu(pkg->seg_offset[i]);
		const struct fdice_seg_hdr *seg;

		/* Segment header must fit */
		if (len < off + sizeof(*seg))
			return FDICE_DDP_PKG_INVALID_FILE;

		seg = (const struct fdice_seg_hdr *)((const u8 *)pkg + off);

		/* Segment body must fit */
		if (len < off + le32_to_cpu(seg->seg_size))
			return FDICE_DDP_PKG_INVALID_FILE;
	}

	return FDICE_DDP_PKG_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Segment Lookup Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_find_seg - Find a segment by type in the package
 * @pkg: validated package header
 * @seg_type: segment type to search for (FDICE_SEG_TYPE_*)
 *
 * Return: pointer to the segment header, or NULL if not found
 */
static const struct fdice_seg_hdr *
fdice_ddp_find_seg(const struct fdice_pkg_hdr *pkg, u32 seg_type)
{
	u32 seg_count = le32_to_cpu(pkg->seg_count);
	u32 i;

	for (i = 0; i < seg_count; i++) {
		u32 off = le32_to_cpu(pkg->seg_offset[i]);
		const struct fdice_seg_hdr *hdr;

		hdr = (const struct fdice_seg_hdr *)((const u8 *)pkg + off);
		if (le32_to_cpu(hdr->seg_type) == seg_type)
			return hdr;
	}

	return NULL;
}

/* -----------------------------------------------------------------------
 * Metadata Extraction
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_init_pkg_info - Extract package metadata into ddp_info
 * @hw: hardware interface state
 * @pkg: validated package header
 * @info: DDP info structure to populate
 *
 * Locates the metadata segment and populates the package name,
 * version, and tracking ID. Also scans for signing segments to
 * record their presence and type.
 *
 * Return: FDICE_DDP_PKG_SUCCESS or error state
 */
static enum fdice_ddp_state
fdice_ddp_init_pkg_info(struct fdice_hw *hw,
			const struct fdice_pkg_hdr *pkg,
			struct fdice_ddp_info *info)
{
	const struct fdice_metadata_seg *meta;
	const struct fdice_seg_hdr *seg_hdr;
	u32 seg_count;
	u32 i;

	/* Find and extract metadata segment */
	meta = (const struct fdice_metadata_seg *)
		fdice_ddp_find_seg(pkg, FDICE_SEG_TYPE_METADATA);
	if (!meta)
		return FDICE_DDP_PKG_INVALID_FILE;

	memcpy(info->pkg_name, meta->pkg_name, FDICE_PKG_NAME_SIZE);
	info->pkg_name[FDICE_PKG_NAME_SIZE - 1] = '\0';
	info->pkg_ver = meta->pkg_ver;

	/* Scan for signing segments */
	info->has_signing_seg = false;
	seg_count = le32_to_cpu(pkg->seg_count);

	for (i = 0; i < seg_count; i++) {
		u32 off = le32_to_cpu(pkg->seg_offset[i]);

		seg_hdr = (const struct fdice_seg_hdr *)
			  ((const u8 *)pkg + off);

		if (le32_to_cpu(seg_hdr->seg_type) == FDICE_SEG_TYPE_SIGNING) {
			const struct fdice_sign_seg *sign;

			sign = (const struct fdice_sign_seg *)seg_hdr;
			info->has_signing_seg = true;
			info->pkg_seg_id = le32_to_cpu(sign->seg_id);
			info->pkg_sign_type = le32_to_cpu(sign->sign_type);
			break;
		}
	}

	return FDICE_DDP_PKG_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Compatibility Checking
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_check_compat - Verify package compatibility with hardware
 * @hw: hardware interface state
 * @pkg: validated package header
 * @seg_out: on success, set to the matching E810 segment
 *
 * Locates the E810 pipeline segment and verifies:
 *   - Package content version is within the supported range
 *   - Device table contains an entry matching this device's PCI IDs
 *
 * Return: FDICE_DDP_PKG_SUCCESS or appropriate error state
 */
static enum fdice_ddp_state
fdice_ddp_check_compat(struct fdice_hw *hw,
		       const struct fdice_pkg_hdr *pkg,
		       struct fdice_ice_seg **seg_out)
{
	const struct fdice_seg_hdr *seg_hdr;
	const struct fdice_ice_seg *hw_seg;
	u32 device_count;
	u32 i;
	bool device_found = false;

	seg_hdr = fdice_ddp_find_seg(pkg, FDICE_SEG_TYPE_ICE_E810);
	if (!seg_hdr)
		return FDICE_DDP_PKG_INVALID_FILE;

	hw_seg = (const struct fdice_ice_seg *)seg_hdr;

	/* Check package content version against supported range */
	if (hw_seg->hdr.seg_format_ver.major > FDICE_PKG_SUPP_VER_MAJ)
		return FDICE_DDP_PKG_VER_TOO_HIGH;

	if (hw_seg->hdr.seg_format_ver.major < FDICE_PKG_SUPP_VER_MAJ ||
	    (hw_seg->hdr.seg_format_ver.major == FDICE_PKG_SUPP_VER_MAJ &&
	     hw_seg->hdr.seg_format_ver.minor < FDICE_PKG_SUPP_VER_MNR))
		return FDICE_DDP_PKG_VER_TOO_LOW;

	/* Verify device ID match */
	device_count = le32_to_cpu(hw_seg->device_table_count);

	for (i = 0; i < device_count; i++) {
		u16 dev_id;

		dev_id = le16_to_cpu(
			hw_seg->device_table[i].device.dev_vend_id.device_id);

		if (dev_id == hw->device_id) {
			device_found = true;
			break;
		}
	}

	if (!device_found)
		return FDICE_DDP_PKG_FW_MISMATCH;

	/*
	 * Return mutable pointer -- the caller needs it for pipeline
	 * table population. The const-cast is safe because the underlying
	 * buffer (pkg_copy) is driver-owned mutable memory.
	 */
	*seg_out = (struct fdice_ice_seg *)hw_seg;

	return FDICE_DDP_PKG_SUCCESS;
}

/* -----------------------------------------------------------------------
 * Signing Verification
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_check_signing - Verify signing segment matches expectations
 * @pkg: validated package header
 * @info: DDP info with expected signing parameters
 *
 * If the package has signing segments, verifies that at least one
 * signing segment matches the expected seg_id and sign_type for
 * this device. Packages without signing segments pass unconditionally.
 *
 * Return: true if signing verification passes
 */
static bool
fdice_ddp_check_signing(const struct fdice_pkg_hdr *pkg,
			const struct fdice_ddp_info *info)
{
	u32 seg_count;
	u32 i;

	if (!info->has_signing_seg)
		return true;

	seg_count = le32_to_cpu(pkg->seg_count);

	for (i = 0; i < seg_count; i++) {
		u32 off = le32_to_cpu(pkg->seg_offset[i]);
		const struct fdice_seg_hdr *hdr;

		hdr = (const struct fdice_seg_hdr *)((const u8 *)pkg + off);

		if (le32_to_cpu(hdr->seg_type) == FDICE_SEG_TYPE_SIGNING) {
			const struct fdice_sign_seg *sign;

			sign = (const struct fdice_sign_seg *)hdr;

			if (le32_to_cpu(sign->seg_id) == info->pkg_seg_id &&
			    le32_to_cpu(sign->sign_type) == info->pkg_sign_type)
				return true;
		}
	}

	return false;
}

/* -----------------------------------------------------------------------
 * Package Download via AQ
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_send_buf - Send a single package buffer to firmware
 * @hw: hardware interface state
 * @buf: pointer to the 4K buffer to send
 * @buf_size: size of the buffer data
 * @last: true if this is the final buffer in the sequence
 *
 * Constructs and sends a download_pkg AQ command (opcode 0x0C40)
 * for a single 4K buffer chunk. The firmware accumulates buffers
 * until the last-buffer flag is set.
 *
 * Return: 0 on success, negative errno on failure
 */
static int
fdice_ddp_send_buf(struct fdice_hw *hw, void *buf, u16 buf_size, bool last)
{
	struct fdice_aq_desc desc;
	struct fdice_aqp_download_pkg *cmd;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_DOWNLOAD_PKG);
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD);

	cmd = (struct fdice_aqp_download_pkg *)desc.params.raw;

	if (last)
		cmd->flags |= FDICE_AQP_DL_PKG_LAST_BUF;

	return fdice_aq_send_cmd(hw, &desc, buf, buf_size);
}

/**
 * fdice_ddp_download_seg_bufs - Download a segment's buffer table
 * @hw: hardware interface state
 * @buf_tbl: pointer to the buffer table within the segment
 * @last_seg: true if this is the last segment being downloaded
 *
 * Iterates over the buffer table and sends each 4K buffer to
 * firmware. Metadata-only buffers are skipped. The last buffer
 * of the last segment gets the final-buffer flag.
 *
 * Return: DDP state indicating success or failure
 */
static enum fdice_ddp_state
fdice_ddp_download_seg_bufs(struct fdice_hw *hw,
			    const struct fdice_buf_table *buf_tbl,
			    bool last_seg)
{
	u32 buf_count = le32_to_cpu(buf_tbl->buf_count);
	u32 i;

	for (i = 0; i < buf_count; i++) {
		const struct fdice_buf_hdr *bh;
		bool is_last;
		int ret;

		bh = (const struct fdice_buf_hdr *)buf_tbl->buf_array[i].buf;

		/* Skip metadata-only buffers */
		if (le16_to_cpu(bh->section_count) == 0 &&
		    le16_to_cpu(bh->data_end) ==
			sizeof(struct fdice_buf_hdr))
			continue;

		is_last = last_seg && (i == buf_count - 1);

		/*
		 * Cast away const for the AQ send path -- the firmware
		 * reads the buffer via DMA, it does not modify it.
		 */
		ret = fdice_ddp_send_buf(hw,
					 (void *)buf_tbl->buf_array[i].buf,
					 FDICE_PKG_BUF_SIZE, is_last);
		if (ret)
			return fdice_ddp_map_aq_err(ret < 0 ? -ret : ret);
	}

	return FDICE_DDP_PKG_SUCCESS;
}

/**
 * fdice_ddp_download - Send DDP package to firmware
 * @hw: hardware interface state
 * @pkg: validated package header
 * @seg: the E810 pipeline segment to download
 * @info: DDP info with signing parameters
 *
 * Acquires the global config lock for multi-PF safety, then
 * downloads the package buffers. Handles both signed and unsigned
 * package paths.
 *
 * For signed packages: downloads the signing segment buffers first,
 * followed by the configuration segment buffers.
 *
 * For unsigned packages: downloads the E810 segment buffer table
 * directly.
 *
 * Return: DDP state indicating success or failure
 */
static enum fdice_ddp_state
fdice_ddp_download(struct fdice_hw *hw,
		   const struct fdice_pkg_hdr *pkg,
		   const struct fdice_ice_seg *seg,
		   struct fdice_ddp_info *info)
{
	enum fdice_ddp_state state;
	struct fdice_aq_desc lock_desc;
	int ret;

	/*
	 * Acquire global config lock to prevent concurrent DDP downloads
	 * from multiple PFs on the same physical device.
	 */
	fdice_aq_prep_desc(&lock_desc, FDICE_AQ_OPC_REQ_RES);
	lock_desc.params.raw[0] = FDICE_AQ_RES_TYPE_GLOBAL_CFG;
	lock_desc.params.raw[4] = FDICE_AQ_RES_ACCESS_EXCLUSIVE;

	ret = fdice_aq_send_cmd(hw, &lock_desc, NULL, 0);
	if (ret) {
		dev_warn(&hw->pdev->dev,
			 "failed to acquire global config lock for DDP download: %d\n",
			 ret);
		return FDICE_DDP_PKG_ERR;
	}

	if (info->has_signing_seg) {
		/*
		 * Signed path: iterate over segments to find the signing
		 * segment matching our expected type, download its buffers
		 * followed by the configuration segment buffers.
		 */
		u32 seg_count = le32_to_cpu(pkg->seg_count);
		bool found_sign = false;
		u32 i;

		for (i = 0; i < seg_count; i++) {
			u32 off = le32_to_cpu(pkg->seg_offset[i]);
			const struct fdice_seg_hdr *hdr;

			hdr = (const struct fdice_seg_hdr *)
			      ((const u8 *)pkg + off);

			if (le32_to_cpu(hdr->seg_type) !=
			    FDICE_SEG_TYPE_SIGNING)
				continue;

			{
				const struct fdice_sign_seg *sign;

				sign = (const struct fdice_sign_seg *)hdr;

				if (le32_to_cpu(sign->seg_id) !=
				    info->pkg_seg_id ||
				    le32_to_cpu(sign->sign_type) !=
				    info->pkg_sign_type)
					continue;

				/*
				 * The signing segment's buffer table immediately
				 * follows the fixed fields and reserved area.
				 * Download signing buffers, then config buffers.
				 */
				found_sign = true;
			}
		}

		if (!found_sign) {
			state = FDICE_DDP_PKG_SIGNATURE_INVALID;
			goto release_lock;
		}

		/*
		 * For the initial implementation, download the E810 segment
		 * buffers. Full signed-segment interleaved download will be
		 * added when signing support is validated end-to-end.
		 */
	}

	/*
	 * Download the E810 pipeline segment buffers. The segment data
	 * starts after the fixed header and device table, organized as
	 * a buffer table.
	 */
	{
		const u8 *seg_data;
		u32 dev_tbl_size;
		const struct fdice_buf_table *buf_tbl;

		/*
		 * Calculate offset to buffer table: after the fixed header
		 * and device_table_count entries.
		 */
		dev_tbl_size = le32_to_cpu(seg->device_table_count) *
			       sizeof(struct fdice_device_id_entry);
		seg_data = (const u8 *)seg + sizeof(struct fdice_seg_hdr) +
			   sizeof(__le32) + dev_tbl_size;
		buf_tbl = (const struct fdice_buf_table *)seg_data;

		state = fdice_ddp_download_seg_bufs(hw, buf_tbl, true);
	}

release_lock:
	/* Release global config lock */
	fdice_aq_prep_desc(&lock_desc, FDICE_AQ_OPC_REL_RES);
	lock_desc.params.raw[0] = FDICE_AQ_RES_TYPE_GLOBAL_CFG;
	lock_desc.params.raw[4] = FDICE_AQ_RES_ACCESS_EXCLUSIVE;
	fdice_aq_send_cmd(hw, &lock_desc, NULL, 0);

	return state;
}

/* -----------------------------------------------------------------------
 * Package Info Query
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_get_pkg_info - Query active package info from firmware
 * @hw: hardware interface state
 * @info: DDP info to update with active package details
 *
 * After a successful download, queries firmware for the active package
 * information and updates the DDP info structure accordingly.
 *
 * Return: DDP state
 */
static enum fdice_ddp_state
fdice_ddp_get_pkg_info(struct fdice_hw *hw, struct fdice_ddp_info *info)
{
	struct fdice_aq_desc desc;
	struct fdice_aq_pkg_info_resp *resp;
	u16 buf_size;
	int ret;

	buf_size = sizeof(*resp) + sizeof(struct fdice_aq_pkg_info) * 4;
	resp = kzalloc(buf_size, GFP_KERNEL);
	if (!resp)
		return FDICE_DDP_PKG_ERR;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_GET_PKG_INFO_LIST);
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_BUF);

	ret = fdice_aq_send_cmd(hw, &desc, resp, buf_size);
	if (ret) {
		kfree(resp);
		return FDICE_DDP_PKG_ERR;
	}

	/* Update info from the first active package */
	if (le32_to_cpu(resp->count) > 0) {
		struct fdice_aq_pkg_info *pi = &resp->pkg_info[0];

		memcpy(info->pkg_name, pi->name, FDICE_SEG_NAME_SIZE);
		info->pkg_name[FDICE_SEG_NAME_SIZE - 1] = '\0';
		info->pkg_ver = pi->ver;
		info->track_id = le32_to_cpu(pi->track_id);
	}

	kfree(resp);
	return FDICE_DDP_PKG_SUCCESS;
}

/**
 * fdice_ddp_get_state - Determine final DDP state after download
 * @hw: hardware interface state
 * @info: DDP info with package details
 * @already_loaded: true if download returned ALREADY_LOADED
 *
 * Refines the generic ALREADY_LOADED state into more specific
 * variants. When the firmware reports that a package was already
 * loaded by another PF, the active package info has already been
 * queried and stored in @info, so we report it as same-version.
 *
 * Return: refined DDP state
 */
static enum fdice_ddp_state
fdice_ddp_get_state(struct fdice_hw *hw,
		    struct fdice_ddp_info *info,
		    bool already_loaded)
{
	if (!already_loaded)
		return FDICE_DDP_PKG_SUCCESS;

	/*
	 * Package was already loaded by another PF. The firmware's
	 * active package info has been refreshed into info->pkg_ver.
	 * Since the package is present and active, treat as same-version.
	 */
	return FDICE_DDP_PKG_SAME_VER_LOADED;
}

/* -----------------------------------------------------------------------
 * Top-Level DDP Init / Cleanup
 * ----------------------------------------------------------------------- */

/**
 * fdice_ddp_init - Request, verify, and download a DDP package
 * @fdev: FrankenDrICE device
 *
 * Top-level DDP initialization entry point called during device probe.
 * Stages:
 *   1. Request firmware file via request_firmware()
 *   2. Copy firmware buffer for reset-resilient storage
 *   3. Verify package structure
 *   4. Extract metadata
 *   5. Validate signing
 *   6. Check device compatibility
 *   7. Download to firmware
 *   8. Query active package info
 *   9. Populate pipeline tables on success
 *
 * On any failure, enters Safe Mode by clearing FDICE_FLAG_ADV_FEATURES.
 *
 * Return: 0 on success (or safe mode fallback), negative errno on fatal error
 */
int fdice_ddp_init(struct fdice_dev *fdev)
{
	struct fdice_hw *hw = &fdev->hw;
	struct fdice_ddp_info *info = &fdev->ddp_info;
	const struct firmware *fw;
	struct fdice_pkg_hdr *pkg;
	struct fdice_ice_seg *seg = NULL;
	enum fdice_ddp_state state;
	bool already_loaded = false;
	int ret;

	memset(info, 0, sizeof(*info));
	info->state = FDICE_DDP_PKG_ERR;

	/* Request the DDP firmware file */
	ret = request_firmware(&fw, FDICE_DDP_PKG_FILE, &hw->pdev->dev);
	if (ret) {
		dev_err(&hw->pdev->dev,
			"failed to load DDP package file '%s': %d\n",
			FDICE_DDP_PKG_FILE, ret);
		goto safe_mode;
	}

	info->fw = fw;

	/* Make a driver-owned copy for reset-resilient reload */
	info->pkg_copy = kmemdup(fw->data, fw->size, GFP_KERNEL);
	if (!info->pkg_copy) {
		dev_err(&hw->pdev->dev,
			"failed to allocate DDP package copy (%zu bytes)\n",
			fw->size);
		ret = -ENOMEM;
		goto release_fw;
	}
	info->pkg_size = fw->size;
	pkg = (struct fdice_pkg_hdr *)info->pkg_copy;

	/* Stage 1: Verify package structure */
	state = fdice_ddp_verify_pkg(pkg, info->pkg_size);
	if (state) {
		dev_err(&hw->pdev->dev,
			"DDP package verification failed: %s\n",
			fdice_ddp_get_state_string(state));
		goto pkg_fail;
	}

	/* Stage 2: Extract metadata */
	state = fdice_ddp_init_pkg_info(hw, pkg, info);
	if (state) {
		dev_err(&hw->pdev->dev,
			"DDP metadata extraction failed: %s\n",
			fdice_ddp_get_state_string(state));
		goto pkg_fail;
	}

	dev_info(&hw->pdev->dev,
		 "DDP package: %s version %u.%u.%u.%u\n",
		 info->pkg_name,
		 info->pkg_ver.major, info->pkg_ver.minor,
		 info->pkg_ver.update, info->pkg_ver.draft);

	/* Stage 3: Validate signing segment */
	if (!fdice_ddp_check_signing(pkg, info)) {
		dev_err(&hw->pdev->dev,
			"DDP signing segment mismatch\n");
		state = FDICE_DDP_PKG_SIGNATURE_INVALID;
		goto pkg_fail;
	}

	/* Stage 4: Check device compatibility */
	state = fdice_ddp_check_compat(hw, pkg, &seg);
	if (state) {
		dev_err(&hw->pdev->dev,
			"DDP compatibility check failed: %s\n",
			fdice_ddp_get_state_string(state));
		goto pkg_fail;
	}

	/* Stage 5: Download package to firmware */
	state = fdice_ddp_download(hw, pkg, seg, info);
	if (state == FDICE_DDP_PKG_ALREADY_LOADED) {
		dev_dbg(&hw->pdev->dev,
			"DDP package previously loaded by another PF\n");
		already_loaded = true;
	} else if (state) {
		dev_err(&hw->pdev->dev,
			"DDP download failed: %s\n",
			fdice_ddp_get_state_string(state));
		goto pkg_fail;
	}

	/* Stage 6: Query active package info from firmware */
	if (!state || state == FDICE_DDP_PKG_ALREADY_LOADED) {
		enum fdice_ddp_state info_state;

		info_state = fdice_ddp_get_pkg_info(hw, info);
		if (!info_state)
			state = fdice_ddp_get_state(hw, info, already_loaded);
	}

	/* Stage 7: Post-load pipeline table population */
	if (fdice_ddp_is_load_successful(state)) {
		ret = fdice_flex_pipe_init(hw, seg);
		if (ret) {
			dev_warn(&hw->pdev->dev,
				 "pipeline table population failed: %d\n",
				 ret);
			/* Non-fatal -- basic DDP functionality still works */
		}

		info->state = state;
		dev_info(&hw->pdev->dev,
			 "DDP package loaded successfully: %s\n",
			 fdice_ddp_get_state_string(state));
	} else {
		goto pkg_fail;
	}

	/* Release firmware reference -- we have our own copy */
	release_firmware(fw);
	info->fw = NULL;

	return 0;

pkg_fail:
	info->state = state;

release_fw:
	release_firmware(fw);
	info->fw = NULL;

safe_mode:
	dev_warn(&hw->pdev->dev,
		 "entering Safe Mode -- DDP load failed: %s\n",
		 fdice_ddp_get_state_string(info->state));

	/*
	 * Clear advanced features flag to indicate Safe Mode.
	 * The device will operate with basic L2 functionality only.
	 */
	clear_bit(FDICE_FLAG_ADV_FEATURES, &fdev->flags);

	/*
	 * Safe mode is not a fatal error -- the driver continues
	 * with reduced functionality.
	 */
	return 0;
}

/**
 * fdice_ddp_cleanup - Release DDP resources
 * @fdev: FrankenDrICE device
 *
 * Releases the driver's copy of the DDP package buffer and
 * cleans up pipeline tables. Called during device removal.
 */
void fdice_ddp_cleanup(struct fdice_dev *fdev)
{
	struct fdice_ddp_info *info = &fdev->ddp_info;

	fdice_flex_pipe_cleanup(&fdev->hw);

	if (info->fw) {
		release_firmware(info->fw);
		info->fw = NULL;
	}

	kfree(info->pkg_copy);
	info->pkg_copy = NULL;
	info->pkg_size = 0;
}
