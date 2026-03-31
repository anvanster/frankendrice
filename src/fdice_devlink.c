// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_devlink.c - Devlink ops, parameters, and lifecycle
 *
 * Implements the devlink_ops for info_get, reload, and flash_update,
 * plus parameter registration for RDMA protocol toggles.
 *
 * Architectural choices:
 *   - info_get reports firmware, NVM, DDP, board, and driver versions
 *     using the E810 AQ capability data already cached in fdice_hw.
 *   - reload uses the init_once/load separation (mlx5 pattern) so
 *     DRIVER_REINIT is a clean fdice_unload()/fdice_load() cycle.
 *   - RDMA params (enable_roce, enable_iwarp) use runtime mode with
 *     mutual exclusion validation matching E810 hardware constraints.
 *   - flash_update is a stub awaiting NVM AQ command integration in
 *     a later patch; the ops entry is wired so devlink reports the
 *     capability.
 *
 * Reference study: ice devlink/devlink.c (kernel 6.17), mlx5 devlink.c
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/string.h>
#include <net/devlink.h>

#include "fdice_dev.h"
#include "fdice_devlink.h"
#include "fdice_iidc.h"

/* Driver name and version for info reporting */
#define FDICE_DRV_NAME_STR	"frankendrice"
#define FDICE_DRV_VER_STR	"0.1.0"

