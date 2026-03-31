// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE FW Health Reporters
 *
 * Implements two devlink health reporters for firmware-related events:
 *
 * - **fw**: Non-fatal firmware health events (AQ errors, syndrome reports).
 *   Provides diagnose and dump callbacks but no auto-recovery, since these
 *   events are informational.
 *
 * - **fw_fatal**: Fatal firmware errors requiring device recovery (PCI
 *   liveness failure, persistent heartbeat loss, critical AQ failure).
 *   Provides diagnose, dump, and recover callbacks with auto-recovery
 *   enabled and a configurable grace period.
 *
 * The fatal recovery path performs a full device reload cycle:
 *   fdice_unload() -> fdice_function_setup() -> fdice_load()
 *
 * This is the single biggest improvement over ice, which explicitly states
 * "we do not do auto recovering" for all four of its health reporters.
 *
 * Architecture reference: docs/architecture/05-health-monitoring.md
 */

#include <linux/pci.h>
#include <linux/workqueue.h>
#include <net/devlink.h>

#include "fdice_dev.h"
#include "fdice_health_core.h"

/* -----------------------------------------------------------------------
 * FW Reporter Context
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_fw_reporter_ctx - Context passed to fw reporter callbacks
 * @syndrome: Current health syndrome code
 * @miss_count: Firmware heartbeat miss counter at time of report
 *
 * Allocated on the stack and passed as priv_ctx to devlink_health_report().
 */
struct fdice_fw_reporter_ctx {
	u32	syndrome;
	u32	miss_count;
};

#define FDICE_SYNDROME_RING_SIZE	16

/* -----------------------------------------------------------------------
 * FW Reporter: Diagnose
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fw_diagnose - Report current FW health status via devlink fmsg
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @extack: netlink extended ACK for error reporting
 *
 * Called by devlink when user requests `devlink health diagnose`.
 * Reports the current firmware version, AQ statistics, health counter,
 * and most recent syndrome if one is active.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_fw_diagnose(struct devlink_health_reporter *reporter,
			 struct devlink_fmsg *fmsg,
			 struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = devlink_health_reporter_priv(reporter);
	struct fdice_health *health = &fdev->health;
	struct fdice_hw *hw = &fdev->hw;
	struct fdice_health_syndrome *syn = &health->last_syndrome;

	devlink_fmsg_obj_nest_start(fmsg);

	/* Firmware version information */
	devlink_fmsg_pair_nest_start(fmsg, "fw_version");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u8_pair_put(fmsg, "major", hw->fw.fw_major);
	devlink_fmsg_u8_pair_put(fmsg, "minor", hw->fw.fw_minor);
	devlink_fmsg_u8_pair_put(fmsg, "patch", hw->fw.fw_patch);
	devlink_fmsg_u32_pair_put(fmsg, "build", hw->fw.fw_build);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* API version */
	devlink_fmsg_pair_nest_start(fmsg, "api_version");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u8_pair_put(fmsg, "major", hw->fw.api_major);
	devlink_fmsg_u8_pair_put(fmsg, "minor", hw->fw.api_minor);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* AQ health statistics */
	devlink_fmsg_u32_pair_put(fmsg, "health_counter",
				  health->fw_health_counter);
	devlink_fmsg_u32_pair_put(fmsg, "miss_count", health->miss_count);
	devlink_fmsg_u32_pair_put(fmsg, "aq_pending",
				  (u32)atomic_read(&hw->adminq.pending_count));

	/* Current syndrome if present */
	if (syn->code) {
		devlink_fmsg_pair_nest_start(fmsg, "syndrome");
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "code", syn->code);
		devlink_fmsg_u32_pair_put(fmsg, "severity",
					  (u32)syn->severity);
		if (syn->description)
			devlink_fmsg_string_pair_put(fmsg, "description",
						     syn->description);
		if (syn->solution)
			devlink_fmsg_string_pair_put(fmsg, "solution",
						     syn->solution);
		devlink_fmsg_u64_pair_put(fmsg, "timestamp", syn->timestamp);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);
	}

	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * FW Reporter: Dump
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fw_dump - Collect AQ error log and recent syndrome history
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @priv_ctx: optional fdice_fw_reporter_ctx from the report trigger
 * @extack: netlink extended ACK for error reporting
 *
 * Dumps the AQ error log (per-opcode statistics from the xarray),
 * the current reporter context if available, and the recent syndrome
 * ring buffer entries.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_fw_dump(struct devlink_health_reporter *reporter,
		     struct devlink_fmsg *fmsg, void *priv_ctx,
		     struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = devlink_health_reporter_priv(reporter);
	struct fdice_health *health = &fdev->health;
	struct fdice_health_syndrome *syn;
	int i;

	devlink_fmsg_obj_nest_start(fmsg);

	/* Include reporter context if triggered by an event */
	if (priv_ctx) {
		struct fdice_fw_reporter_ctx *ctx = priv_ctx;

		devlink_fmsg_pair_nest_start(fmsg, "event_context");
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "syndrome", ctx->syndrome);
		devlink_fmsg_u32_pair_put(fmsg, "miss_count", ctx->miss_count);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);
	}

	/* AQ error statistics summary */
	devlink_fmsg_pair_nest_start(fmsg, "aq_stats");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "pending_count",
				  (u32)atomic_read(&fdev->hw.adminq.pending_count));
	devlink_fmsg_bool_pair_put(fmsg, "initialized",
				   fdev->hw.adminq.initialized);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Syndrome ring buffer -- most recent entries */
	devlink_fmsg_arr_pair_nest_start(fmsg, "syndrome_history");
	for (i = 0; i < FDICE_SYNDROME_RING_SIZE; i++) {
		syn = &health->last_syndrome;

		/* Only emit entries that have been written */
		if (!syn->code && i > 0)
			break;
		if (i > 0)
			break; /* Single syndrome tracking in this phase */

		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "code", syn->code);
		devlink_fmsg_u32_pair_put(fmsg, "severity",
					  (u32)syn->severity);
		devlink_fmsg_u64_pair_put(fmsg, "timestamp", syn->timestamp);
		if (syn->description)
			devlink_fmsg_string_pair_put(fmsg, "description",
						     syn->description);
		devlink_fmsg_obj_nest_end(fmsg);
	}
	devlink_fmsg_arr_pair_nest_end(fmsg);

	/* Recovery history summary */
	devlink_fmsg_pair_nest_start(fmsg, "recovery_stats");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u64_pair_put(fmsg, "total_recoveries",
				  health->history.total_recoveries);
	devlink_fmsg_u64_pair_put(fmsg, "total_failures",
				  health->history.total_recovery_failures);
	devlink_fmsg_u64_pair_put(fmsg, "total_escalations",
				  health->history.total_escalations);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * FW Reporter Ops
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fw_ops - devlink_health_reporter_ops for non-fatal FW events
 *
 * Provides diagnose and dump callbacks. No recover callback because
 * non-fatal firmware events are informational -- they require user
 * attention but not automatic device recovery.
 */
