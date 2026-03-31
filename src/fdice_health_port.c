// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE Port Health Reporter
 *
 * Implements a devlink health reporter for port-level and link health
 * events from firmware. Reports link-related health codes: module errors,
 * PHY problems, link configuration issues.
 *
 * Unlike the TX and RX reporters, the port reporter has no recover
 * callback -- link events are infrastructure-dependent and cannot be
 * resolved by driver-level recovery. The reporter provides diagnose and
 * dump callbacks for user-space visibility through `devlink health`.
 *
 * This adapts ice's extensive health status lookup table for human-readable
 * descriptions and suggested solutions, combined with mlx5-inspired
 * severity classification for consistent operational visibility.
 *
 * Architecture reference: docs/architecture/05-health-monitoring.md
 */

#include <linux/netdevice.h>
#include <linux/ktime.h>
#include <net/devlink.h>

#include "fdice_dev.h"
#include "fdice_aq_cmd.h"
#include "fdice_health_core.h"

/* -----------------------------------------------------------------------
 * Link Event History Ring
 * ----------------------------------------------------------------------- */

#define FDICE_LINK_EVENT_RING_SIZE	16

/**
 * struct fdice_link_event - Record of a single link state change
 * @timestamp: Kernel timestamp (ktime_get_ns) of the event
 * @link_up: Link state at time of event
 * @link_speed: Link speed bitmap at time of event
 * @syndrome_code: Associated health syndrome code (0 if none)
 */
struct fdice_link_event {
	u64	timestamp;
	bool	link_up;
	u16	link_speed;
	u32	syndrome_code;
};

/**
 * struct fdice_port_health_state - Per-device port health tracking
 * @events: Ring buffer of recent link events
 * @head: Next write position in the event ring
 * @count: Total events recorded
 *
 * File-local state for the port reporter's link event history.
 * Accessed only from the health workqueue context.
 */
static struct {
	struct fdice_link_event	events[FDICE_LINK_EVENT_RING_SIZE];
	unsigned int		head;
	unsigned int		count;
} fdice_port_state;

/**
 * fdice_port_health_record_event - Record a link event in the history ring
 * @link_up: Current link state
 * @link_speed: Current link speed bitmap
 * @syndrome_code: Associated health syndrome code
 */
static void fdice_port_health_record_event(bool link_up, u16 link_speed,
					   u32 syndrome_code)
{
	struct fdice_link_event *ev;

	ev = &fdice_port_state.events[fdice_port_state.head];
	ev->timestamp = ktime_get_ns();
	ev->link_up = link_up;
	ev->link_speed = link_speed;
	ev->syndrome_code = syndrome_code;

	fdice_port_state.head =
		(fdice_port_state.head + 1) % FDICE_LINK_EVENT_RING_SIZE;
	if (fdice_port_state.count < FDICE_LINK_EVENT_RING_SIZE)
		fdice_port_state.count++;
}

/* -----------------------------------------------------------------------
 * Link Speed String Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_link_speed_str - Convert link speed bitmap to human-readable string
 * @speed: Link speed bitmap from fdice_link_status_data.link_speed
 *
 * Return: Static string describing the link speed.
 */
static const char *fdice_link_speed_str(u16 speed)
{
	if (speed & FDICE_AQ_LINK_SPEED_100GB)
		return "100 Gbps";
	if (speed & FDICE_AQ_LINK_SPEED_50GB)
		return "50 Gbps";
	if (speed & FDICE_AQ_LINK_SPEED_40GB)
		return "40 Gbps";
	if (speed & FDICE_AQ_LINK_SPEED_25GB)
		return "25 Gbps";
	if (speed & FDICE_AQ_LINK_SPEED_10GB)
		return "10 Gbps";
	if (speed & FDICE_AQ_LINK_SPEED_5GB)
		return "5 Gbps";
	if (speed & FDICE_AQ_LINK_SPEED_2500MB)
		return "2.5 Gbps";
	if (speed & FDICE_AQ_LINK_SPEED_1000MB)
		return "1 Gbps";
	if (speed & FDICE_AQ_LINK_SPEED_100MB)
		return "100 Mbps";
	if (speed & FDICE_AQ_LINK_SPEED_10MB)
		return "10 Mbps";
	return "unknown";
}

