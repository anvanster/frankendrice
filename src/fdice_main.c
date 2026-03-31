// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_main.c - FrankenDrICE E810 Network Driver
 *
 * Clean-sheet driver for Intel E810 adapters, informed by architectural
 * patterns studied from mlx5 and ice/irdma. See docs/architecture/ for
 * design rationale.
 *
 * Init architecture uses a 6-layer model inspired by mlx5's structured
 * probe (Exploration 01), with init_once/load separation enabling clean
 * devlink reload support.
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/io.h>
#include <linux/crash_dump.h>
#include <net/devlink.h>

#include "fdice_dev.h"

#define FDICE_DRV_NAME		"frankendrice"
#define FDICE_DRV_VERSION	"0.1.0"
#define FDICE_DRV_DESCRIPTION	"FrankenDrICE E810 Network Driver"

static const struct pci_device_id fdice_pci_tbl[] = {
	{ PCI_VDEVICE(INTEL, FDICE_DEV_ID_E810C_BACKPLANE) },
	{ PCI_VDEVICE(INTEL, FDICE_DEV_ID_E810C_QSFP) },
	{ PCI_VDEVICE(INTEL, FDICE_DEV_ID_E810C_SFP) },
	{ PCI_VDEVICE(INTEL, FDICE_DEV_ID_E810_XXV_BACKPLANE) },
	{ PCI_VDEVICE(INTEL, FDICE_DEV_ID_E810_XXV_QSFP) },
	{ PCI_VDEVICE(INTEL, FDICE_DEV_ID_E810_XXV_SFP) },
	{ }
};
MODULE_DEVICE_TABLE(pci, fdice_pci_tbl);

/*
 * Devlink ops — minimal for Phase 3.1, extended in Phase 3.9.
 * Defined early because devlink_alloc() requires the ops struct.
 */
static const struct devlink_ops fdice_devlink_ops = {
};

/* ---- Layer 1: Core device init (mutexes, debug, health timer) ---- */

/**
 * fdice_mdev_init - Initialize core device structures
 * @fdev: fdice device
 *
 * First init layer. Sets up synchronization primitives and debug
 * infrastructure. No hardware access — only software state.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_mdev_init(struct fdice_dev *fdev)
{
	mutex_init(&fdev->devlink_lock);
	/* Phase 3.4: health timer init goes here */
	return 0;
}

/**
 * fdice_mdev_uninit - Tear down core device structures
 * @fdev: fdice device
 *
 * Reverse of fdice_mdev_init(). Last teardown layer — destroys
 * mutexes and cancels health timer.
 */
static void fdice_mdev_uninit(struct fdice_dev *fdev)
{
	mutex_destroy(&fdev->devlink_lock);
}

/* ---- Layer 2: PCI resource init (enable, BAR map, hw struct) ---- */

/**
 * fdice_pci_init - Enable PCI device and map resources
 * @fdev: fdice device
 *
 * Second init layer. Enables PCI device, maps BAR0, populates the
 * fdice_hw structure with PCI identity. Handles kdump FLR clearing
 * of pending DMA transactions (ice pattern).
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_pci_init(struct fdice_dev *fdev)
{
	struct pci_dev *pdev = fdev->pdev;
	struct fdice_hw *hw = &fdev->hw;
	int err;

	err = pcim_enable_device(pdev);
	if (err) {
		dev_err(&pdev->dev, "failed to enable PCI device: %d\n", err);
		return err;
	}

	err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
	if (err) {
		dev_err(&pdev->dev, "DMA 64-bit mask failed, trying 32-bit\n");
		err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(32));
		if (err) {
			dev_err(&pdev->dev, "DMA configuration failed: %d\n",
				err);
			return err;
		}
	}

	err = pcim_iomap_regions(pdev, BIT(0), FDICE_DRV_NAME);
	if (err) {
		dev_err(&pdev->dev, "BAR0 mapping failed: %d\n", err);
		return err;
	}

	hw->hw_addr = pcim_iomap_table(pdev)[0];
	if (!hw->hw_addr) {
		dev_err(&pdev->dev, "BAR0 address is NULL\n");
		return -EIO;
	}

	pci_set_master(pdev);

	hw->pdev = pdev;
	hw->vendor_id = pdev->vendor;
	hw->device_id = pdev->device;
	hw->subsystem_vendor_id = pdev->subsystem_vendor;
	hw->subsystem_device_id = pdev->subsystem_device;
	hw->revision_id = pdev->revision;

	return 0;
}

/**
 * fdice_pci_uninit - Release PCI resources
 * @fdev: fdice device
 *
 * Reverse of fdice_pci_init(). BAR unmapping is handled by devres
 * (pcim_iomap_regions), so only bus-master needs explicit clear.
 */