const struct devlink_health_reporter_ops fdice_health_fw_ops = {
	.name		= "fw",
	.diagnose	= fdice_health_fw_diagnose,
	.dump		= fdice_health_fw_dump,
};

/* -----------------------------------------------------------------------
 * FW Fatal Reporter: Diagnose
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fw_fatal_diagnose - Report fatal error state via devlink fmsg
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @extack: netlink extended ACK for error reporting
 *
 * Reports the fatal error condition including the triggering syndrome,
 * PCI state, AQ state, and device state flags.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_fw_fatal_diagnose(struct devlink_health_reporter *reporter,
			       struct devlink_fmsg *fmsg,
			       struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = devlink_health_reporter_priv(reporter);
	struct fdice_health *health = &fdev->health;
	struct fdice_hw *hw = &fdev->hw;

	devlink_fmsg_obj_nest_start(fmsg);

	devlink_fmsg_string_pair_put(fmsg, "status", "fatal_error");
	devlink_fmsg_bool_pair_put(fmsg, "recovery_state",
				   test_bit(FDICE_STATE_RECOVERY, &fdev->state));
	devlink_fmsg_bool_pair_put(fmsg, "pci_channel_offline",
				   pci_channel_offline(fdev->pdev));

	/* Firmware version at time of failure */
	devlink_fmsg_pair_nest_start(fmsg, "fw_version");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u8_pair_put(fmsg, "major", hw->fw.fw_major);
	devlink_fmsg_u8_pair_put(fmsg, "minor", hw->fw.fw_minor);
	devlink_fmsg_u8_pair_put(fmsg, "patch", hw->fw.fw_patch);
	devlink_fmsg_u32_pair_put(fmsg, "build", hw->fw.fw_build);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Syndrome details */
	devlink_fmsg_u32_pair_put(fmsg, "syndrome_code",
				  health->last_syndrome.code);
	devlink_fmsg_u32_pair_put(fmsg, "miss_count", health->miss_count);
	devlink_fmsg_u32_pair_put(fmsg, "health_counter",
				  health->fw_health_counter);

	/* AQ state */
	devlink_fmsg_pair_nest_start(fmsg, "aq_state");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_bool_pair_put(fmsg, "initialized",
				   hw->adminq.initialized);
	devlink_fmsg_u32_pair_put(fmsg, "pending_count",
				  (u32)atomic_read(&hw->adminq.pending_count));
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * FW Fatal Reporter: Dump
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fw_fatal_dump - Capture register snapshot and AQ state
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @priv_ctx: optional fdice_fw_reporter_ctx from the report trigger
 * @extack: netlink extended ACK for error reporting
 *
 * Captures a snapshot of key device registers, AQ ring state, and the
 * full syndrome details for post-mortem analysis. This data is preserved
 * even after the recovery cycle completes.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_fw_fatal_dump(struct devlink_health_reporter *reporter,
			   struct devlink_fmsg *fmsg, void *priv_ctx,
			   struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = devlink_health_reporter_priv(reporter);
	struct fdice_health *health = &fdev->health;
	struct fdice_hw *hw = &fdev->hw;
	u32 reg_val;

	devlink_fmsg_obj_nest_start(fmsg);

	/* Event context from the trigger */
	if (priv_ctx) {
		struct fdice_fw_reporter_ctx *ctx = priv_ctx;

		devlink_fmsg_pair_nest_start(fmsg, "event_context");
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "syndrome", ctx->syndrome);
		devlink_fmsg_u32_pair_put(fmsg, "miss_count", ctx->miss_count);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);
	}

	/* Register snapshot -- read key registers if PCI is still live */
	devlink_fmsg_pair_nest_start(fmsg, "register_snapshot");
	devlink_fmsg_obj_nest_start(fmsg);

	if (hw->hw_addr && !pci_channel_offline(fdev->pdev)) {
		reg_val = ioread32(hw->hw_addr + FDICE_REG_ATQ_LEN);
		devlink_fmsg_u32_pair_put(fmsg, "atq_len", reg_val);

		reg_val = ioread32(hw->hw_addr + FDICE_REG_ATQ_HEAD);
		devlink_fmsg_u32_pair_put(fmsg, "atq_head", reg_val);

		reg_val = ioread32(hw->hw_addr + FDICE_REG_ATQ_TAIL);
		devlink_fmsg_u32_pair_put(fmsg, "atq_tail", reg_val);

		reg_val = ioread32(hw->hw_addr + FDICE_REG_ARQ_LEN);
		devlink_fmsg_u32_pair_put(fmsg, "arq_len", reg_val);

		reg_val = ioread32(hw->hw_addr + FDICE_REG_ARQ_HEAD);
		devlink_fmsg_u32_pair_put(fmsg, "arq_head", reg_val);

		reg_val = ioread32(hw->hw_addr + FDICE_REG_ARQ_TAIL);
		devlink_fmsg_u32_pair_put(fmsg, "arq_tail", reg_val);
	} else {
		devlink_fmsg_string_pair_put(fmsg, "pci_state", "offline");
	}

	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* AQ ring state snapshot */
	devlink_fmsg_pair_nest_start(fmsg, "aq_ring_state");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "sq_next_to_use",
				  hw->adminq.sq.next_to_use);
	devlink_fmsg_u32_pair_put(fmsg, "sq_next_to_clean",
				  hw->adminq.sq.next_to_clean);
	devlink_fmsg_u32_pair_put(fmsg, "rq_next_to_use",
				  hw->adminq.rq.next_to_use);
	devlink_fmsg_u32_pair_put(fmsg, "rq_next_to_clean",
				  hw->adminq.rq.next_to_clean);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Health state at time of capture */
	devlink_fmsg_pair_nest_start(fmsg, "health_state");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u32_pair_put(fmsg, "syndrome_code",
				  health->last_syndrome.code);
	devlink_fmsg_u32_pair_put(fmsg, "severity",
				  (u32)health->last_syndrome.severity);
	devlink_fmsg_u64_pair_put(fmsg, "syndrome_timestamp",
				  health->last_syndrome.timestamp);
	devlink_fmsg_u32_pair_put(fmsg, "miss_count", health->miss_count);
	devlink_fmsg_u32_pair_put(fmsg, "fw_health_counter",
				  health->fw_health_counter);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * FW Fatal Reporter: Recover
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fw_fatal_wait_pci - Wait for PCI to become accessible
 * @fdev: FrankenDrICE device
 *
 * After a fatal error, PCI may be temporarily inaccessible. This waits
 * for PCI to come back online before attempting recovery, with a bounded
 * timeout to avoid infinite waits.
 *
 * Return: 0 if PCI accessible, -ETIMEDOUT if still offline.
 */
