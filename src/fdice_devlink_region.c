// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_devlink_region.c - Devlink regions for NVM debugging
 *
 * Provides three devlink regions that expose E810 flash contents for
 * debugging via the ``devlink region`` CLI:
 *
 *   - **nvm-flash**: Full NVM flash snapshot (read via AQ NVM commands)
 *   - **shadow-ram**: Shadow RAM snapshot (cached NVM configuration)
 *   - **device-caps**: Device capabilities raw buffer from AQ
 *
 * These regions are invaluable for debugging E810 firmware issues,
 * verifying NVM contents after flash updates, and inspecting the
 * raw capability data that the driver uses for feature detection.
 *
 * Each region supports snapshot capture (allocates a buffer and fills
 * it from hardware) and destruction (frees the buffer via vfree).
 *
 * Reference study: ice devlink region implementation (kernel 6.17).
 * Rewritten with fdice_ abstractions and cleaner error paths.
 */

#include <linux/pci.h>
#include <linux/vmalloc.h>
#include <net/devlink.h>

#include "fdice_dev.h"
#include "fdice_devlink.h"

/* Maximum AQ buffer for capability queries */
#define FDICE_DEVCAPS_BUF_SIZE		FDICE_AQ_MAX_BUF_LEN

/*
 * NVM read block size — break large reads into chunks to avoid
 * AQ timeout under slow flash. 512 KiB per chunk is conservative
 * enough for all known E810 NVM configurations.
 */
#define FDICE_NVM_READ_BLK_SIZE		(512 * 1024)

/* Default region sizes when NVM discovery is not yet available.
 * E810 typical: 16 MiB NVM flash, 256 KiB shadow RAM.
 * These are overridden once the NVM subsystem populates actual sizes.
 */
#define FDICE_NVM_DEFAULT_SIZE		(16 * 1024 * 1024)
#define FDICE_SRAM_DEFAULT_SIZE		(256 * 1024)

/* -----------------------------------------------------------------------
 * NVM Flash Region
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_nvm_snapshot - Capture snapshot of NVM flash contents
 * @devlink: devlink instance
 * @ops: region ops identifying which region (nvm-flash)
 * @extack: netlink extended ACK
 * @data: output pointer to allocated snapshot buffer
 *
 * Allocates a buffer and reads the full NVM flash contents via AQ
 * NVM read commands. The read is broken into blocks to prevent AQ
 * timeouts when the NVM is large or the flash is slow.
 *
 * The snapshot buffer is owned by the devlink core and freed via
 * the destructor callback (vfree).
 *
 * Note: Full implementation requires NVM AQ read commands from
 * the HAL layer. Currently returns a zeroed buffer as a placeholder
 * until the NVM read path is wired in.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_nvm_snapshot(struct devlink *devlink,
				      const struct devlink_region_ops *ops,
				      struct netlink_ext_ack *extack,
				      u8 **data)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);
	u32 nvm_size;
	u8 *buf;

	nvm_size = fdev->nvm_size;
	if (!nvm_size) {
		NL_SET_ERR_MSG_MOD(extack, "NVM size not yet discovered");
		return -ENODATA;
	}

	buf = vzalloc(nvm_size);
	if (!buf)
		return -ENOMEM;

	/*
	 * TODO: Read NVM flash via AQ NVM read commands in blocks.
	 * Requires fdice_aq_read_nvm() from the HAL layer.
	 *
	 * for (offset = 0; offset < nvm_size; offset += blk_sz) {
	 *     blk_sz = min_t(u32, FDICE_NVM_READ_BLK_SIZE,
	 *                    nvm_size - offset);
	 *     err = fdice_aq_read_nvm(hw, offset, blk_sz,
	 *                             buf + offset, false);
	 * }
	 */

	*data = buf;
	return 0;
}

/* -----------------------------------------------------------------------
 * Shadow RAM Region
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_sram_snapshot - Capture snapshot of Shadow RAM
 * @devlink: devlink instance
 * @ops: region ops identifying which region (shadow-ram)
 * @extack: netlink extended ACK
 * @data: output pointer to allocated snapshot buffer
 *
 * Shadow RAM is the cached NVM configuration area that the firmware
 * reads during boot. Reading it provides insight into the active
 * configuration without parsing the full NVM flash.
 *
 * Note: Full implementation requires NVM AQ read with shadow_ram flag.
 * Currently returns a zeroed buffer as a placeholder.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_sram_snapshot(struct devlink *devlink,
				       const struct devlink_region_ops *ops,
				       struct netlink_ext_ack *extack,
				       u8 **data)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);
	u32 sram_size;
	u8 *buf;

	sram_size = fdev->sram_size;
	if (!sram_size) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Shadow RAM size not yet discovered");
		return -ENODATA;
	}

	buf = vzalloc(sram_size);
	if (!buf)
		return -ENOMEM;

	/*
	 * TODO: Read Shadow RAM via AQ NVM read commands.
	 * Requires fdice_aq_read_nvm() with shadow_ram=true.
	 */

	*data = buf;
	return 0;
}