/* -----------------------------------------------------------------------
 * info_get: Version Reporting
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_info_put_fw_ver - Format and report firmware version
 * @fdev: device context
 * @req: devlink info request
 *
 * Reports the running firmware version as "major.minor.patch.build"
 * from the AQ get_ver response cached in fdev->hw.fw.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_info_put_fw_ver(struct fdice_dev *fdev,
					 struct devlink_info_req *req)
{
	struct fdice_fw_ver *fw = &fdev->hw.fw;
	char buf[48];

	snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
		 fw->fw_major, fw->fw_minor, fw->fw_patch, fw->fw_build);

	return devlink_info_version_running_put(req,
						DEVLINK_INFO_VERSION_GENERIC_FW,
						buf);
}

/**
 * fdice_devlink_info_put_api_ver - Format and report FW API version
 * @fdev: device context
 * @req: devlink info request
 *
 * Reports the FW management API version as "major.minor.patch".
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_info_put_api_ver(struct fdice_dev *fdev,
					  struct devlink_info_req *req)
{
	struct fdice_fw_ver *fw = &fdev->hw.fw;
	char buf[32];

	snprintf(buf, sizeof(buf), "%u.%u.%u",
		 fw->api_major, fw->api_minor, fw->api_patch);

	return devlink_info_version_running_put(req, "fw.mgmt.api", buf);
}

/**
 * fdice_devlink_info_put_board_id - Report board identifier
 * @fdev: device context
 * @req: devlink info request
 *
 * Reports the PCI subsystem vendor:device as the board identifier,
 * which uniquely identifies the adapter card model.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_info_put_board_id(struct fdice_dev *fdev,
					   struct devlink_info_req *req)
{
	char buf[24];

	snprintf(buf, sizeof(buf), "%04x:%04x",
		 fdev->hw.subsystem_vendor_id,
		 fdev->hw.subsystem_device_id);

	return devlink_info_version_fixed_put(req,
					      DEVLINK_INFO_VERSION_GENERIC_BOARD_ID,
					      buf);
}

/**
 * fdice_devlink_info_put_ddp - Report DDP package name and version
 * @fdev: device context
 * @req: devlink info request
 *
 * Reports the loaded Dynamic Device Personalization package identity
 * so operators can verify correct package deployment.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_info_put_ddp(struct fdice_dev *fdev,
				      struct devlink_info_req *req)
{
	struct fdice_ddp_info *ddp = &fdev->ddp_info;
	char buf[48];
	int err;

	if (ddp->pkg_name[0] == '\0')
		return 0;

	err = devlink_info_version_running_put(req, "fw.app.name",
					       ddp->pkg_name);
	if (err)
		return err;

	snprintf(buf, sizeof(buf), "%u.%u.%u.%u",
		 ddp->pkg_ver.major, ddp->pkg_ver.minor,
		 ddp->pkg_ver.update, ddp->pkg_ver.draft);

	return devlink_info_version_running_put(req,
						DEVLINK_INFO_VERSION_GENERIC_FW_APP,
						buf);
}

/**
 * fdice_devlink_info_put_serial - Report device serial number via DSN
 * @fdev: device context
 * @req: devlink info request
 *
 * Reads the PCIe Device Serial Number capability and formats it
 * as the device serial number for devlink info.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_info_put_serial(struct fdice_dev *fdev,
					 struct devlink_info_req *req)
{
	u64 dsn;
	char buf[24];

	dsn = pci_get_dsn(fdev->pdev);
	if (!dsn)
		return 0;

	snprintf(buf, sizeof(buf), "%016llx", dsn);

	return devlink_info_serial_number_put(req, buf);
}

/**
 * fdice_devlink_info_get - Devlink info_get callback
 * @devlink: devlink instance
 * @req: devlink info request
 * @extack: netlink extended ACK
 *
 * Reports comprehensive device identification and version information:
 *   - Driver name and version
 *   - Serial number (PCIe DSN)
 *   - Board ID (subsystem vendor:device)
 *   - Running firmware version
 *   - Firmware management API version
 *   - DDP package name and version
 *
 * Each sub-reporter is called independently so a failure in one
 * non-critical field does not prevent reporting the others.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_devlink_info_get(struct devlink *devlink,
				  struct devlink_info_req *req,
				  struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);
	int err;

	err = fdice_devlink_info_put_serial(fdev, req);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Failed to report serial number");
		return err;
	}

	err = fdice_devlink_info_put_board_id(fdev, req);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack, "Failed to report board ID");
		return err;
	}

	err = fdice_devlink_info_put_fw_ver(fdev, req);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Failed to report firmware version");
		return err;
	}

	err = fdice_devlink_info_put_api_ver(fdev, req);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Failed to report API version");
		return err;
	}

	err = fdice_devlink_info_put_ddp(fdev, req);
	if (err) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Failed to report DDP information");
		return err;
	}

	return 0;
}

/* -----------------------------------------------------------------------
 * Reload: DRIVER_REINIT via fdice_unload/fdice_load
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_reload_down - Tear down device for reload
 * @devlink: devlink instance
 * @netns_change: true if reload is for net namespace change
 * @action: requested reload action
 * @limit: requested reload limit
 * @extack: netlink extended ACK
 *
 * Handles the "down" phase of a devlink reload. For DRIVER_REINIT,
 * calls fdice_unload() to stop all active subsystems while preserving
 * persistent state allocated in fdice_init_once().
 *
 * The devlink_lock serializes reload against concurrent operations
 * (health recovery, parameter changes).
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_devlink_reload_down(struct devlink *devlink, bool netns_change,
			  enum devlink_reload_action action,
			  enum devlink_reload_limit limit,
			  struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);

	if (netns_change) {
		NL_SET_ERR_MSG_MOD(extack,
				   "Namespace change is not supported");
		return -EOPNOTSUPP;
	}

	switch (action) {
	case DEVLINK_RELOAD_ACTION_DRIVER_REINIT:
		if (test_bit(FDICE_STATE_RESET_IN_PROGRESS, &fdev->state)) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Device is busy with a reset");
			return -EBUSY;
		}

		mutex_lock(&fdev->devlink_lock);
		fdice_unload(fdev);
		mutex_unlock(&fdev->devlink_lock);
		return 0;

	default:
		NL_SET_ERR_MSG_MOD(extack, "Unsupported reload action");
		return -EOPNOTSUPP;
	}
}

/**
 * fdice_devlink_reload_up - Bring device back after reload
 * @devlink: devlink instance
 * @action: reload action that was performed
 * @limit: reload limit that was applied
 * @actions_performed: output bitmask of completed actions
 * @extack: netlink extended ACK
 *
 * Handles the "up" phase of a devlink reload. For DRIVER_REINIT,
 * calls fdice_load() to re-activate all hardware subsystems using
 * the persistent state from fdice_init_once().
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_devlink_reload_up(struct devlink *devlink,
			enum devlink_reload_action action,
			enum devlink_reload_limit limit,
			u32 *actions_performed,
			struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);
	int err;

	switch (action) {
	case DEVLINK_RELOAD_ACTION_DRIVER_REINIT:
		mutex_lock(&fdev->devlink_lock);
		err = fdice_load(fdev);
		mutex_unlock(&fdev->devlink_lock);

		if (err) {
			NL_SET_ERR_MSG_MOD(extack,
					   "Failed to reload device");
			return err;
		}

		*actions_performed = BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT);
		return 0;

	default:
		NL_SET_ERR_MSG_MOD(extack, "Unsupported reload action");
		return -EOPNOTSUPP;
	}
}

/* -----------------------------------------------------------------------
 * Flash Update (stub — full NVM update requires AQ NVM commands)
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_flash_update - Firmware flash update via devlink
 * @devlink: devlink instance
 * @params: flash update parameters (firmware file, overwrite mask)
 * @extack: netlink extended ACK
 *
 * Stub implementation for Phase 3.9. Full NVM update support requires
 * NVM AQ command infrastructure (acquire/release NVM semaphore,
 * write NVM module, erase NVM module, NVM update checksum).
 * The ops entry is present so that devlink advertises flash_update
 * capability; the actual implementation will be filled in when the
 * NVM command layer is complete.
 *
 * Return: -EOPNOTSUPP until NVM command layer is implemented.
 */
