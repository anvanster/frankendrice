// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE Health Monitoring Core
 *
 * Implements the core health monitoring subsystem: timer-based hardware
 * polling with jitter, syndrome-based diagnostics, error history tracking
 * with ring buffer, and reset escalation policy.
 *
 * This is Layer 1 (hardware health buffer polling), Layer 2 (syndrome
 * diagnostics), and Layer 5 (error history / escalation) from the
 * six-layer health architecture.
 *
 * Devlink health reporter callbacks (Layers 3/6) are in separate files:
 *   fdice_health_fw.c   -- fw + fw_fatal reporters
 *   fdice_health_tx.c   -- TX per-queue reporter
 *   fdice_health_rx.c   -- RX per-queue reporter
 *   fdice_health_port.c -- port reporter
 *
 * Architecture reference: docs/architecture/05-health-monitoring.md
 */

#include <linux/module.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/jiffies.h>
#include <linux/random.h>
#include <linux/ktime.h>
#include <net/devlink.h>

#include "fdice_dev.h"
#include "fdice_health_core.h"

/* -----------------------------------------------------------------------
 * Syndrome Lookup Table
 *
 * Covers E810-specific health codes derived from studying ice's firmware
 * health status processing, augmented with hardware sensor conditions.
 * Each entry provides a human-readable description and remediation hint.
 * ----------------------------------------------------------------------- */

static const struct fdice_syndrome_entry fdice_syndrome_table[] = {
	/* Module errors (0x0001 - 0x000F) */
	{
		.code = 0x0001,
		.severity = FDICE_HEALTH_SEV_WARNING,
		.description = "Unsupported module type inserted",
		.solution = "Replace with a qualified SFP/QSFP module",
	},
	{
		.code = 0x0002,
		.severity = FDICE_HEALTH_SEV_WARNING,
		.description = "Module type does not match port configuration",
		.solution = "Insert correct module type for port speed/type",
	},
	{
		.code = 0x0003,
		.severity = FDICE_HEALTH_SEV_NOTICE,
		.description = "Module not qualified for this adapter",
		.solution = "Use a vendor-qualified transceiver module",
	},
	{
		.code = 0x0004,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "Module communication failure (I2C/MDIO)",
		.solution = "Reseat the module; replace if error persists",
	},
	{
		.code = 0x0005,
		.severity = FDICE_HEALTH_SEV_WARNING,
		.description = "Module over-temperature condition",
		.solution = "Check airflow and ambient temperature",
	},

	/* Link errors (0x0010 - 0x001F) */
	{
		.code = 0x0010,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "Invalid link configuration requested",
		.solution = "Verify link settings match remote end",
	},
	{
		.code = 0x0011,
		.severity = FDICE_HEALTH_SEV_WARNING,
		.description = "Parallel detect fault on link partner",
		.solution = "Check cable and link partner configuration",
	},
	{
		.code = 0x0012,
		.severity = FDICE_HEALTH_SEV_NOTICE,
		.description = "PHY capability limited by module type",
		.solution = "Use a module supporting desired link speed",
	},
	{
		.code = 0x0013,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "Link training failure",
		.solution = "Check cable quality and length; verify FEC settings",
	},
	{
		.code = 0x0014,
		.severity = FDICE_HEALTH_SEV_WARNING,
		.description = "Excessive link flapping detected",
		.solution = "Inspect cable connections and switch port",
	},
	{
		.code = 0x0015,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "Remote fault received from link partner",
		.solution = "Check link partner status and cable integrity",
	},

	/* Netlist errors (0x0020 - 0x002F) */
	{
		.code = 0x0020,
		.severity = FDICE_HEALTH_SEV_CRITICAL,
		.description = "Netlist topology file corrupted",
		.solution = "Reflash NVM with valid firmware image",
	},
	{
		.code = 0x0021,
		.severity = FDICE_HEALTH_SEV_FATAL,
		.description = "Unrecoverable netlist error",
		.solution = "RMA the adapter; NVM integrity compromised",
	},

	/* Flash/NVM errors (0x0030 - 0x003F) */
	{
		.code = 0x0030,
		.severity = FDICE_HEALTH_SEV_CRITICAL,
		.description = "Flash/NVM access failure",
		.solution = "Retry firmware update; check for NVM corruption",
	},
	{
		.code = 0x0031,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "NVM authentication failure",
		.solution = "Use a properly signed firmware image",
	},
	{
		.code = 0x0032,
		.severity = FDICE_HEALTH_SEV_WARNING,
		.description = "NVM image version rollback attempted",
		.solution = "Use a firmware image with equal or higher version",
	},

	/* DDP/OROM errors (0x0040 - 0x004F) */
	{
		.code = 0x0040,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "DDP package authentication failure",
		.solution = "Load a properly signed DDP package",
	},
	{
		.code = 0x0041,
		.severity = FDICE_HEALTH_SEV_WARNING,
		.description = "DDP package incompatible with firmware",
		.solution = "Update DDP package to match firmware version",
	},
	{
		.code = 0x0042,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "Option ROM authentication failure",
		.solution = "Reflash with a valid OROM image",
	},

	/* Sensor errors (0x1000 - 0x100F) */
	{
		.code = 0x1000,
		.severity = FDICE_HEALTH_SEV_FATAL,
		.description = "PCI device not accessible (0xFFFFFFFF read)",
		.solution = "Check PCIe slot seating; may require host reboot",
	},
	{
		.code = 0x1001,
		.severity = FDICE_HEALTH_SEV_CRITICAL,
		.description = "Firmware heartbeat lost",
		.solution = "Firmware may be hung; device recovery in progress",
	},
	{
		.code = 0x1002,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "Admin Queue not responding",
		.solution = "AQ may be wedged; device recovery in progress",
	},

	/* Data path errors (0x2000 - 0x200F) */
	{
		.code = 0x2000,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "TX queue timeout",
		.solution = "Per-queue recovery in progress",
	},
	{
		.code = 0x2001,
		.severity = FDICE_HEALTH_SEV_ERROR,
		.description = "RX queue stall detected",
		.solution = "Per-queue recovery in progress",
	},
	{
		.code = 0x2002,
		.severity = FDICE_HEALTH_SEV_WARNING,
		.description = "Completion queue overrun",
		.solution = "Check interrupt coalescing and NAPI budget",
	},
};