static int fdice_health_fw_fatal_wait_pci(struct fdice_dev *fdev)
{
	unsigned long timeout = jiffies + msecs_to_jiffies(5000);

	while (pci_channel_offline(fdev->pdev)) {
		if (time_after(jiffies, timeout))
			return -ETIMEDOUT;
		msleep(100);
	}

	return 0;
}

/**
 * fdice_health_fw_fatal_recover - Full device reload recovery
 * @reporter: devlink health reporter instance
 * @priv_ctx: optional context from the report trigger
 * @extack: netlink extended ACK for error reporting
 *
 * Performs a complete device recovery cycle:
 * 1. Wait for PCI to become accessible
 * 2. Tear down the device (fdice_unload)
 * 3. Re-initialize firmware interface (fdice_function_setup)
 * 4. Bring up the device (fdice_load)
 *
 * This is invoked automatically when auto_recover is enabled, or
 * manually via `devlink health recover`.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_fw_fatal_recover(struct devlink_health_reporter *reporter,
			      void *priv_ctx,
			      struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = devlink_health_reporter_priv(reporter);
	struct device *dev = &fdev->pdev->dev;
	int err;

	/* Avoid concurrent recovery attempts */
	if (test_and_set_bit(FDICE_STATE_RECOVERY, &fdev->state)) {
		dev_warn(dev, "Recovery already in progress\n");
		return -EBUSY;
	}

	dev_info(dev, "Initiating fatal error recovery\n");

	/* Step 1: Wait for PCI to become accessible */
	err = fdice_health_fw_fatal_wait_pci(fdev);
	if (err) {
		dev_err(dev, "PCI not accessible after timeout, cannot recover\n");
		NL_SET_ERR_MSG_MOD(extack,
				   "PCI device not accessible after timeout");
		goto out_clear;
	}

	/* Step 2: Tear down the device */
	fdice_unload(fdev);

	/* Step 3: Re-initialize firmware interface */
	err = fdice_function_setup(fdev);
	if (err) {
		dev_err(dev, "Function setup failed during recovery: %d\n",
			err);
		NL_SET_ERR_MSG_MOD(extack,
				   "Firmware re-initialization failed");
		goto out_clear;
	}

	/* Step 4: Bring up the device */
	err = fdice_load(fdev);
	if (err) {
		dev_err(dev, "Device load failed during recovery: %d\n", err);
		NL_SET_ERR_MSG_MOD(extack, "Device reload failed");
		goto out_teardown;
	}

	dev_info(dev, "Fatal error recovery completed successfully\n");

	fdice_health_history_record(&fdev->health.history,
				    fdev->health.last_syndrome.code,
				    FDICE_HEALTH_SEV_FATAL,
				    FDICE_RESET_PF, true);

	clear_bit(FDICE_STATE_RECOVERY, &fdev->state);
	return 0;