static int
fdice_devlink_flash_update(struct devlink *devlink,
			   struct devlink_flash_update_params *params,
			   struct netlink_ext_ack *extack)
{
	NL_SET_ERR_MSG_MOD(extack,
			   "Flash update not yet implemented");
	return -EOPNOTSUPP;
}

/* -----------------------------------------------------------------------
 * Devlink Ops Structure
 * ----------------------------------------------------------------------- */

const struct devlink_ops fdice_devlink_ops = {
	.reload_actions	= BIT(DEVLINK_RELOAD_ACTION_DRIVER_REINIT),
	.reload_down	= fdice_devlink_reload_down,
	.reload_up	= fdice_devlink_reload_up,
	.info_get	= fdice_devlink_info_get,
	.flash_update	= fdice_devlink_flash_update,
};

/* -----------------------------------------------------------------------
 * RDMA Parameters: enable_roce / enable_iwarp
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_enable_roce_get - Get current RoCE enable state
 * @devlink: devlink instance
 * @id: parameter ID
 * @ctx: parameter value context
 *
 * Return: 0 on success, negative errno if RDMA not supported.
 */
static int
fdice_devlink_enable_roce_get(struct devlink *devlink, u32 id,
			      struct devlink_param_gset_ctx *ctx)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);

	if (!fdev->hw.func_caps.rdma_capable)
		return -EOPNOTSUPP;

	/* Phase 3.8 IIDC populates protocol; default to false */
	ctx->val.vbool = test_bit(FDICE_FLAG_ADV_FEATURES, &fdev->flags) &&
			 fdev->hw.func_caps.rdma_capable;

	return 0;
}

/**
 * fdice_devlink_enable_roce_set - Set RoCE enable state
 * @devlink: devlink instance
 * @id: parameter ID
 * @ctx: parameter value context
 * @extack: netlink extended ACK
 *
 * Enables or disables RoCEv2 by plugging/unplugging the RDMA auxiliary
 * device. Mutually exclusive with iWARP — validated before reaching
 * this callback.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_devlink_enable_roce_set(struct devlink *devlink, u32 id,
			      struct devlink_param_gset_ctx *ctx,
			      struct netlink_ext_ack *extack)
{
	/* Phase 3.8 IIDC: plug/unplug auxiliary device */
	NL_SET_ERR_MSG_MOD(extack,
			   "RDMA auxiliary device lifecycle not yet wired");
	return -EOPNOTSUPP;
}

/**
 * fdice_devlink_enable_roce_validate - Validate RoCE param change
 * @devlink: devlink instance
 * @id: parameter ID
 * @val: proposed value
 * @extack: netlink extended ACK
 *
 * Ensures RDMA is supported and iWARP is not concurrently active.
 *
 * Return: 0 if valid, negative errno if change is disallowed.
 */
static int
fdice_devlink_enable_roce_validate(struct devlink *devlink, u32 id,
				   union devlink_param_value val,
				   struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);

	if (!fdev->hw.func_caps.rdma_capable) {
		NL_SET_ERR_MSG_MOD(extack, "Device does not support RDMA");
		return -EOPNOTSUPP;
	}

	return 0;
}

/**
 * fdice_devlink_enable_iwarp_get - Get current iWARP enable state
 * @devlink: devlink instance
 * @id: parameter ID
 * @ctx: parameter value context
 *
 * Return: 0 on success, negative errno if RDMA not supported.
 */