/* -----------------------------------------------------------------------
 * Port Reporter: Diagnose
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_port_diagnose - Report link state via devlink fmsg
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @extack: netlink extended ACK for error reporting
 *
 * Reports the current link state including: link up/down, speed, FEC mode,
 * module type, fault conditions, and recent link event history.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_port_diagnose(struct devlink_health_reporter *reporter,
			   struct devlink_fmsg *fmsg,
			   struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = devlink_health_reporter_priv(reporter);
	struct fdice_health *health = &fdev->health;
	struct fdice_port_info *pi = fdev->hw.port_info;
	struct fdice_health_syndrome *syn = &health->last_syndrome;

	devlink_fmsg_obj_nest_start(fmsg);

	/* Current link state */
	devlink_fmsg_pair_nest_start(fmsg, "link_state");
	devlink_fmsg_obj_nest_start(fmsg);
	if (pi) {
		devlink_fmsg_bool_pair_put(fmsg, "link_up", pi->link_up);
		devlink_fmsg_string_pair_put(fmsg, "speed",
					     fdice_link_speed_str(pi->link_speed));
		devlink_fmsg_u32_pair_put(fmsg, "speed_raw", pi->link_speed);
		devlink_fmsg_u8_pair_put(fmsg, "lport", pi->lport);
		devlink_fmsg_u64_pair_put(fmsg, "phy_type_low",
					  pi->phy_type_low);
		devlink_fmsg_u64_pair_put(fmsg, "phy_type_high",
					  pi->phy_type_high);
	} else {
		devlink_fmsg_string_pair_put(fmsg, "status",
					     "port_info_unavailable");
	}
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Current syndrome if port-related */
	if (syn->code && syn->code < 0x1000) {
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

		/* Diagnostic data fields with labels */
		if (syn->data_labels[0]) {
			devlink_fmsg_string_pair_put(fmsg, "data0_label",
						     syn->data_labels[0]);
			devlink_fmsg_u32_pair_put(fmsg, "data0", syn->data[0]);
		}
		if (syn->data_labels[1]) {
			devlink_fmsg_string_pair_put(fmsg, "data1_label",
						     syn->data_labels[1]);
			devlink_fmsg_u32_pair_put(fmsg, "data1", syn->data[1]);
		}
		devlink_fmsg_u64_pair_put(fmsg, "timestamp", syn->timestamp);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);
	}

	/* Recent link event history */
	if (fdice_port_state.count > 0) {
		unsigned int i, count, start;

		count = fdice_port_state.count;
		if (count > FDICE_LINK_EVENT_RING_SIZE)
			count = FDICE_LINK_EVENT_RING_SIZE;

		start = (fdice_port_state.head + FDICE_LINK_EVENT_RING_SIZE -
			 count) % FDICE_LINK_EVENT_RING_SIZE;

		devlink_fmsg_arr_pair_nest_start(fmsg, "link_event_history");
		for (i = 0; i < count; i++) {
			unsigned int idx = (start + i) % FDICE_LINK_EVENT_RING_SIZE;
			struct fdice_link_event *ev =
				&fdice_port_state.events[idx];

			devlink_fmsg_obj_nest_start(fmsg);
			devlink_fmsg_u64_pair_put(fmsg, "timestamp",
						  ev->timestamp);
			devlink_fmsg_bool_pair_put(fmsg, "link_up",
						   ev->link_up);
			devlink_fmsg_string_pair_put(fmsg, "speed",
						     fdice_link_speed_str(ev->link_speed));
			if (ev->syndrome_code)
				devlink_fmsg_u32_pair_put(fmsg, "syndrome_code",
							  ev->syndrome_code);
			devlink_fmsg_obj_nest_end(fmsg);
		}
		devlink_fmsg_arr_pair_nest_end(fmsg);
	}

	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * Port Reporter: Dump
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_port_dump - Dump PHY capabilities and link event history
 * @reporter: devlink health reporter instance
 * @fmsg: formatted message builder
 * @priv_ctx: optional context from the report trigger
 * @extack: netlink extended ACK for error reporting
 *
 * Dumps comprehensive PHY capability information, FEC mode, link topology,
 * and the full link event history ring for post-mortem analysis.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int
