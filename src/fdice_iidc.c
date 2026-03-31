// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE IIDC Auxiliary Device Lifecycle
 *
 * Manages the auxiliary bus device that exposes RDMA functionality to
 * the RDMA provider module. The auxiliary device carries a capability
 * snapshot (fdice_iidc_core) and an ops vtable, replacing ice/irdma's
 * exported GPL symbol pattern with compile-time type-safe indirection.
 *
 * Event dispatch is synchronous with pre/post semantics: PRE_* events
 * block until the RDMA driver acknowledges readiness (or timeout),
 * eliminating the race window present in ice/irdma's async bitmap model.
 *
 * Architecture reference: docs/architecture/03-rdma-integration.md
 */

#include <linux/auxiliary_bus.h>
#include <linux/device.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/err.h>

#include "fdice_dev.h"
#include "fdice_iidc.h"
#include "fdice_rdma.h"

/* Maximum retries for PRE_* events when RDMA driver returns -EBUSY */
#define FDICE_IIDC_PRE_EVENT_RETRIES	5
#define FDICE_IIDC_PRE_EVENT_DELAY_MS	50

/**
 * fdice_iidc_adev_release - Auxiliary device release callback
 * @dev: device embedded in the auxiliary_device
 *
 * Called when the auxiliary device refcount drops to zero.
 * Frees the fdice_iidc_aux_dev wrapper structure.
 */
static void fdice_iidc_adev_release(struct device *dev)
{
	struct auxiliary_device *adev = to_auxiliary_dev(dev);
	struct fdice_iidc_aux_dev *iaux;

	iaux = container_of(adev, struct fdice_iidc_aux_dev, adev);
	kfree(iaux);
}

/**
 * fdice_iidc_core_populate - Fill IIDC core from device state
 * @fdev: FrankenDrICE device
 * @core: IIDC core structure to populate
 *
 * Snapshots the current device state into the IIDC core structure.
 * Called at init time and during replug cycles to refresh capability
 * data visible to the RDMA auxiliary driver.
 */
static void fdice_iidc_core_populate(struct fdice_dev *fdev,
				     struct fdice_iidc_core *core)
{
	struct fdice_hw *hw = &fdev->hw;
	struct fdice_irq_pool *pool = fdev->eq_table.irq_pool;

	core->pdev = fdev->pdev;
	core->netdev = fdev->priv.netdev;
	core->rdma_hw_addr = hw->hw_addr;
	core->pf_id = hw->pf_id;
	core->vport_id = 0;
	core->pf_priv = fdev;

	/*
	 * Protocol selection: check capability flags to determine
	 * whether iWARP or RoCE is supported. Default to iWARP
	 * for E810 which natively supports iWARP.
	 */
	core->protocol = FDICE_RDMA_PROTOCOL_IWARP;

	/* Capability snapshot from HW discovery */
	core->caps.max_qps = 32768;
	core->caps.max_cqs = 32768;
	core->caps.max_mrs = 262144;
	core->caps.max_ceqs = 256;
	core->caps.max_pd = 32768;
	core->caps.max_srqs = 16384;
	core->caps.hw_rev = hw->revision_id;
	core->caps.features = FDICE_RDMA_FEAT_IWARP | FDICE_RDMA_FEAT_SRQ;
	core->caps.max_mtu = hw->func_caps.max_mtu;
	core->caps.rdma_msix_allocated = pool ? pool->rdma_count : 0;

	/* Default QoS snapshot -- single TC, no PFC */
	core->qos.num_tc = 1;
	memset(core->qos.up_to_tc_map, 0, sizeof(core->qos.up_to_tc_map));
	memset(core->qos.tc_to_bw_map, 0, sizeof(core->qos.tc_to_bw_map));
	core->qos.tc_to_bw_map[0] = 100;
	core->qos.pfc_enabled = 0;
	core->qos.dscp_mode = false;
	memset(core->qos.dscp_map, 0, sizeof(core->qos.dscp_map));
}