/**
 * fdice_syndrome_lookup - Look up a syndrome code in the diagnostic table
 * @code: E810 health status code
 *
 * Performs a linear search of the syndrome table. The table is small
 * enough (~30 entries) that binary search is not warranted.
 *
 * Return: Pointer to the matching entry, or NULL if the code is unknown.
 */
const struct fdice_syndrome_entry *fdice_syndrome_lookup(u32 code)
{
	unsigned int i;

	for (i = 0; i < ARRAY_SIZE(fdice_syndrome_table); i++) {
		if (fdice_syndrome_table[i].code == code)
			return &fdice_syndrome_table[i];
	}

	return NULL;
}

/* -----------------------------------------------------------------------
 * History Ring Buffer
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_history_init - Initialize the health history ring buffer
 * @history: history structure to initialize
 */
static void fdice_health_history_init(struct fdice_health_history *history)
{
	memset(history, 0, sizeof(*history));
	spin_lock_init(&history->lock);
	history->window_start = jiffies;
}

/**
 * fdice_health_history_record - Record a health event in the history ring
 * @history: history tracking structure
 * @code: syndrome code of the event
 * @severity: severity of the event
 * @recovery_type: what recovery was attempted
 * @success: whether recovery succeeded
 *
 * Appends an event to the ring buffer under spinlock protection.
 * Updates lifetime statistics and escalation window counters.
 */