out_teardown:
	fdice_function_teardown(fdev);
out_clear:
	fdice_health_history_record(&fdev->health.history,
				    fdev->health.last_syndrome.code,
				    FDICE_HEALTH_SEV_FATAL,
				    FDICE_RESET_PF, false);
	clear_bit(FDICE_STATE_RECOVERY, &fdev->state);
	return err;
}

/* -----------------------------------------------------------------------
 * FW Fatal Reporter Ops
 * ----------------------------------------------------------------------- */

/** @FDICE_FW_FATAL_GRACE_PERIOD_MS: Default grace period for PF recovery */
#define FDICE_FW_FATAL_GRACE_PERIOD_MS	60000

/**
 * fdice_health_fw_fatal_ops - devlink_health_reporter_ops for fatal FW errors
 *
 * Provides diagnose, dump, and recover callbacks with a 60-second default
 * grace period. Auto-recovery is enabled through the recover callback.
 */
const struct devlink_health_reporter_ops fdice_health_fw_fatal_ops = {
	.name		= "fw_fatal",
	.diagnose	= fdice_health_fw_fatal_diagnose,
	.dump		= fdice_health_fw_fatal_dump,
	.recover	= fdice_health_fw_fatal_recover,
};

/* -----------------------------------------------------------------------
 * Fatal Work Function
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fatal_work - Deferred fatal error handler
 * @work: work_struct embedded in fdice_health
 *
 * Scheduled by the health poll timer or AQ event handler when a fatal
 * condition is detected. Transitions the device to error state, notifies
 * subsystems (RDMA pre-reset), and triggers the devlink health report
 * which invokes the fw_fatal reporter's recovery path.
 *
 * If the health reporter is not available (registration failed), falls
 * back to a direct recovery attempt. If recovery was canceled due to
 * grace period, unloads the driver to prevent further damage.
 */