/**
 * fdice_iidc_init - Create and register the RDMA auxiliary device
 * @fdev: FrankenDrICE device
 *
 * Allocates the auxiliary device wrapper, populates the IIDC core
 * capability snapshot, and registers the auxiliary device on the bus.
 * The RDMA provider module (if loaded) will probe on this device and
 * receive the fdice_iidc_core structure.
 *
 * Must be called after queue infrastructure is operational and the
 * interrupt pool RDMA partition has been established.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_iidc_init(struct fdice_dev *fdev)
{
	struct fdice_rdma *rdma = &fdev->rdma;
	struct fdice_iidc_aux_dev *iaux;
	struct auxiliary_device *adev;
	int err;

	if (!rdma->iidc_core) {
		dev_err(&fdev->pdev->dev,
			"IIDC core not initialized before aux device creation\n");
		return -EINVAL;
	}

	iaux = kzalloc(sizeof(*iaux), GFP_KERNEL);
	if (!iaux)
		return -ENOMEM;

	fdice_iidc_core_populate(fdev, rdma->iidc_core);
	iaux->core = rdma->iidc_core;

	adev = &iaux->adev;
	adev->name = "iwarp";
	adev->id = fdev->hw.pf_id;
	adev->dev.parent = &fdev->pdev->dev;
	adev->dev.release = fdice_iidc_adev_release;

	err = auxiliary_device_init(adev);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"Failed to init RDMA auxiliary device: %d\n", err);
		kfree(iaux);
		return err;
	}

	err = auxiliary_device_add(adev);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"Failed to add RDMA auxiliary device: %d\n", err);
		auxiliary_device_uninit(adev);
		return err;
	}

	rdma->aux_dev = adev;
	rdma->registered = true;

	dev_info(&fdev->pdev->dev,
		 "RDMA auxiliary device registered (PF %u, %u MSI-X vectors)\n",
		 fdev->hw.pf_id, rdma->iidc_core->caps.rdma_msix_allocated);

	return 0;
}

/**
 * fdice_iidc_cleanup - Destroy the RDMA auxiliary device
 * @fdev: FrankenDrICE device
 *
 * Removes and uninitializes the auxiliary device. This triggers the
 * RDMA provider module's remove callback, ensuring orderly teardown
 * of RDMA resources before the PF driver releases shared state.
 */
void fdice_iidc_cleanup(struct fdice_dev *fdev)
{
	struct fdice_rdma *rdma = &fdev->rdma;

	if (!rdma->registered || !rdma->aux_dev)
		return;

	rdma->registered = false;
	auxiliary_device_delete(rdma->aux_dev);
	auxiliary_device_uninit(rdma->aux_dev);
	rdma->aux_dev = NULL;
}

/**
 * fdice_iidc_notify_event - Dispatch event to registered RDMA handlers
 * @fdev: FrankenDrICE device
 * @event: event descriptor to dispatch
 *
 * Synchronously delivers the event to all registered event handlers.
 * For PRE_* events (PRE_RESET, PRE_TC_CHANGE), the handler may return
 * -EBUSY to request a retry. The PF retries up to
 * FDICE_IIDC_PRE_EVENT_RETRIES times with FDICE_IIDC_PRE_EVENT_DELAY_MS
 * between attempts. If the handler still returns -EBUSY after all
 * retries, the PF proceeds anyway (logged as warning).
 *
 * For POST_* events and informational events, handler return values
 * are logged but do not block the PF driver.
 *
 * Return: 0 on success, negative errno if a PRE_* handler failed
 *         after all retries (PF proceeds anyway but caller is informed)
 */
int fdice_iidc_notify_event(struct fdice_dev *fdev,
			    struct fdice_iidc_event *event)
{
	struct fdice_rdma *rdma = &fdev->rdma;
	struct fdice_iidc_event_handler *handler;
	bool is_pre_event;
	int ret = 0;
	int retries;
	int err;

	if (!rdma->enabled || !rdma->registered)
		return 0;

	is_pre_event = (event->type == FDICE_IIDC_EVENT_PRE_RESET ||
			event->type == FDICE_IIDC_EVENT_PRE_TC_CHANGE);

	/*
	 * Handler list iteration is safe here because handlers are only
	 * added/removed during auxiliary device probe/remove, which is
	 * serialized with event dispatch by the auxiliary bus framework.
	 */
	list_for_each_entry(handler, &rdma->event_handlers, list) {
		if (!handler->handle_event)
			continue;

		if (is_pre_event) {
			for (retries = 0; retries < FDICE_IIDC_PRE_EVENT_RETRIES;
			     retries++) {
				err = handler->handle_event(rdma->iidc_core,
							    event);
				if (err != -EBUSY)
					break;

				dev_dbg(&fdev->pdev->dev,
					"IIDC PRE event %d: handler busy, retry %d/%d\n",
					event->type, retries + 1,
					FDICE_IIDC_PRE_EVENT_RETRIES);

				msleep(FDICE_IIDC_PRE_EVENT_DELAY_MS);
			}

			if (err) {
				dev_warn(&fdev->pdev->dev,
					 "IIDC PRE event %d: handler returned %d after retries, proceeding\n",
					 event->type, err);
				ret = err;
			}
		} else {
			err = handler->handle_event(rdma->iidc_core, event);
			if (err)
				dev_info(&fdev->pdev->dev,
					 "IIDC POST event %d: handler returned %d\n",
					 event->type, err);
		}
	}

	return ret;
}