void fdice_health_history_record(struct fdice_health_history *history,
				 u32 code,
				 enum fdice_health_severity severity,
				 enum fdice_reset_type recovery_type,
				 bool success)
{
	struct fdice_health_event *evt;
	unsigned long flags;

	spin_lock_irqsave(&history->lock, flags);

	evt = &history->events[history->head];
	evt->timestamp = ktime_get_ns();
	evt->syndrome_code = code;
	evt->severity = severity;
	evt->recovery_type = recovery_type;
	evt->recovery_success = success;

	history->head = (history->head + 1) % FDICE_HEALTH_HISTORY_SIZE;
	if (history->count < FDICE_HEALTH_HISTORY_SIZE)
		history->count++;

	/* Update lifetime stats */
	if (success)
		history->total_recoveries++;
	else
		history->total_recovery_failures++;

	/* Update escalation window counters */
	switch (recovery_type) {
	case FDICE_RESET_QUEUE:
		history->queue_recoveries++;
		break;
	case FDICE_RESET_CHANNEL:
		history->channel_recoveries++;
		break;
	case FDICE_RESET_PF:
		history->pf_resets++;
		break;
	case FDICE_RESET_CORE:
		history->core_resets++;
		break;
	case FDICE_RESET_GLOBAL:
		/* Global resets are terminal; no further escalation */
		break;
	}

	spin_unlock_irqrestore(&history->lock, flags);
}

/**
 * fdice_health_window_refresh - Reset escalation window if expired
 * @history: history tracking structure
 *
 * Called under history->lock. If the escalation window has elapsed,
 * resets all per-window counters and advances the window start.
 */
static void fdice_health_window_refresh(struct fdice_health_history *history)
{
	unsigned long window_jiffies;

	window_jiffies = msecs_to_jiffies(FDICE_HEALTH_ESCALATION_WINDOW_S *
					  1000);

	if (time_after(jiffies, history->window_start + window_jiffies)) {
		history->queue_recoveries = 0;
		history->channel_recoveries = 0;
		history->pf_resets = 0;
		history->core_resets = 0;
		history->window_start = jiffies;
	}
}

/**
 * fdice_health_should_escalate - Check if escalation threshold is exceeded
 * @history: history tracking structure
 * @current_level: current recovery level being attempted
 *
 * Examines the escalation counters within the current time window to
 * determine if too many recoveries at @current_level have occurred,
 * warranting escalation to the next level.
 *
 * Return: true if escalation should occur, false otherwise.
 */
bool fdice_health_should_escalate(struct fdice_health_history *history,
				  enum fdice_reset_type current_level)
{
	bool escalate = false;
	unsigned long flags;

	spin_lock_irqsave(&history->lock, flags);

	fdice_health_window_refresh(history);

	switch (current_level) {
	case FDICE_RESET_QUEUE:
		escalate = history->queue_recoveries >=
			   FDICE_HEALTH_QUEUE_ESCALATE;
		break;
	case FDICE_RESET_CHANNEL:
		escalate = history->channel_recoveries >=
			   FDICE_HEALTH_CHANNEL_ESCALATE;
		break;
	case FDICE_RESET_PF:
		escalate = history->pf_resets >= FDICE_HEALTH_PF_ESCALATE;
		break;
	case FDICE_RESET_CORE:
		/* >1 core reset in window -> escalate to global */
		escalate = history->core_resets > 1;
		break;
	case FDICE_RESET_GLOBAL:
		/* Cannot escalate beyond global */
		escalate = false;
		break;
	}

	if (escalate)
		history->total_escalations++;

	spin_unlock_irqrestore(&history->lock, flags);

	return escalate;
}

/* -----------------------------------------------------------------------
 * Hardware Sensor Checks
 * ----------------------------------------------------------------------- */

/**
 * fdice_sensor_pci_liveness - Check if PCI device is accessible
 * @health: health monitoring state
 *
 * Reads a well-known register via MMIO. A return value of 0xFFFFFFFF
 * indicates the device has been removed or the PCIe link is down.
 *
 * Return: true if device is alive, false if PCI is dead.
 */
static bool fdice_sensor_pci_liveness(struct fdice_health *health)
{
	struct fdice_hw *hw = &health->dev->hw;
	u32 val;

	if (!hw->hw_addr)
		return false;

	val = ioread32(hw->hw_addr + FDICE_REG_ATQ_LEN);

	return val != FDICE_HEALTH_PCI_DEAD_VALUE;
}

/**
 * fdice_sensor_fw_heartbeat - Check firmware heartbeat counter
 * @health: health monitoring state
 *
 * Reads the current firmware health counter and compares to the
 * cached value. If unchanged, increments the miss counter. Three
 * consecutive misses indicate a firmware hang.
 *
 * Return: true if heartbeat is healthy, false if missed.
 */
