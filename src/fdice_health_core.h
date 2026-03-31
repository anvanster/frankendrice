// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_HEALTH_CORE_H_
#define _FDICE_HEALTH_CORE_H_

/**
 * DOC: FrankenDrICE Health Monitoring Core
 *
 * Master header for the six-layer health monitoring subsystem.
 * Provides hardware health polling, syndrome-based diagnostics,
 * devlink health reporters, per-queue data path recovery,
 * error history tracking, and reset escalation.
 *
 * Architecture reference: docs/architecture/05-health-monitoring.md
 */

#include <linux/types.h>
#include <linux/timer.h>
#include <linux/workqueue.h>
#include <linux/spinlock.h>
#include <net/devlink.h>

/* Forward declarations */
struct fdice_dev;
struct fdice_hw;
struct fdice_channel;

/* -----------------------------------------------------------------------
 * Health History Configuration
 * ----------------------------------------------------------------------- */

/** @FDICE_HEALTH_HISTORY_SIZE: Ring buffer capacity for health events */
#define FDICE_HEALTH_HISTORY_SIZE	64

/* Escalation thresholds within a time window (default 5 minutes) */

/** @FDICE_HEALTH_QUEUE_ESCALATE: Per-queue recoveries before channel escalation */
#define FDICE_HEALTH_QUEUE_ESCALATE	3

/** @FDICE_HEALTH_CHANNEL_ESCALATE: Channel recoveries before PF reset escalation */
#define FDICE_HEALTH_CHANNEL_ESCALATE	2

/** @FDICE_HEALTH_PF_ESCALATE: PF resets before core reset escalation */
#define FDICE_HEALTH_PF_ESCALATE	2

/** @FDICE_HEALTH_POLL_INTERVAL_MS: Base poll interval in milliseconds */
#define FDICE_HEALTH_POLL_INTERVAL_MS	2000

/** @FDICE_HEALTH_POLL_JITTER_MS: Maximum jitter added to poll interval */
#define FDICE_HEALTH_POLL_JITTER_MS	500

/** @FDICE_HEALTH_MISS_THRESHOLD: Consecutive heartbeat misses before report */
#define FDICE_HEALTH_MISS_THRESHOLD	3

/** @FDICE_HEALTH_ESCALATION_WINDOW_S: Escalation time window in seconds */
#define FDICE_HEALTH_ESCALATION_WINDOW_S 300

/** @FDICE_HEALTH_FATAL_GRACE_MS: Default fatal recovery grace period (PF) */
#define FDICE_HEALTH_FATAL_GRACE_MS	60000

/** @FDICE_HEALTH_PCI_DEAD_VALUE: PCI liveness check sentinel */
#define FDICE_HEALTH_PCI_DEAD_VALUE	0xFFFFFFFF

/* -----------------------------------------------------------------------
 * Severity Classification
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_health_severity - Health event severity levels
 * @FDICE_HEALTH_SEV_DEBUG: Informational only, not user-visible
 * @FDICE_HEALTH_SEV_INFO: Normal operational event
 * @FDICE_HEALTH_SEV_NOTICE: Noteworthy condition, no action needed
 * @FDICE_HEALTH_SEV_WARNING: Degraded but operational
 * @FDICE_HEALTH_SEV_ERROR: Recoverable error, auto-recovery attempted
 * @FDICE_HEALTH_SEV_CRITICAL: Service-affecting error
 * @FDICE_HEALTH_SEV_EMERGENCY: Immediate action required
 * @FDICE_HEALTH_SEV_FATAL: Unrecoverable, device reset needed
 *
 * Maps to kernel log levels for consistent severity classification.
 */
enum fdice_health_severity {
	FDICE_HEALTH_SEV_DEBUG = 0,
	FDICE_HEALTH_SEV_INFO,
	FDICE_HEALTH_SEV_NOTICE,
	FDICE_HEALTH_SEV_WARNING,
	FDICE_HEALTH_SEV_ERROR,
	FDICE_HEALTH_SEV_CRITICAL,
	FDICE_HEALTH_SEV_EMERGENCY,
	FDICE_HEALTH_SEV_FATAL,
};

