// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_DEVLINK_H_
#define _FDICE_DEVLINK_H_

/**
 * DOC: FrankenDrICE Devlink Integration
 *
 * Devlink ops, parameters, regions, and lifecycle management for the
 * FrankenDrICE E810 driver. Wires subsystems (health, RDMA, NVM, DDP)
 * into the kernel devlink interface for unified device management.
 *
 * Design references:
 *   - docs/exploration/07-devlink-integration.md
 *   - docs/architecture/06-implementation-plan.md (Phase 3.9)
 *
 * info_get reports: FW version, NVM version, board ID, DDP package
 * name/version, and driver version.
 *
 * reload supports DRIVER_REINIT via fdice_unload()/fdice_load() cycle.
 *
 * Parameters: enable_roce, enable_iwarp (runtime).
 *
 * Regions: nvm-flash, shadow-ram, device-caps (for NVM debugging).
 */

#include <net/devlink.h>

struct fdice_dev;

/* -----------------------------------------------------------------------
 * Devlink Ops (defined in fdice_devlink.c)
 * ----------------------------------------------------------------------- */

extern const struct devlink_ops fdice_devlink_ops;

/* -----------------------------------------------------------------------
 * Driver-specific Devlink Parameter IDs
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_devlink_param_id - Driver-specific devlink param identifiers
 * @FDICE_DEVLINK_PARAM_ID_BASE: start after generic IDs
 *
 * Currently no driver-specific parameters are defined; all registered
 * params use generic DEVLINK_PARAM_GENERIC() IDs (ENABLE_ROCE,
 * ENABLE_IWARP). This enum reserves the numbering space for future
 * driver-specific params.
 */
enum fdice_devlink_param_id {
	FDICE_DEVLINK_PARAM_ID_BASE = DEVLINK_PARAM_GENERIC_ID_MAX,
};

/* -----------------------------------------------------------------------
 * Devlink Init / Cleanup (params + regions)
 * ----------------------------------------------------------------------- */

int fdice_devlink_init(struct fdice_dev *fdev);
void fdice_devlink_cleanup(struct fdice_dev *fdev);

/* -----------------------------------------------------------------------
 * Devlink Regions (fdice_devlink_region.c)
 * ----------------------------------------------------------------------- */

int fdice_devlink_regions_init(struct fdice_dev *fdev);
void fdice_devlink_regions_cleanup(struct fdice_dev *fdev);

#endif /* _FDICE_DEVLINK_H_ */