static bool fdice_sensor_fw_heartbeat(struct fdice_health *health)
{
	struct fdice_hw *hw = &health->dev->hw;
	u32 counter;

	if (!hw->hw_addr)
		return false;

	/*
	 * The E810 firmware heartbeat is exposed via a health status
	 * register. We read it via MMIO rather than AQ to avoid
	 * depending on the AQ being functional for health checks.
	 * Using ATQ_HEAD as a proxy for FW liveness -- if FW is
	 * processing commands, this register changes.
	 */
	counter = ioread32(hw->hw_addr + FDICE_REG_ATQ_HEAD);

	if (counter == health->fw_health_counter) {
		health->miss_count++;
		return health->miss_count < FDICE_HEALTH_MISS_THRESHOLD;
	}

	health->fw_health_counter = counter;
	health->miss_count = 0;

	return true;
}

/**
 * fdice_sensor_aq_responsive - Check if admin queue is responsive
 * @health: health monitoring state
 *
 * Verifies the AQ is operational by checking for overflow or critical
 * error flags in the queue length register.
 *
 * Return: true if AQ appears responsive, false otherwise.
 */
static bool fdice_sensor_aq_responsive(struct fdice_health *health)
{
	struct fdice_hw *hw = &health->dev->hw;
	u32 len_reg;

	if (!hw->hw_addr || !hw->aq_initialized)
		return true; /* Cannot check; assume OK before AQ init */

	len_reg = ioread32(hw->hw_addr + FDICE_REG_ATQ_LEN);

	/* Check for overflow or critical error flags */
	if (len_reg & (FDICE_REG_CTLQ_LEN_OVFL | FDICE_REG_CTLQ_LEN_CRIT))
		return false;

	return true;
}

/**
 * fdice_health_check_sensors - Run all hardware sensor checks
 * @health: health monitoring state
 *
 * Runs PCI liveness, firmware heartbeat, and AQ responsiveness checks
 * in sequence. Short-circuits on PCI death (no point checking FW or AQ
 * if the device is gone).
 *
 * Return: 0 if all sensors healthy, bitmask of FDICE_SENSOR_* flags otherwise.
 */
unsigned int fdice_health_check_sensors(struct fdice_health *health)
{
	unsigned int result = 0;

	if (!fdice_sensor_pci_liveness(health)) {
		result |= FDICE_SENSOR_PCI_DEAD;
		return result; /* No point checking further */
	}

	if (pci_channel_offline(health->dev->pdev)) {
		result |= FDICE_SENSOR_PCI_DEAD;
		return result;
	}

	if (!fdice_sensor_fw_heartbeat(health))
		result |= FDICE_SENSOR_FW_HEARTBEAT;

	if (!fdice_sensor_aq_responsive(health))
		result |= FDICE_SENSOR_AQ_TIMEOUT;

	return result;
}

/* -----------------------------------------------------------------------
 * Health Work Items
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_fatal_work_fn - Deferred handler for fatal health events
 * @work: work_struct embedded in fdice_health
 *
 * Processes fatal errors detected by the poll timer. Reports to the
 * fw_fatal devlink reporter and records the event in history.
 */
static void fdice_health_fatal_work_fn(struct work_struct *work)
{
	struct fdice_health *health;

	health = container_of(work, struct fdice_health, fatal_work);

	if (health->fw_fatal_reporter)
		devlink_health_report(health->fw_fatal_reporter,
				      "Fatal hardware error detected", health);

	fdice_health_history_record(&health->history, 0x1000,
				    FDICE_HEALTH_SEV_FATAL,
				    FDICE_RESET_PF, false);
}

/**
 * fdice_health_fw_work_fn - Deferred handler for non-fatal FW events
 * @work: work_struct embedded in fdice_health
 *
 * Processes non-fatal firmware health events (heartbeat misses,
 * AQ issues). Reports to the fw devlink reporter.
 */
static void fdice_health_fw_work_fn(struct work_struct *work)
{
	struct fdice_health *health;

	health = container_of(work, struct fdice_health, fw_work);

	if (health->fw_reporter)
		devlink_health_report(health->fw_reporter,
				      "Firmware health event", health);

	fdice_health_history_record(&health->history,
				    health->last_syndrome.code,
				    health->last_syndrome.severity,
				    FDICE_RESET_QUEUE, true);
}