/* -----------------------------------------------------------------------
 * Per-Queue Health State Machine
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_queue_health_state - Per-queue recovery state machine
 * @FDICE_QH_ACTIVE: Normal operation
 * @FDICE_QH_ERROR: Error detected, pending recovery
 * @FDICE_QH_RECOVERING: Recovery in progress
 * @FDICE_QH_READY: Recovery complete, resuming traffic
 * @FDICE_QH_FAILED: Recovery failed, escalate to next level
 *
 * State transitions: ACTIVE -> ERROR -> RECOVERING -> READY -> ACTIVE
 * On failure: RECOVERING -> FAILED (triggers escalation)
 */
enum fdice_queue_health_state {
	FDICE_QH_ACTIVE,
	FDICE_QH_ERROR,
	FDICE_QH_RECOVERING,
	FDICE_QH_READY,
	FDICE_QH_FAILED,
};

/* -----------------------------------------------------------------------
 * Reset Escalation Levels
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_reset_type - Recovery escalation levels
 * @FDICE_RESET_QUEUE: Per-queue disable/reset/enable via AQ
 * @FDICE_RESET_CHANNEL: Reopen the full channel (all queue pairs)
 * @FDICE_RESET_PF: PF-level reset (PFR)
 * @FDICE_RESET_CORE: Core reset (CORER)
 * @FDICE_RESET_GLOBAL: Global reset (GLOBR), device goes offline
 *
 * Each level is attempted before escalating to the next.
 * GLOBAL reset marks the device offline if it also fails.
 */
enum fdice_reset_type {
	FDICE_RESET_QUEUE,
	FDICE_RESET_CHANNEL,
	FDICE_RESET_PF,
	FDICE_RESET_CORE,
	FDICE_RESET_GLOBAL,
};

/* -----------------------------------------------------------------------
 * Syndrome Diagnostics
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_health_syndrome - Structured error diagnostic
 * @code: E810 health status code from firmware
 * @severity: Classified severity level
 * @description: Human-readable description of the condition
 * @solution: Suggested remediation action
 * @data_labels: Labels for the two diagnostic data fields
 * @data: Diagnostic data values from hardware
 * @timestamp: Kernel timestamp (ktime_get_ns) of detection
 *
 * Populated by fdice_syndrome_lookup() from the syndrome table
 * and enriched with runtime data from the health event.
 */
struct fdice_health_syndrome {
	u32				code;
	enum fdice_health_severity	severity;
	const char			*description;
	const char			*solution;
	const char			*data_labels[2];
	u32				data[2];
	u64				timestamp;
};

/**
 * struct fdice_syndrome_entry - Static syndrome lookup table entry
 * @code: E810 health status code
 * @severity: Default severity for this syndrome
 * @description: Human-readable description
 * @solution: Suggested remediation
 *
 * Used in the compile-time syndrome table; runtime events copy
 * relevant fields into struct fdice_health_syndrome.
 */
struct fdice_syndrome_entry {
	u32				code;
	enum fdice_health_severity	severity;
	const char			*description;
	const char			*solution;
};

/* -----------------------------------------------------------------------
 * Health Event and History Tracking
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_health_event - Single event record in history buffer
 * @timestamp: Kernel timestamp (ktime_get_ns) of the event
 * @syndrome_code: E810 health status code that triggered the event
 * @severity: Severity at time of detection
 * @recovery_type: What recovery level was attempted
 * @recovery_success: Whether recovery succeeded
 */
struct fdice_health_event {
	u64				timestamp;
	u32				syndrome_code;
	enum fdice_health_severity	severity;
	enum fdice_reset_type		recovery_type;
	bool				recovery_success;
};

/**
 * struct fdice_health_history - Error history with escalation tracking
 * @events: Ring buffer of recent health events
 * @head: Next write position in ring buffer
 * @count: Total events written (saturates at ring size for wrap detect)
 * @lock: Protects ring buffer and escalation counters
 * @queue_recoveries: Per-queue recoveries in current escalation window
 * @channel_recoveries: Channel recoveries in current escalation window
 * @pf_resets: PF resets in current escalation window
 * @core_resets: Core resets in current escalation window
 * @window_start: Jiffies timestamp of current escalation window start
 * @total_recoveries: Lifetime successful recovery count
 * @total_recovery_failures: Lifetime failed recovery count
 * @total_escalations: Lifetime escalation count
 * @recovery_time_sum_ns: Cumulative recovery time for average calculation
 */