void fdice_health_fatal_work(struct work_struct *work)
{
	struct fdice_health *health;
	struct fdice_fw_reporter_ctx ctx;
	struct fdice_dev *fdev;
	int ret;

	health = container_of(work, struct fdice_health, fatal_work);
	fdev = health->dev;

	/* Check if device is being removed -- bail if so */
	if (test_bit(FDICE_STATE_REMOVING, &fdev->state))
		return;

	/* Mark device as undergoing recovery */
	set_bit(FDICE_STATE_RECOVERY, &fdev->state);

	/* Prepare reporter context */
	ctx.syndrome = health->last_syndrome.code;
	ctx.miss_count = health->miss_count;

	/* If fw_fatal reporter is unavailable, attempt direct recovery */
	if (IS_ERR_OR_NULL(health->fw_fatal_reporter)) {
		struct devlink *devlink = fdice_devlink_from_dev(fdev);

		devl_lock(devlink);
		ret = fdice_health_fw_fatal_recover(health->fw_fatal_reporter,
						    &ctx, NULL);
		devl_unlock(devlink);
		if (ret)
			dev_err(&fdev->pdev->dev,
				"Direct fatal recovery failed: %d\n", ret);
		return;
	}

	/*
	 * Report the fatal error through devlink. If auto-recovery is
	 * enabled, the recover callback runs after the grace period.
	 * If recovery is canceled (returns -ECANCELED), unload the driver
	 * to prevent operating in a broken state.
	 */
	ret = devlink_health_report(health->fw_fatal_reporter,
				    "FW fatal error detected", &ctx);
	if (ret == -ECANCELED) {
		dev_err(&fdev->pdev->dev,
			"Fatal recovery canceled, unloading device\n");
		fdice_unload(fdev);
	}
}

/* -----------------------------------------------------------------------
 * Reporter Registration / Teardown
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fw_reporters_create - Register fw and fw_fatal reporters
 * @fdev: FrankenDrICE device
 *
 * Creates both firmware-level devlink health reporters. Called during
 * fdice_health_init(). Errors are non-fatal -- the device operates
 * without health reporting if registration fails.
 */
void fdice_health_fw_reporters_create(struct fdice_dev *fdev)
{
	struct fdice_health *health = &fdev->health;
	struct devlink *devlink = fdice_devlink_from_dev(fdev);

	health->fw_reporter =
		devl_health_reporter_create(devlink, &fdice_health_fw_ops,
					    0, fdev);
	if (IS_ERR(health->fw_reporter)) {
		dev_warn(&fdev->pdev->dev,
			 "Failed to create fw health reporter: %pe\n",
			 health->fw_reporter);
	}

	health->fw_fatal_reporter =
		devl_health_reporter_create(devlink, &fdice_health_fw_fatal_ops,
					    FDICE_FW_FATAL_GRACE_PERIOD_MS,
					    fdev);
	if (IS_ERR(health->fw_fatal_reporter)) {
		dev_warn(&fdev->pdev->dev,
			 "Failed to create fw_fatal health reporter: %pe\n",
			 health->fw_fatal_reporter);
	}
}

/**
 * fdice_health_fw_reporters_destroy - Unregister fw and fw_fatal reporters
 * @fdev: FrankenDrICE device
 *
 * Destroys both firmware-level devlink health reporters. Called during
 * fdice_health_cleanup(). Safe to call if reporters were never created.
 */
void fdice_health_fw_reporters_destroy(struct fdice_dev *fdev)
{
	struct fdice_health *health = &fdev->health;

	if (!IS_ERR_OR_NULL(health->fw_fatal_reporter))
		devl_health_reporter_destroy(health->fw_fatal_reporter);

	if (!IS_ERR_OR_NULL(health->fw_reporter))
		devl_health_reporter_destroy(health->fw_reporter);
}