/* -----------------------------------------------------------------------
 * Poll Timer
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_jitter - Compute a jittered poll interval
 * @base_ms: base interval in milliseconds
 * @jitter_ms: maximum jitter in milliseconds
 *
 * Adds a pseudo-random jitter to the base interval to avoid
 * thundering-herd effects when multiple functions poll simultaneously.
 *
 * Return: jittered interval in jiffies.
 */
static unsigned long fdice_health_jitter(unsigned int base_ms,
					 unsigned int jitter_ms)
{
	unsigned int jitter;

	get_random_bytes(&jitter, sizeof(jitter));
	jitter = jitter % (jitter_ms + 1);

	return msecs_to_jiffies(base_ms + jitter);
}

/**
 * fdice_health_poll_timer - Timer callback for periodic health checks
 * @t: timer_list embedded in fdice_health
 *
 * Core health polling loop. Checks all hardware sensors and schedules
 * appropriate work items for any detected issues. Re-arms the timer
 * with a jittered interval to prevent synchronization artifacts.
 */
void fdice_health_poll_timer(struct timer_list *t)
{
	struct fdice_health *health = container_of(t, struct fdice_health,
						   poll_timer);
	unsigned int sensor_status;

	if (test_bit(FDICE_STATE_REMOVING, &health->dev->state))
		return;

	sensor_status = fdice_health_check_sensors(health);

	if (sensor_status & FDICE_SENSOR_PCI_DEAD) {
		/* Fatal: PCI device is gone */
		health->last_syndrome.code = 0x1000;
		health->last_syndrome.severity = FDICE_HEALTH_SEV_FATAL;
		health->last_syndrome.timestamp = ktime_get_ns();
		queue_work(health->wq, &health->fatal_work);
		return; /* Do not re-arm; device is dead */
	}

	if (sensor_status & FDICE_SENSOR_FW_HEARTBEAT) {
		/* FW heartbeat missed threshold times */
		health->last_syndrome.code = 0x1001;
		health->last_syndrome.severity = FDICE_HEALTH_SEV_CRITICAL;
		health->last_syndrome.timestamp = ktime_get_ns();
		queue_work(health->wq, &health->fatal_work);
		return; /* Recovery will restart polling */
	}

	if (sensor_status & FDICE_SENSOR_AQ_TIMEOUT) {
		/* AQ not responsive -- non-fatal, report and continue */
		health->last_syndrome.code = 0x1002;
		health->last_syndrome.severity = FDICE_HEALTH_SEV_ERROR;
		health->last_syndrome.timestamp = ktime_get_ns();
		queue_work(health->wq, &health->fw_work);
	}

	/* Re-arm timer with jitter */
	health->poll_jiffies = fdice_health_jitter(FDICE_HEALTH_POLL_INTERVAL_MS,
						   FDICE_HEALTH_POLL_JITTER_MS);
	mod_timer(&health->poll_timer, jiffies + health->poll_jiffies);
}