struct fdice_health_history {
	struct fdice_health_event	events[FDICE_HEALTH_HISTORY_SIZE];
	unsigned int			head;
	unsigned int			count;
	spinlock_t			lock;

	/* Escalation tracking (within current time window) */
	unsigned int			queue_recoveries;
	unsigned int			channel_recoveries;
	unsigned int			pf_resets;
	unsigned int			core_resets;
	unsigned long			window_start;

	/* Lifetime recovery statistics */
	u64				total_recoveries;
	u64				total_recovery_failures;
	u64				total_escalations;
	u64				recovery_time_sum_ns;
};

/* -----------------------------------------------------------------------
 * Top-Level Health State
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_health - Top-level health monitoring state
 * @dev: Backpointer to owning fdice_dev
 * @poll_timer: Timer for periodic hardware health polling
 * @poll_jiffies: Current jittered poll interval in jiffies
 * @fatal_work: Work item for fatal error processing
 * @fw_work: Work item for non-fatal firmware event processing
 * @wq: Dedicated health workqueue (ordered, single-threaded)
 * @fw_reporter: Devlink health reporter for non-fatal FW events
 * @fw_fatal_reporter: Devlink health reporter for fatal FW errors
 * @port_reporter: Devlink health reporter for port/link events
 * @last_syndrome: Most recently detected syndrome
 * @fw_health_counter: Last observed firmware heartbeat counter value
 * @miss_count: Consecutive heartbeat misses (resets on success)
 * @history: Error history ring buffer with escalation tracking
 * @fatal_grace_period_ms: Grace period before auto-recovery (ms)
 * @auto_recover_enabled: Whether automatic recovery is enabled
 *
 * Embedded in struct fdice_dev. Initialized by fdice_health_init(),
 * activated by fdice_health_start(), stopped by fdice_health_stop(),
 * and torn down by fdice_health_cleanup().
 *
 * The tx/rx reporters are per-channel and stored in fdice_channel,
 * not here. This struct holds only device-level reporters.
 */
struct fdice_health {
	struct fdice_dev		*dev;

	/* Health polling timer */
	struct timer_list		poll_timer;
	unsigned long			poll_jiffies;

	/* Work items for deferred health processing */
	struct work_struct		fatal_work;
	struct work_struct		fw_work;
	struct workqueue_struct		*wq;

	/* Devlink health reporters (device-level) */
	struct devlink_health_reporter	*fw_reporter;
	struct devlink_health_reporter	*fw_fatal_reporter;
	struct devlink_health_reporter	*port_reporter;

	/* Syndrome state */
	struct fdice_health_syndrome	last_syndrome;
	u32				fw_health_counter;
	unsigned int			miss_count;

	/* Error history and escalation */
	struct fdice_health_history	history;

	/* Configuration */
	unsigned int			fatal_grace_period_ms;
	bool				auto_recover_enabled;
};

/* -----------------------------------------------------------------------
 * Health Subsystem Lifecycle API
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_init - Initialize the health monitoring subsystem
 * @fdev: FrankenDrICE device
 *
 * Allocates the health workqueue, initializes the poll timer with
 * jitter, sets up the history ring buffer, and registers the fw,
 * fw_fatal, and port devlink health reporters.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_health_init(struct fdice_dev *fdev);

/**
 * fdice_health_cleanup - Tear down the health monitoring subsystem
 * @fdev: FrankenDrICE device
 *
 * Cancels the poll timer, flushes and destroys the health workqueue,
 * unregisters all devlink health reporters, and frees resources.
 */
void fdice_health_cleanup(struct fdice_dev *fdev);

/**
 * fdice_health_start - Activate health polling
 * @fdev: FrankenDrICE device
 *
 * Arms the poll timer. Called from fdice_load() after hardware is
 * fully initialized and operational.
 */
void fdice_health_start(struct fdice_dev *fdev);

/**
 * fdice_health_stop - Deactivate health polling
 * @fdev: FrankenDrICE device
 *
 * Disarms the poll timer and flushes pending health work items.
 * Called from fdice_unload() before hardware teardown.
 */
void fdice_health_stop(struct fdice_dev *fdev);

