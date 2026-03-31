// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE PF-side RDMA Subsystem
 *
 * Manages RDMA capability detection, MSI-X vector partitioning, IIDC
 * core initialization, and auxiliary device lifecycle from the PF
 * driver's perspective. The actual RDMA transport (iWARP or RoCE) is
 * handled by a separate provider module that probes on the auxiliary
 * device registered here.
 *
 * Interrupt partitioning reserves 1/3 of MSI-X vectors for RDMA at
 * init time, enabling the RDMA driver to allocate vectors from a
 * dedicated partition without per-vector AQ round-trips. This improves
 * over ice's proxied ice_alloc_rdma_qvector() pattern.
 *
 * Architecture reference: docs/architecture/03-rdma-integration.md
 */

#include <linux/slab.h>
#include <linux/device.h>

#include "fdice_dev.h"
#include "fdice_rdma.h"
#include "fdice_iidc.h"

/**
 * fdice_rdma_partition_irqs - Reserve MSI-X vectors for RDMA
 * @fdev: FrankenDrICE device
 *
 * Pre-partitions a fraction (1/FDICE_RDMA_MSIX_DIVISOR) of the total
 * MSI-X pool for the RDMA subsystem. The partition starts after the
 * networking vectors and is reported in fdice_rdma_caps.rdma_msix_allocated.
 *
 * If the resulting partition is smaller than FDICE_RDMA_MSIX_MIN, RDMA
 * is not enabled (insufficient vectors for meaningful RDMA operation).
 *
 * Return: number of vectors allocated, 0 if insufficient
 */
static u16 fdice_rdma_partition_irqs(struct fdice_dev *fdev)
{
	struct fdice_irq_pool *pool = fdev->eq_table.irq_pool;
	u16 total;
	u16 rdma_count;

	if (!pool)
		return 0;

	total = pool->msix_count;
	rdma_count = total / FDICE_RDMA_MSIX_DIVISOR;

	if (rdma_count < FDICE_RDMA_MSIX_MIN) {
		dev_info(&fdev->pdev->dev,
			 "Insufficient MSI-X for RDMA partition (%u/%u needed)\n",
			 rdma_count, FDICE_RDMA_MSIX_MIN);
		return 0;
	}

	/*
	 * Set the RDMA partition in the IRQ pool. The base index
	 * starts after all networking vectors (total - rdma_count).
	 */
	pool->rdma_base = total - rdma_count;
	pool->rdma_count = rdma_count;

	dev_dbg(&fdev->pdev->dev,
		"RDMA IRQ partition: base=%u count=%u (total=%u)\n",
		pool->rdma_base, pool->rdma_count, total);

	return rdma_count;
}

/**
 * fdice_rdma_release_irq_partition - Release RDMA MSI-X partition
 * @fdev: FrankenDrICE device
 *
 * Clears the RDMA partition in the IRQ pool, returning vectors to
 * the general pool for networking use.
 */
static void fdice_rdma_release_irq_partition(struct fdice_dev *fdev)
{
	struct fdice_irq_pool *pool = fdev->eq_table.irq_pool;

	if (!pool)
		return;

	pool->rdma_base = 0;
	pool->rdma_count = 0;
}

/**
 * fdice_rdma_init - Initialize PF-side RDMA subsystem
 * @fdev: FrankenDrICE device
 *
 * Performs the following sequence:
 *   1. Check hardware RDMA capability flag
 *   2. Partition MSI-X vectors for RDMA
 *   3. Allocate and populate the IIDC core capability snapshot
 *   4. Register the RDMA auxiliary device on the auxiliary bus
 *
 * If RDMA is not supported by hardware or insufficient MSI-X vectors
 * are available, the function returns 0 (RDMA is optional).
 *
 * Return: 0 on success or RDMA not supported, negative errno on failure
 */