/* -----------------------------------------------------------------------
 * Lifecycle: Init / Cleanup / Start / Stop
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_init - Initialize the health monitoring subsystem
 * @fdev: FrankenDrICE device
 *
 * Sets up the dedicated health workqueue, initializes the poll timer,
 * prepares the history ring buffer, and registers the three device-level
 * devlink health reporters (fw, fw_fatal, port).
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_health_init(struct fdice_dev *fdev)
{
	struct fdice_health *health = &fdev->health;
	struct devlink *devlink = fdice_devlink_from_dev(fdev);

	health->dev = fdev;

	/* Create a dedicated ordered workqueue for health processing */
	health->wq = alloc_ordered_workqueue("fdice_health_%s", WQ_MEM_RECLAIM,
					     pci_name(fdev->pdev));
	if (!health->wq)
		return -ENOMEM;

	/* Initialize work items */
	INIT_WORK(&health->fatal_work, fdice_health_fatal_work_fn);
	INIT_WORK(&health->fw_work, fdice_health_fw_work_fn);

	/* Initialize poll timer (not yet armed) */
	timer_setup(&health->poll_timer, fdice_health_poll_timer, 0);
	health->poll_jiffies = fdice_health_jitter(FDICE_HEALTH_POLL_INTERVAL_MS,
						   FDICE_HEALTH_POLL_JITTER_MS);

	/* Initialize history ring buffer */
	fdice_health_history_init(&health->history);

	/* Initialize syndrome state */
	memset(&health->last_syndrome, 0, sizeof(health->last_syndrome));
	health->fw_health_counter = 0;
	health->miss_count = 0;

	/* Configuration defaults */
	health->fatal_grace_period_ms = FDICE_HEALTH_FATAL_GRACE_MS;
	health->auto_recover_enabled = true;

	/* Register device-level devlink health reporters */
	health->fw_reporter =
		devlink_health_reporter_create(devlink, &fdice_health_fw_ops,
					       0, fdev);
	if (IS_ERR(health->fw_reporter)) {
		health->fw_reporter = NULL;
		goto err_destroy_wq;
	}

	health->fw_fatal_reporter =
		devlink_health_reporter_create(devlink,
					       &fdice_health_fw_fatal_ops,
					       health->fatal_grace_period_ms,
					       fdev);
	if (IS_ERR(health->fw_fatal_reporter)) {
		health->fw_fatal_reporter = NULL;
		goto err_destroy_fw;
	}

	health->port_reporter =
		devlink_health_reporter_create(devlink, &fdice_health_port_ops,
					       0, fdev);
	if (IS_ERR(health->port_reporter)) {
		health->port_reporter = NULL;
		goto err_destroy_fw_fatal;
	}

	return 0;

err_destroy_fw_fatal:
	devlink_health_reporter_destroy(health->fw_fatal_reporter);
	health->fw_fatal_reporter = NULL;
err_destroy_fw:
	devlink_health_reporter_destroy(health->fw_reporter);
	health->fw_reporter = NULL;
err_destroy_wq:
	destroy_workqueue(health->wq);
	health->wq = NULL;
	return -ENOMEM;
}

/**
 * fdice_health_cleanup - Tear down the health monitoring subsystem
 * @fdev: FrankenDrICE device
 *
 * Cancels the poll timer, flushes and destroys the health workqueue,
 * and unregisters all devlink health reporters. Safe to call even if
 * init was only partially completed.
 */
void fdice_health_cleanup(struct fdice_dev *fdev)
{
	struct fdice_health *health = &fdev->health;

	/* Stop the timer first */
	timer_delete_sync(&health->poll_timer);

	/* Flush and destroy the workqueue */
	if (health->wq) {
		flush_workqueue(health->wq);
		destroy_workqueue(health->wq);
		health->wq = NULL;
	}

	/* Unregister reporters in reverse order */
	if (health->port_reporter) {
		devlink_health_reporter_destroy(health->port_reporter);
		health->port_reporter = NULL;
	}

	if (health->fw_fatal_reporter) {
		devlink_health_reporter_destroy(health->fw_fatal_reporter);
		health->fw_fatal_reporter = NULL;
	}

	if (health->fw_reporter) {
		devlink_health_reporter_destroy(health->fw_reporter);
		health->fw_reporter = NULL;
	}
}

/**
 * fdice_health_start - Activate health polling
 * @fdev: FrankenDrICE device
 *
 * Arms the poll timer to begin periodic hardware health checks.
 * Called from fdice_load() after hardware is fully initialized.
 */
void fdice_health_start(struct fdice_dev *fdev)
{
	struct fdice_health *health = &fdev->health;

	health->miss_count = 0;
	health->fw_health_counter = 0;
	health->poll_jiffies = fdice_health_jitter(FDICE_HEALTH_POLL_INTERVAL_MS,
						   FDICE_HEALTH_POLL_JITTER_MS);
	mod_timer(&health->poll_timer, jiffies + health->poll_jiffies);
}

/**
 * fdice_health_stop - Deactivate health polling
 * @fdev: FrankenDrICE device
 *
 * Disarms the poll timer and flushes any pending health work items.
 * Called from fdice_unload() before hardware teardown begins.
 */
void fdice_health_stop(struct fdice_dev *fdev)
{
	struct fdice_health *health = &fdev->health;

	timer_delete_sync(&health->poll_timer);

	if (health->wq)
		flush_workqueue(health->wq);
}