static void fdice_pci_uninit(struct fdice_dev *fdev)
{
	pci_clear_master(fdev->pdev);
}

/* ---- Layer 3: Function setup/teardown in fdice_hw.c ---- */

/* ---- Layer 4: One-time SW allocation ---- */

/**
 * fdice_init_once - Allocate one-time software structures
 * @fdev: fdice device
 *
 * Fourth init layer. Allocates all subsystem software structures that
 * persist across devlink reload. Separated from fdice_load() to enable
 * clean reload (mlx5 init_once/load pattern, Exploration 01).
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_init_once(struct fdice_dev *fdev)
{
	/* Phase 3.3: queue structures, EQ table */
	/* Phase 3.4: health subsystem */
	/* Phase 3.5: flow steering tree */
	return 0;
}

/**
 * fdice_cleanup_once - Free one-time software structures
 * @fdev: fdice device
 *
 * Reverse of fdice_init_once(). Frees all persistent subsystem state.
 */
static void fdice_cleanup_once(struct fdice_dev *fdev)
{
	/* Phase 3.5: flow steering cleanup */
	/* Phase 3.4: health cleanup */
	/* Phase 3.3: queue structure cleanup */
}

/* ---- Layer 5: Hardware activation ---- */

/**
 * fdice_load - Activate hardware and start subsystems
 * @fdev: fdice device
 *
 * Fifth init layer. Enables hardware queues, starts health monitoring,
 * activates flow steering. This is the layer re-executed on devlink
 * reload (paired with fdice_unload).
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_load(struct fdice_dev *fdev)
{
	/* Phase 3.3: enable queues, start channels */
	/* Phase 3.4: start health polling */
	/* Phase 3.5: activate flow steering */
	/* Phase 3.6: load DDP package */
	return 0;
}

/**
 * fdice_unload - Deactivate hardware and stop subsystems
 * @fdev: fdice device
 *
 * Reverse of fdice_load(). Stops all active subsystems in reverse
 * order. Called on devlink reload and during remove.
 */
void fdice_unload(struct fdice_dev *fdev)
{
	/* Phase 3.6: DDP cleanup */
	/* Phase 3.5: deactivate flow steering */
	/* Phase 3.4: stop health polling */
	/* Phase 3.3: disable queues, stop channels */
}

/* ---- Layer 6: Device registration ---- */

/**
 * fdice_register_device - Register with kernel subsystems
 * @fdev: fdice device
 *
 * Sixth init layer. Registers netdev, devlink ports, auxiliary devices
 * for RDMA. Non-critical registrations (e.g., hwmon) log errors but
 * do not fail the probe.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_register_device(struct fdice_dev *fdev)
{
	/* Phase 3.8: auxiliary device for RDMA */
	/* Phase 3.9: devlink params, regions, reporters */
	/* Phase 3.11: netdev registration */
	return 0;
}

/**
 * fdice_unregister_device - Unregister from kernel subsystems
 * @fdev: fdice device
 *
 * Reverse of fdice_register_device().
 */
static void fdice_unregister_device(struct fdice_dev *fdev)
{
	/* Phase 3.11: netdev unregistration */
	/* Phase 3.9: devlink cleanup */
	/* Phase 3.8: auxiliary device removal */
}

/* ---- Init orchestration ---- */

/**
 * fdice_init_one - Full device initialization
 * @fdev: fdice device
 *
 * Orchestrates layers 3-6. Called from fdice_probe() after PCI init.
 * Uses goto-chain error handling with exact init/cleanup symmetry
 * (mlx5 pattern, fixes ice's fragile flag-based cleanup from
 * Exploration 03).
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_init_one(struct fdice_dev *fdev)
{
	int err;

	err = fdice_function_setup(fdev);
	if (err)
		return err;

	err = fdice_init_once(fdev);
	if (err)
		goto err_function_teardown;

	err = fdice_load(fdev);
	if (err)
		goto err_cleanup_once;

	err = fdice_register_device(fdev);
	if (err)
		goto err_unload;

	set_bit(FDICE_STATE_UP, &fdev->state);

	return 0;

err_unload:
	fdice_unload(fdev);
err_cleanup_once:
	fdice_cleanup_once(fdev);
err_function_teardown:
	fdice_function_teardown(fdev);
	return err;
}

/**
 * fdice_uninit_one - Full device teardown
 * @fdev: fdice device
 *
 * Reverse of fdice_init_one(). State-bit gated — skips unload if
 * device never reached FDICE_STATE_UP (prevents double-free on
 * partial init, mlx5 pattern from Exploration 03).
 */