int fdice_rdma_init(struct fdice_dev *fdev)
{
	struct fdice_rdma *rdma = &fdev->rdma;
	u16 rdma_vecs;
	int err;

	INIT_LIST_HEAD(&rdma->event_handlers);
	rdma->enabled = false;
	rdma->registered = false;
	rdma->iidc_core = NULL;
	rdma->aux_dev = NULL;

	/* Check hardware RDMA capability */
	if (!fdev->hw.func_caps.rdma_capable) {
		dev_info(&fdev->pdev->dev,
			 "RDMA not supported by hardware, skipping\n");
		return 0;
	}

	/* Reserve MSI-X vectors for RDMA partition */
	rdma_vecs = fdice_rdma_partition_irqs(fdev);
	if (!rdma_vecs) {
		dev_info(&fdev->pdev->dev,
			 "Insufficient MSI-X vectors for RDMA, skipping\n");
		return 0;
	}

	/* Allocate IIDC core structure */
	rdma->iidc_core = kzalloc(sizeof(*rdma->iidc_core), GFP_KERNEL);
	if (!rdma->iidc_core) {
		err = -ENOMEM;
		goto err_release_irq;
	}

	rdma->enabled = true;

	/* Register auxiliary device (populates iidc_core and creates adev) */
	err = fdice_iidc_init(fdev);
	if (err) {
		dev_err(&fdev->pdev->dev,
			"Failed to register RDMA auxiliary device: %d\n", err);
		goto err_free_core;
	}

	return 0;

err_free_core:
	kfree(rdma->iidc_core);
	rdma->iidc_core = NULL;
	rdma->enabled = false;
err_release_irq:
	fdice_rdma_release_irq_partition(fdev);
	return err;
}

/**
 * fdice_rdma_cleanup - Tear down PF-side RDMA subsystem
 * @fdev: FrankenDrICE device
 *
 * Reverse of fdice_rdma_init(). Safe to call even if init was not
 * called or failed partway through. Ordering:
 *   1. Destroy auxiliary device (triggers RDMA driver remove)
 *   2. Free IIDC core structure
 *   3. Release MSI-X partition back to networking pool
 */
void fdice_rdma_cleanup(struct fdice_dev *fdev)
{
	struct fdice_rdma *rdma = &fdev->rdma;

	fdice_iidc_cleanup(fdev);

	kfree(rdma->iidc_core);
	rdma->iidc_core = NULL;
	rdma->enabled = false;

	fdice_rdma_release_irq_partition(fdev);
}

/**
 * fdice_rdma_notify_reset - Send pre/post reset notifications to RDMA
 * @fdev: FrankenDrICE device
 * @pre: true for PRE_RESET, false for POST_RESET
 *
 * Constructs the appropriate reset event and dispatches it through
 * fdice_iidc_notify_event(). PRE_RESET gives the RDMA driver a chance
 * to save state and quiesce operations. POST_RESET signals that the
 * device is operational again and the RDMA driver may restore state.
 *
 * Return: 0 on success, negative errno if PRE handler fails (PF proceeds)
 */
int fdice_rdma_notify_reset(struct fdice_dev *fdev, bool pre)
{
	struct fdice_iidc_event event = {};

	if (!fdev->rdma.enabled)
		return 0;

	event.type = pre ? FDICE_IIDC_EVENT_PRE_RESET
			 : FDICE_IIDC_EVENT_POST_RESET;

	return fdice_iidc_notify_event(fdev, &event);
}

/**
 * fdice_rdma_notify_tc_change - Send pre/post TC change notifications
 * @fdev: FrankenDrICE device
 * @pre: true for PRE_TC_CHANGE, false for POST_TC_CHANGE
 *
 * Called from the DCB reconfiguration path. PRE_TC_CHANGE gives the
 * RDMA driver a chance to suspend QPs and drain completions before
 * the PF reconfigures traffic classes. POST_TC_CHANGE delivers the
 * updated QoS snapshot (via the iidc_core.qos field) so the RDMA
 * driver can remap QP-to-TC associations.
 *
 * For POST events, the iidc_core.qos snapshot is refreshed before
 * dispatch so the RDMA driver sees the new configuration.
 *
 * Return: 0 on success, negative errno if PRE handler fails (PF proceeds)
 */
int fdice_rdma_notify_tc_change(struct fdice_dev *fdev, bool pre)
{
	struct fdice_rdma *rdma = &fdev->rdma;
	struct fdice_iidc_event event = {};

	if (!rdma->enabled)
		return 0;

	if (pre) {
		event.type = FDICE_IIDC_EVENT_PRE_TC_CHANGE;
	} else {
		event.type = FDICE_IIDC_EVENT_POST_TC_CHANGE;
		/*
		 * Point the event QoS data at the refreshed snapshot
		 * in the IIDC core so the handler can read new config.
		 */
		event.qos = &rdma->iidc_core->qos;
	}

	return fdice_iidc_notify_event(fdev, &event);
}