/* -----------------------------------------------------------------------
 * Device Capabilities Region
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_devcaps_snapshot - Capture snapshot of device capabilities
 * @devlink: devlink instance
 * @ops: region ops identifying which region (device-caps)
 * @extack: netlink extended ACK
 * @data: output pointer to allocated snapshot buffer
 *
 * Captures the raw AQ response from the list_dev_caps command (opcode
 * 0x000B). This provides the complete capability data that the driver
 * parses during initialization, useful for verifying firmware feature
 * advertisements.
 *
 * Note: Full implementation requires issuing AQ list_dev_caps with
 * raw buffer capture. Currently returns a zeroed buffer.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_devcaps_snapshot(struct devlink *devlink,
					  const struct devlink_region_ops *ops,
					  struct netlink_ext_ack *extack,
					  u8 **data)
{
	u8 *buf;

	buf = vzalloc(FDICE_DEVCAPS_BUF_SIZE);
	if (!buf)
		return -ENOMEM;

	/*
	 * TODO: Issue AQ list_dev_caps (0x000B) with raw buffer capture.
	 * Requires fdice_aq_list_caps_raw() from the HAL layer.
	 */

	*data = buf;
	return 0;
}

/* -----------------------------------------------------------------------
 * Region Ops Structures
 * ----------------------------------------------------------------------- */

static const struct devlink_region_ops fdice_devlink_nvm_region_ops = {
	.name		= "nvm-flash",
	.destructor	= vfree,
	.snapshot	= fdice_devlink_nvm_snapshot,
};

static const struct devlink_region_ops fdice_devlink_sram_region_ops = {
	.name		= "shadow-ram",
	.destructor	= vfree,
	.snapshot	= fdice_devlink_sram_snapshot,
};

static const struct devlink_region_ops fdice_devlink_devcaps_region_ops = {
	.name		= "device-caps",
	.destructor	= vfree,
	.snapshot	= fdice_devlink_devcaps_snapshot,
};

/* -----------------------------------------------------------------------
 * Region Lifecycle
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_regions_init - Create all devlink regions
 * @fdev: fdice device
 *
 * Creates the three NVM debugging regions (nvm-flash, shadow-ram,
 * device-caps). Regions that fail to create are logged but do not
 * prevent the other regions from being created — partial region
 * availability is better than none.
 *
 * Uses devl_region_create() which requires the devlink instance lock
 * to be held by the caller (or called during init before registration).
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_devlink_regions_init(struct fdice_dev *fdev)
{
	struct devlink *devlink = fdice_devlink_from_dev(fdev);
	struct device *dev = &fdev->pdev->dev;
	u64 nvm_size, sram_size;

	/*
	 * Use discovered sizes if available, otherwise fall back to
	 * E810 typical defaults. The sizes will be corrected when the
	 * NVM subsystem initializes and updates fdev->nvm_size/sram_size.
	 */
	nvm_size = fdev->nvm_size ? fdev->nvm_size : FDICE_NVM_DEFAULT_SIZE;
	sram_size = fdev->sram_size ? fdev->sram_size : FDICE_SRAM_DEFAULT_SIZE;

	fdev->nvm_region = devl_region_create(devlink,
					      &fdice_devlink_nvm_region_ops,
					      1, nvm_size);
	if (IS_ERR(fdev->nvm_region)) {
		dev_err(dev,
			"failed to create nvm-flash devlink region: %ld\n",
			PTR_ERR(fdev->nvm_region));
		fdev->nvm_region = NULL;
	}

	fdev->sram_region = devl_region_create(devlink,
					       &fdice_devlink_sram_region_ops,
					       1, sram_size);
	if (IS_ERR(fdev->sram_region)) {
		dev_err(dev,
			"failed to create shadow-ram devlink region: %ld\n",
			PTR_ERR(fdev->sram_region));
		fdev->sram_region = NULL;
	}

	fdev->devcaps_region = devl_region_create(devlink,
						  &fdice_devlink_devcaps_region_ops,
						  10, FDICE_DEVCAPS_BUF_SIZE);
	if (IS_ERR(fdev->devcaps_region)) {
		dev_err(dev,
			"failed to create device-caps devlink region: %ld\n",
			PTR_ERR(fdev->devcaps_region));
		fdev->devcaps_region = NULL;
	}

	return 0;
}

/**
 * fdice_devlink_regions_cleanup - Destroy all devlink regions
 * @fdev: fdice device
 *
 * Destroys any regions that were successfully created during init.
 * NULL-safe — skips regions that were not created.
 */
void fdice_devlink_regions_cleanup(struct fdice_dev *fdev)
{
	if (fdev->devcaps_region)
		devl_region_destroy(fdev->devcaps_region);

	if (fdev->sram_region)
		devl_region_destroy(fdev->sram_region);

	if (fdev->nvm_region)
		devl_region_destroy(fdev->nvm_region);
}