static void fdice_uninit_one(struct fdice_dev *fdev)
{
	if (test_and_clear_bit(FDICE_STATE_UP, &fdev->state)) {
		fdice_unregister_device(fdev);
		fdice_unload(fdev);
	}
	fdice_cleanup_once(fdev);
	fdice_function_teardown(fdev);
}

/* ---- PCI driver callbacks ---- */

/**
 * fdice_probe - PCI probe callback
 * @pdev: PCI device being probed
 * @ent: matching PCI device ID table entry
 *
 * Thin probe function (mlx5 pattern): allocates devlink (which embeds
 * fdice_dev), runs layered init, and registers devlink. kdump FLR
 * handling clears pending DMA before PCI enable (ice pattern).
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_probe(struct pci_dev *pdev,
		       const struct pci_device_id __always_unused *ent)
{
	struct devlink *devlink;
	struct fdice_dev *fdev;
	int err;

	if (pdev->is_virtfn) {
		dev_err(&pdev->dev, "cannot probe a virtual function\n");
		return -EINVAL;
	}

	/* kdump FLR: clear pending DMA from previous kernel (ice pattern) */
	if (is_kdump_kernel()) {
		pci_save_state(pdev);
		pci_clear_master(pdev);
		err = pcie_flr(pdev);
		if (err)
			return err;
		pci_restore_state(pdev);
	}

	devlink = devlink_alloc(&fdice_devlink_ops, sizeof(*fdev), &pdev->dev);
	if (!devlink) {
		dev_err(&pdev->dev, "devlink allocation failed\n");
		return -ENOMEM;
	}

	fdev = fdice_dev_from_devlink(devlink);
	fdev->pdev = pdev;
	pci_set_drvdata(pdev, fdev);

	/* Layer 1: core device init */
	err = fdice_mdev_init(fdev);
	if (err)
		goto err_devlink_free;

	/* Layer 2: PCI resource init */
	err = fdice_pci_init(fdev);
	if (err)
		goto err_mdev_uninit;

	/* Layers 3-6: function setup, init_once, load, register */
	err = fdice_init_one(fdev);
	if (err)
		goto err_pci_uninit;

	devlink_register(devlink);

	dev_info(&pdev->dev,
		 "FrankenDrICE %04x:%04x BAR0 %pR initialized\n",
		 pdev->vendor, pdev->device, &pdev->resource[0]);

	return 0;

err_pci_uninit:
	fdice_pci_uninit(fdev);
err_mdev_uninit:
	fdice_mdev_uninit(fdev);
err_devlink_free:
	devlink_free(devlink);
	return err;
}

/**
 * fdice_remove - PCI remove callback
 * @pdev: PCI device being removed
 *
 * Sets REMOVING state to prevent races with concurrent operations,
 * then tears down all layers in reverse order. Devlink unregistered
 * first to block new operations.
 */
static void fdice_remove(struct pci_dev *pdev)
{
	struct fdice_dev *fdev = pci_get_drvdata(pdev);
	struct devlink *devlink = fdice_devlink_from_dev(fdev);

	set_bit(FDICE_STATE_REMOVING, &fdev->state);

	devlink_unregister(devlink);

	fdice_uninit_one(fdev);
	fdice_pci_uninit(fdev);
	fdice_mdev_uninit(fdev);

	devlink_free(devlink);
}

/**
 * fdice_shutdown - PCI shutdown callback
 * @pdev: PCI device
 *
 * Called on system shutdown/reboot. Minimal teardown — disable
 * bus-master DMA to prevent stale transactions.
 */
static void fdice_shutdown(struct pci_dev *pdev)
{
	pci_disable_device(pdev);
}

static struct pci_driver fdice_driver = {
	.name		= FDICE_DRV_NAME,
	.id_table	= fdice_pci_tbl,
	.probe		= fdice_probe,
	.remove		= fdice_remove,
	.shutdown	= fdice_shutdown,
};

module_pci_driver(fdice_driver);

MODULE_LICENSE("GPL v2");
MODULE_DESCRIPTION(FDICE_DRV_DESCRIPTION);
MODULE_VERSION(FDICE_DRV_VERSION);