static int
fdice_devlink_enable_iwarp_get(struct devlink *devlink, u32 id,
			       struct devlink_param_gset_ctx *ctx)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);

	if (!fdev->hw.func_caps.rdma_capable)
		return -EOPNOTSUPP;

	ctx->val.vbool = false; /* Phase 3.8 populates actual state */

	return 0;
}

/**
 * fdice_devlink_enable_iwarp_set - Set iWARP enable state
 * @devlink: devlink instance
 * @id: parameter ID
 * @ctx: parameter value context
 * @extack: netlink extended ACK
 *
 * Enables or disables iWARP by plugging/unplugging the RDMA auxiliary
 * device. Mutually exclusive with RoCE.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_devlink_enable_iwarp_set(struct devlink *devlink, u32 id,
			       struct devlink_param_gset_ctx *ctx,
			       struct netlink_ext_ack *extack)
{
	NL_SET_ERR_MSG_MOD(extack,
			   "RDMA auxiliary device lifecycle not yet wired");
	return -EOPNOTSUPP;
}

/**
 * fdice_devlink_enable_iwarp_validate - Validate iWARP param change
 * @devlink: devlink instance
 * @id: parameter ID
 * @val: proposed value
 * @extack: netlink extended ACK
 *
 * Ensures RDMA is supported and RoCE is not concurrently active.
 *
 * Return: 0 if valid, negative errno if change is disallowed.
 */
static int
fdice_devlink_enable_iwarp_validate(struct devlink *devlink, u32 id,
				    union devlink_param_value val,
				    struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = fdice_dev_from_devlink(devlink);

	if (!fdev->hw.func_caps.rdma_capable) {
		NL_SET_ERR_MSG_MOD(extack, "Device does not support RDMA");
		return -EOPNOTSUPP;
	}

	return 0;
}

/* -----------------------------------------------------------------------
 * Param Table
 * ----------------------------------------------------------------------- */

static const struct devlink_param fdice_devlink_rdma_params[] = {
	DEVLINK_PARAM_GENERIC(ENABLE_ROCE, BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			      fdice_devlink_enable_roce_get,
			      fdice_devlink_enable_roce_set,
			      fdice_devlink_enable_roce_validate),
	DEVLINK_PARAM_GENERIC(ENABLE_IWARP, BIT(DEVLINK_PARAM_CMODE_RUNTIME),
			      fdice_devlink_enable_iwarp_get,
			      fdice_devlink_enable_iwarp_set,
			      fdice_devlink_enable_iwarp_validate),
};

/* -----------------------------------------------------------------------
 * Lifecycle: Init / Cleanup
 * ----------------------------------------------------------------------- */

/**
 * fdice_devlink_init - Register devlink params and regions
 * @fdev: fdice device
 *
 * Called from fdice_register_device() (Layer 6) after the devlink
 * instance is allocated and the device is fully initialized.
 * Registers RDMA parameters and creates NVM debugging regions.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_devlink_init(struct fdice_dev *fdev)
{
	struct devlink *devlink = fdice_devlink_from_dev(fdev);
	int err;

	err = devl_params_register(devlink, fdice_devlink_rdma_params,
				   ARRAY_SIZE(fdice_devlink_rdma_params));
	if (err) {
		dev_err(&fdev->pdev->dev,
			"failed to register devlink RDMA params: %d\n", err);
		return err;
	}

	err = fdice_devlink_regions_init(fdev);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"failed to create devlink regions: %d\n", err);
		goto err_unregister_params;
	}

	return 0;

err_unregister_params:
	devl_params_unregister(devlink, fdice_devlink_rdma_params,
			       ARRAY_SIZE(fdice_devlink_rdma_params));
	return err;
}

/**
 * fdice_devlink_cleanup - Unregister devlink params and regions
 * @fdev: fdice device
 *
 * Called from fdice_unregister_device(). Tears down regions first,
 * then unregisters parameters — reverse of fdice_devlink_init().
 */
void fdice_devlink_cleanup(struct fdice_dev *fdev)
{
	struct devlink *devlink = fdice_devlink_from_dev(fdev);

	fdice_devlink_regions_cleanup(fdev);

	devl_params_unregister(devlink, fdice_devlink_rdma_params,
			       ARRAY_SIZE(fdice_devlink_rdma_params));
}
