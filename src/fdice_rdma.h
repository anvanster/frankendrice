// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_RDMA_H_
#define _FDICE_RDMA_H_

/**
 * DOC: FrankenDrICE PF-side RDMA Subsystem Declarations
 *
 * Internal header for the PF driver's RDMA management layer.
 * This is NOT consumed by the RDMA provider module -- only
 * fdice_iidc.h is the public interface. This header defines
 * the PF-side state that tracks RDMA lifecycle, interrupt
 * partitioning, and event handler registration.
 *
 * Architecture reference: docs/architecture/03-rdma-integration.md
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/auxiliary_bus.h>

#include "fdice_iidc.h"

/* Forward declarations */
struct fdice_dev;

/* -----------------------------------------------------------------------
 * RDMA Interrupt Partition Constants
 * ----------------------------------------------------------------------- */

/**
 * FDICE_RDMA_MSIX_FRACTION - Fraction of MSI-X vectors reserved for RDMA
 *
 * One third of total MSI-X vectors are pre-partitioned for the RDMA
 * subsystem at init time, eliminating per-vector AQ round-trips.
 */
#define FDICE_RDMA_MSIX_DIVISOR		3

/**
 * FDICE_RDMA_MSIX_MIN - Minimum MSI-X vectors to allocate for RDMA
 *
 * Below this threshold, RDMA capability is not advertised.
 */
#define FDICE_RDMA_MSIX_MIN		4

/* -----------------------------------------------------------------------
 * PF-side RDMA State
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_rdma - PF-side RDMA subsystem state
 * @iidc_core: IIDC capability snapshot passed to RDMA provider at probe
 * @aux_dev: registered auxiliary device (NULL if not registered)
 * @event_handlers: list of registered event handler callbacks
 * @enabled: true if RDMA hardware capability was detected
 * @registered: true if auxiliary device is currently registered
 *
 * Embedded in fdice_dev. Manages the lifecycle of the RDMA auxiliary
 * device and coordinates event notification between the PF driver
 * and the RDMA provider module.
 */
struct fdice_rdma {
	struct fdice_iidc_core		*iidc_core;
	struct auxiliary_device		*aux_dev;
	struct list_head		event_handlers;
	bool				enabled;
	bool				registered;
};

/* -----------------------------------------------------------------------
 * RDMA Subsystem Lifecycle
 * ----------------------------------------------------------------------- */

/**
 * fdice_rdma_init - Initialize PF-side RDMA subsystem
 * @fdev: FrankenDrICE device
 *
 * Detects RDMA capability, allocates the interrupt partition, initializes
 * the IIDC core structure, and registers the auxiliary device.
 *
 * Return: 0 on success, negative errno on failure, -ENODEV if RDMA
 *         is not supported by hardware
 */
int fdice_rdma_init(struct fdice_dev *fdev);

/**
 * fdice_rdma_cleanup - Tear down PF-side RDMA subsystem
 * @fdev: FrankenDrICE device
 *
 * Destroys the auxiliary device and releases all RDMA resources.
 * Safe to call even if fdice_rdma_init() was not called or failed.
 */
void fdice_rdma_cleanup(struct fdice_dev *fdev);

/* -----------------------------------------------------------------------
 * RDMA Event Notification Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_rdma_notify_reset - Send pre/post reset notifications to RDMA
 * @fdev: FrankenDrICE device
 * @pre: true for PRE_RESET, false for POST_RESET
 *
 * Called from the PF reset path. PRE_RESET gives the RDMA driver a
 * chance to save state and quiesce before the reset. POST_RESET
 * signals that the device is operational again.
 *
 * Return: 0 on success, negative errno on PRE failure (PF proceeds anyway)
 */
int fdice_rdma_notify_reset(struct fdice_dev *fdev, bool pre);

/**
 * fdice_rdma_notify_tc_change - Send pre/post TC change notifications
 * @fdev: FrankenDrICE device
 * @pre: true for PRE_TC_CHANGE, false for POST_TC_CHANGE
 *
 * Called from the DCB reconfiguration path. PRE gives the RDMA driver
 * a chance to suspend QPs and drain completions. POST delivers the
 * updated QoS snapshot for QP-to-TC remapping.
 *
 * Return: 0 on success, negative errno on PRE failure (PF proceeds anyway)
 */
int fdice_rdma_notify_tc_change(struct fdice_dev *fdev, bool pre);

#endif /* _FDICE_RDMA_H_ */