/* -----------------------------------------------------------------------
 * Health Polling and Sensor Checks
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_poll_timer - Timer callback for periodic health checks
 * @t: timer_list embedded in fdice_health
 *
 * Checks PCI liveness, firmware heartbeat, and AQ responsiveness.
 * Schedules appropriate work items if issues are detected.
 * Re-arms itself with a jittered interval.
 */
void fdice_health_poll_timer(struct timer_list *t);

/**
 * fdice_health_check_sensors - Run all hardware sensor checks
 * @health: health monitoring state
 *
 * Invokes PCI liveness, firmware heartbeat, and AQ responsiveness
 * checks. Returns a bitmask of detected issues.
 *
 * Return: 0 if all sensors healthy, bitmask of FDICE_SENSOR_* flags otherwise.
 */
unsigned int fdice_health_check_sensors(struct fdice_health *health);

/* Sensor result flags returned by fdice_health_check_sensors() */
#define FDICE_SENSOR_PCI_DEAD		BIT(0)
#define FDICE_SENSOR_FW_HEARTBEAT	BIT(1)
#define FDICE_SENSOR_AQ_TIMEOUT		BIT(2)

/* -----------------------------------------------------------------------
 * History and Escalation
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_history_record - Record a health event in the history ring
 * @history: history tracking structure
 * @code: syndrome code of the event
 * @severity: severity of the event
 * @recovery_type: what recovery was attempted
 * @success: whether recovery succeeded
 */
void fdice_health_history_record(struct fdice_health_history *history,
				 u32 code,
				 enum fdice_health_severity severity,
				 enum fdice_reset_type recovery_type,
				 bool success);

/**
 * fdice_health_should_escalate - Check if escalation thresholds are exceeded
 * @history: history tracking structure
 * @current_level: current recovery level being attempted
 *
 * Checks whether the number of recoveries at @current_level within
 * the escalation time window exceeds the threshold, indicating that
 * escalation to the next level is warranted.
 *
 * Return: true if escalation should occur, false otherwise.
 */
bool fdice_health_should_escalate(struct fdice_health_history *history,
				  enum fdice_reset_type current_level);

/* -----------------------------------------------------------------------
 * Syndrome Lookup
 * ----------------------------------------------------------------------- */

/**
 * fdice_syndrome_lookup - Look up a syndrome code in the diagnostic table
 * @code: E810 health status code
 *
 * Searches the compiled-in syndrome table for a matching entry.
 *
 * Return: Pointer to the matching syndrome entry, or NULL if unknown.
 */
const struct fdice_syndrome_entry *fdice_syndrome_lookup(u32 code);

/* -----------------------------------------------------------------------
 * Reporter Callbacks (implemented in fdice_health_fw.c, _tx.c, etc.)
 * ----------------------------------------------------------------------- */

/* fw reporter ops */
extern const struct devlink_health_reporter_ops fdice_health_fw_ops;

/* fw_fatal reporter ops */
extern const struct devlink_health_reporter_ops fdice_health_fw_fatal_ops;

/* port reporter ops */
extern const struct devlink_health_reporter_ops fdice_health_port_ops;

/* FW reporter registration and work functions (fdice_health_fw.c) */
void fdice_health_fw_reporters_create(struct fdice_dev *fdev);
void fdice_health_fw_reporters_destroy(struct fdice_dev *fdev);
void fdice_health_fatal_work(struct work_struct *work);

/* Port reporter registration (fdice_health_port.c) */
void fdice_health_port_reporter_create(struct fdice_dev *fdev);
void fdice_health_port_reporter_destroy(struct fdice_dev *fdev);
void fdice_health_port_event(struct fdice_dev *fdev,
			     struct fdice_health_syndrome *syn);

/* TX per-queue reporter (fdice_health_tx.c) */
int fdice_health_tx_reporter_create(struct fdice_channel *ch);
void fdice_health_tx_reporter_destroy(struct fdice_channel *ch);
void fdice_health_tx_timeout_detect(struct fdice_channel *ch, u8 tc);

/* RX per-queue reporter (fdice_health_rx.c) */
int fdice_health_rx_reporter_create(struct fdice_channel *ch);
void fdice_health_rx_reporter_destroy(struct fdice_channel *ch);
void fdice_health_rx_stall_detect(struct fdice_channel *ch);

#endif /* _FDICE_HEALTH_CORE_H_ */