fdice_health_port_dump(struct devlink_health_reporter *reporter,
		       struct devlink_fmsg *fmsg, void *priv_ctx,
		       struct netlink_ext_ack *extack)
{
	struct fdice_dev *fdev = devlink_health_reporter_priv(reporter);
	struct fdice_health *health = &fdev->health;
	struct fdice_port_info *pi = fdev->hw.port_info;
	struct fdice_health_syndrome *syn;

	devlink_fmsg_obj_nest_start(fmsg);

	/* Event context if provided */
	if (priv_ctx) {
		syn = priv_ctx;
		devlink_fmsg_pair_nest_start(fmsg, "event_syndrome");
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
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);
	}

	/* PHY capability dump */
	devlink_fmsg_pair_nest_start(fmsg, "phy_capabilities");
	devlink_fmsg_obj_nest_start(fmsg);
	if (pi) {
		devlink_fmsg_u8_pair_put(fmsg, "lport", pi->lport);
		devlink_fmsg_bool_pair_put(fmsg, "link_up", pi->link_up);
		devlink_fmsg_u32_pair_put(fmsg, "link_speed", pi->link_speed);
		devlink_fmsg_string_pair_put(fmsg, "speed_str",
					     fdice_link_speed_str(pi->link_speed));
		devlink_fmsg_u64_pair_put(fmsg, "phy_type_low",
					  pi->phy_type_low);
		devlink_fmsg_u64_pair_put(fmsg, "phy_type_high",
					  pi->phy_type_high);
	} else {
		devlink_fmsg_string_pair_put(fmsg, "status", "unavailable");
	}
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Device state context */
	devlink_fmsg_pair_nest_start(fmsg, "device_state");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u8_pair_put(fmsg, "pf_id", fdev->hw.pf_id);
	devlink_fmsg_bool_pair_put(fmsg, "pci_offline",
				   pci_channel_offline(fdev->pdev));
	devlink_fmsg_bool_pair_put(fmsg, "recovery_state",
				   test_bit(FDICE_STATE_RECOVERY, &fdev->state));
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Firmware version for correlation */
	devlink_fmsg_pair_nest_start(fmsg, "fw_version");
	devlink_fmsg_obj_nest_start(fmsg);
	devlink_fmsg_u8_pair_put(fmsg, "major", fdev->hw.fw.fw_major);
	devlink_fmsg_u8_pair_put(fmsg, "minor", fdev->hw.fw.fw_minor);
	devlink_fmsg_u8_pair_put(fmsg, "patch", fdev->hw.fw.fw_patch);
	devlink_fmsg_u32_pair_put(fmsg, "build", fdev->hw.fw.fw_build);
	devlink_fmsg_obj_nest_end(fmsg);
	devlink_fmsg_pair_nest_end(fmsg);

	/* Last known syndrome */
	syn = &health->last_syndrome;
	if (syn->code) {
		devlink_fmsg_pair_nest_start(fmsg, "last_syndrome");
		devlink_fmsg_obj_nest_start(fmsg);
		devlink_fmsg_u32_pair_put(fmsg, "code", syn->code);
		devlink_fmsg_u32_pair_put(fmsg, "severity",
					  (u32)syn->severity);
		devlink_fmsg_u64_pair_put(fmsg, "timestamp", syn->timestamp);
		devlink_fmsg_obj_nest_end(fmsg);
		devlink_fmsg_pair_nest_end(fmsg);
	}

	/* Full link event history */
	if (fdice_port_state.count > 0) {
		unsigned int i, count, start;

		count = fdice_port_state.count;
		if (count > FDICE_LINK_EVENT_RING_SIZE)
			count = FDICE_LINK_EVENT_RING_SIZE;

		start = (fdice_port_state.head + FDICE_LINK_EVENT_RING_SIZE -
			 count) % FDICE_LINK_EVENT_RING_SIZE;

		devlink_fmsg_arr_pair_nest_start(fmsg, "link_event_log");
		for (i = 0; i < count; i++) {
			unsigned int idx =
				(start + i) % FDICE_LINK_EVENT_RING_SIZE;
			struct fdice_link_event *ev =
				&fdice_port_state.events[idx];

			devlink_fmsg_obj_nest_start(fmsg);
			devlink_fmsg_u64_pair_put(fmsg, "timestamp",
						  ev->timestamp);
			devlink_fmsg_bool_pair_put(fmsg, "link_up",
						   ev->link_up);
			devlink_fmsg_u32_pair_put(fmsg, "speed_raw",
						  ev->link_speed);
			devlink_fmsg_string_pair_put(fmsg, "speed",
						     fdice_link_speed_str(ev->link_speed));
			if (ev->syndrome_code)
				devlink_fmsg_u32_pair_put(fmsg,
							  "syndrome_code",
							  ev->syndrome_code);
			devlink_fmsg_obj_nest_end(fmsg);
		}
		devlink_fmsg_arr_pair_nest_end(fmsg);
	}

	devlink_fmsg_obj_nest_end(fmsg);

	return 0;
}

/* -----------------------------------------------------------------------
 * Port Reporter Ops
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_port_ops - devlink_health_reporter_ops for port/link events
 *
 * Provides diagnose and dump callbacks for port-level health visibility.
 * No recover callback -- link events are infrastructure-dependent and
 * resolution requires physical intervention or firmware action.
 */
const struct devlink_health_reporter_ops fdice_health_port_ops = {
	.name		= "port",
	.diagnose	= fdice_health_port_diagnose,
	.dump		= fdice_health_port_dump,
};

/* -----------------------------------------------------------------------
 * Port Event Handler
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_port_event - Handle an AQ health status event for the port
 * @fdev: FrankenDrICE device
 * @syn: syndrome describing the health event
 *
 * Called from the AQ event handler when a health status event with a
 * port-related syndrome code is received. Records the event in the link
 * event history and triggers a devlink health report for user-space
 * visibility.
 */
void fdice_health_port_event(struct fdice_dev *fdev,
			     struct fdice_health_syndrome *syn)
{
	struct fdice_health *health = &fdev->health;
	struct fdice_port_info *pi = fdev->hw.port_info;

	/* Record in link event history */
	fdice_port_health_record_event(pi ? pi->link_up : false,
				       pi ? pi->link_speed : 0,
				       syn->code);

	/* Update the device-level last syndrome */
	health->last_syndrome = *syn;

	/* Report through devlink if reporter is available */
	if (!IS_ERR_OR_NULL(health->port_reporter))
		devlink_health_report(health->port_reporter,
				      "Port syndrome reported", syn);
}

/* -----------------------------------------------------------------------
 * Reporter Registration / Teardown
 * ----------------------------------------------------------------------- */

/**
 * fdice_health_port_reporter_create - Register the port health reporter
 * @fdev: FrankenDrICE device
 *
 * Creates the port-level devlink health reporter. Called during
 * fdice_health_init(). Errors are non-fatal -- the device operates
 * without port health reporting if registration fails.
 */
void fdice_health_port_reporter_create(struct fdice_dev *fdev)
{
	struct fdice_health *health = &fdev->health;
	struct devlink *devlink = fdice_devlink_from_dev(fdev);

	health->port_reporter =
		devl_health_reporter_create(devlink, &fdice_health_port_ops,
					    0, fdev);
	if (IS_ERR(health->port_reporter)) {
		dev_warn(&fdev->pdev->dev,
			 "Failed to create port health reporter: %pe\n",
			 health->port_reporter);
	}

	/* Initialize link event history */
	fdice_port_state.head = 0;
	fdice_port_state.count = 0;
}

/**
 * fdice_health_port_reporter_destroy - Unregister the port health reporter
 * @fdev: FrankenDrICE device
 *
 * Destroys the port-level devlink health reporter. Called during
 * fdice_health_cleanup(). Safe to call if the reporter was never created.
 */
void fdice_health_port_reporter_destroy(struct fdice_dev *fdev)
{
	struct fdice_health *health = &fdev->health;

	if (!IS_ERR_OR_NULL(health->port_reporter))
		devl_health_reporter_destroy(health->port_reporter);
}
