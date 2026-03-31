// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE eSwitch Mode Management
 *
 * Implements the two-mode embedded switch with legacy and switchdev operation.
 * The eSwitch manages VF vport lifecycle, mode transitions via devlink, and
 * lazy switchdev environment setup on first VF attach.
 *
 * Key design decisions:
 *   - mlx5-inspired vport xarray for scalable VF/uplink management
 *   - Lazy switchdev init: environment set up on first VF attach, not mode_set
 *   - VF state machine with defined transitions for robust lifecycle management
 *   - Pluggable rep_ops enabling future RDMA representor support
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.10)
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/xarray.h>
#include <linux/mutex.h>
#include <linux/etherdevice.h>
#include <net/devlink.h>

#include "fdice_dev.h"
#include "fdice_eswitch.h"
#include "fdice_repr.h"

/* Uplink vport uses index 0; VFs start at index 1 */
#define FDICE_ESW_UPLINK_VPORT_IDX	0
#define FDICE_ESW_VF_VPORT_BASE		1

/* -----------------------------------------------------------------------
 * VF State Machine
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_vf_state_name - Return human-readable VF state name
 * @state: VF lifecycle state
 *
 * Return: static string naming the state
 */
static const char *fdice_esw_vf_state_name(enum fdice_vf_state state)
{
	switch (state) {
	case FDICE_VF_STATE_DISABLED:
		return "disabled";
	case FDICE_VF_STATE_INITIALIZING:
		return "initializing";
	case FDICE_VF_STATE_ACTIVE:
		return "active";
	case FDICE_VF_STATE_QS_ENABLED:
		return "qs_enabled";
	case FDICE_VF_STATE_RESETTING:
		return "resetting";
	}
	return "unknown";
}

/**
 * fdice_esw_vf_transition - Perform a validated VF state transition
 * @vport: vport to transition
 * @new_state: target state
 *
 * Validates the state transition is legal and updates the vport state.
 * Illegal transitions are logged and rejected.
 *
 * Return: 0 on success, -EINVAL on invalid transition
 */
static int fdice_esw_vf_transition(struct fdice_vport *vport,
				   enum fdice_vf_state new_state)
{
	enum fdice_vf_state cur = vport->state;
	bool valid = false;

	switch (cur) {
	case FDICE_VF_STATE_DISABLED:
		valid = (new_state == FDICE_VF_STATE_INITIALIZING);
		break;
	case FDICE_VF_STATE_INITIALIZING:
		valid = (new_state == FDICE_VF_STATE_ACTIVE ||
			 new_state == FDICE_VF_STATE_DISABLED);
		break;
	case FDICE_VF_STATE_ACTIVE:
		valid = (new_state == FDICE_VF_STATE_QS_ENABLED ||
			 new_state == FDICE_VF_STATE_RESETTING ||
			 new_state == FDICE_VF_STATE_DISABLED);
		break;
	case FDICE_VF_STATE_QS_ENABLED:
		valid = (new_state == FDICE_VF_STATE_ACTIVE ||
			 new_state == FDICE_VF_STATE_RESETTING ||
			 new_state == FDICE_VF_STATE_DISABLED);
		break;
	case FDICE_VF_STATE_RESETTING:
		valid = (new_state == FDICE_VF_STATE_INITIALIZING ||
			 new_state == FDICE_VF_STATE_DISABLED);
		break;
	}

	if (!valid) {
		pr_warn("fdice: invalid VF %u state transition %s -> %s\n",
			vport->vf_id, fdice_esw_vf_state_name(cur),
			fdice_esw_vf_state_name(new_state));
		return -EINVAL;
	}

	vport->state = new_state;
	return 0;
}

/* -----------------------------------------------------------------------
 * Switchdev Environment Setup / Teardown
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_setup_switchdev_env - Configure uplink VSI for switchdev operation
 * @esw: eswitch instance
 *
 * Prepares the PF uplink VSI for eswitch operation. Sets up the uplink
 * vport, initializes bridge offload infrastructure, and marks the eswitch
 * as running.
 *
 * Called lazily on first VF attach in switchdev mode, not during mode_set
 * itself. This follows ice's proven lazy initialization pattern.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_esw_setup_switchdev_env(struct fdice_eswitch *esw)
{
	struct fdice_vport *uplink;
	int err;

	uplink = kzalloc(sizeof(*uplink), GFP_KERNEL);
	if (!uplink)
		return -ENOMEM;

	uplink->vsi_id = 0; /* PF uplink VSI */
	uplink->vf_id = 0;
	uplink->is_uplink = true;
	uplink->state = FDICE_VF_STATE_ACTIVE;

	err = xa_insert(&esw->vports, FDICE_ESW_UPLINK_VPORT_IDX, uplink,
			GFP_KERNEL);
	if (err) {
		kfree(uplink);
		return err;
	}

	esw->uplink_vport = uplink;

	err = fdice_esw_bridge_init(esw);
	if (err) {
		xa_erase(&esw->vports, FDICE_ESW_UPLINK_VPORT_IDX);
		kfree(uplink);
		esw->uplink_vport = NULL;
		return err;
	}

	esw->is_running = true;
	return 0;
}

/**
 * fdice_esw_teardown_switchdev_env - Tear down switchdev environment
 * @esw: eswitch instance
 *
 * Cleans up the switchdev environment: destroys bridge offloads,
 * removes the uplink vport, and marks the eswitch as not running.
 * All VF vports must have been detached before calling this.
 */
static void fdice_esw_teardown_switchdev_env(struct fdice_eswitch *esw)
{
	esw->is_running = false;

	fdice_esw_bridge_cleanup(esw);

	if (esw->uplink_vport) {
		xa_erase(&esw->vports, FDICE_ESW_UPLINK_VPORT_IDX);
		kfree(esw->uplink_vport);
		esw->uplink_vport = NULL;
	}
}

/* -----------------------------------------------------------------------
 * eSwitch Init / Cleanup
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_init - Initialize the eSwitch subsystem
 * @fdev: FrankenDrICE device
 *
 * Allocates the eswitch structure, initializes the vport xarray and
 * mode lock. The eswitch starts in legacy mode with no VFs attached.
 * Actual switchdev setup is deferred to first VF attach.
 *
 * Return: 0 on success, -ENOMEM on allocation failure
 */
int fdice_esw_init(struct fdice_dev *fdev)
{
	struct fdice_eswitch *esw;

	esw = kzalloc(sizeof(*esw), GFP_KERNEL);
	if (!esw)
		return -ENOMEM;

	esw->fdev = fdev;
	esw->mode = FDICE_ESWITCH_LEGACY;
	esw->num_vfs = 0;
	esw->is_running = false;
	xa_init(&esw->vports);
	mutex_init(&esw->mode_lock);

	fdev->esw = esw;
	return 0;
}

/**
 * fdice_esw_cleanup - Clean up the eSwitch subsystem
 * @fdev: FrankenDrICE device
 *
 * Tears down switchdev environment if active, destroys the vport xarray,
 * and frees the eswitch structure. Must be called after all VFs have been
 * detached.
 */
void fdice_esw_cleanup(struct fdice_dev *fdev)
{
	struct fdice_eswitch *esw = fdev->esw;
	struct fdice_vport *vport;
	unsigned long idx;

	if (!esw)
		return;

	mutex_lock(&esw->mode_lock);

	if (esw->is_running)
		fdice_esw_teardown_switchdev_env(esw);

	/* Free any remaining vports (shouldn't be any, but defensive) */
	xa_for_each(&esw->vports, idx, vport)
		kfree(vport);

	xa_destroy(&esw->vports);

	mutex_unlock(&esw->mode_lock);
	mutex_destroy(&esw->mode_lock);

	kfree(esw);
	fdev->esw = NULL;
}

/* -----------------------------------------------------------------------
 * Mode Set / Get (devlink eswitch mode)
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_mode_set - Set eSwitch operating mode
 * @fdev: FrankenDrICE device
 * @mode: DEVLINK_ESWITCH_MODE_LEGACY or DEVLINK_ESWITCH_MODE_SWITCHDEV
 *
 * Transitions the eSwitch between legacy and switchdev modes. Mode changes
 * are only permitted when no VFs are attached (following ice's constraint
 * for simplicity; graceful migration is a future enhancement).
 *
 * In switchdev mode, the actual environment setup is deferred to the first
 * VF attach (lazy initialization).
 *
 * Return: 0 on success, -EINVAL for invalid mode, -EBUSY if VFs are present
 */
int fdice_esw_mode_set(struct fdice_dev *fdev, u16 mode)
{
	struct fdice_eswitch *esw = fdev->esw;
	enum fdice_eswitch_mode target;
	int err = 0;

	if (!esw)
		return -ENODEV;

	switch (mode) {
	case DEVLINK_ESWITCH_MODE_LEGACY:
		target = FDICE_ESWITCH_LEGACY;
		break;
	case DEVLINK_ESWITCH_MODE_SWITCHDEV:
		target = FDICE_ESWITCH_SWITCHDEV;
		break;
	default:
		return -EINVAL;
	}

	mutex_lock(&esw->mode_lock);

	if (esw->mode == target)
		goto unlock;

	/* Mode change requires no VFs attached */
	if (esw->num_vfs > 0) {
		dev_warn(&fdev->pdev->dev,
			 "cannot change eswitch mode with %u VFs attached\n",
			 esw->num_vfs);
		err = -EBUSY;
		goto unlock;
	}

	/* Tear down old mode if switchdev was running */
	if (esw->is_running)
		fdice_esw_teardown_switchdev_env(esw);

	esw->mode = target;
	dev_info(&fdev->pdev->dev, "eswitch mode set to %s\n",
		 target == FDICE_ESWITCH_SWITCHDEV ? "switchdev" : "legacy");

unlock:
	mutex_unlock(&esw->mode_lock);
	return err;
}

/**
 * fdice_esw_mode_get - Get current eSwitch operating mode
 * @fdev: FrankenDrICE device
 * @mode: output devlink eswitch mode value
 *
 * Return: 0 on success, -ENODEV if eswitch not initialized
 */
int fdice_esw_mode_get(struct fdice_dev *fdev, u16 *mode)
{
	struct fdice_eswitch *esw = fdev->esw;

	if (!esw)
		return -ENODEV;

	switch (esw->mode) {
	case FDICE_ESWITCH_LEGACY:
		*mode = DEVLINK_ESWITCH_MODE_LEGACY;
		break;
	case FDICE_ESWITCH_SWITCHDEV:
		*mode = DEVLINK_ESWITCH_MODE_SWITCHDEV;
		break;
	}

	return 0;
}

/* -----------------------------------------------------------------------
 * VF Attach / Detach
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_vf_attach - Attach a VF to the eSwitch
 * @esw: eswitch instance
 * @vf_id: VF number to attach
 * @vsi_id: VSI assigned to this VF
 *
 * Creates a vport for the VF and, in switchdev mode, creates a port
 * representor. The switchdev environment is lazily initialized on the
 * first VF attach if not already running.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_esw_vf_attach(struct fdice_eswitch *esw, u16 vf_id, u16 vsi_id)
{
	struct fdice_vport *vport;
	unsigned long vport_idx;
	int err;

	if (!esw)
		return -ENODEV;

	mutex_lock(&esw->mode_lock);

	/* Lazy switchdev environment setup on first VF attach */
	if (fdice_esw_is_switchdev(esw) && !esw->is_running) {
		err = fdice_esw_setup_switchdev_env(esw);
		if (err) {
			mutex_unlock(&esw->mode_lock);
			return err;
		}
	}

	vport = kzalloc(sizeof(*vport), GFP_KERNEL);
	if (!vport) {
		mutex_unlock(&esw->mode_lock);
		return -ENOMEM;
	}

	vport->vsi_id = vsi_id;
	vport->vf_id = vf_id;
	vport->state = FDICE_VF_STATE_DISABLED;
	vport->is_uplink = false;

	/* Transition to INITIALIZING */
	err = fdice_esw_vf_transition(vport, FDICE_VF_STATE_INITIALIZING);
	if (err)
		goto err_free;

	vport_idx = FDICE_ESW_VF_VPORT_BASE + vf_id;
	err = xa_insert(&esw->vports, vport_idx, vport, GFP_KERNEL);
	if (err)
		goto err_free;

	/* Create representor in switchdev mode */
	if (fdice_esw_is_switchdev(esw)) {
		struct fdice_repr *repr;

		repr = fdice_repr_create(esw, vport);
		if (IS_ERR(repr)) {
			err = PTR_ERR(repr);
			goto err_xa;
		}
		vport->repr = repr;
	}

	/* Transition to ACTIVE */
	fdice_esw_vf_transition(vport, FDICE_VF_STATE_ACTIVE);
	esw->num_vfs++;

	mutex_unlock(&esw->mode_lock);
	return 0;

err_xa:
	xa_erase(&esw->vports, vport_idx);
err_free:
	kfree(vport);
	mutex_unlock(&esw->mode_lock);
	return err;
}

/**
 * fdice_esw_vf_detach - Detach a VF from the eSwitch
 * @esw: eswitch instance
 * @vf_id: VF number to detach
 *
 * Destroys the VF's representor (if present), removes its vport from
 * the xarray, and frees resources. If this is the last VF and switchdev
 * is running, the switchdev environment is torn down.
 */
void fdice_esw_vf_detach(struct fdice_eswitch *esw, u16 vf_id)
{
	struct fdice_vport *vport;
	unsigned long vport_idx;

	if (!esw)
		return;

	mutex_lock(&esw->mode_lock);

	vport_idx = FDICE_ESW_VF_VPORT_BASE + vf_id;
	vport = xa_erase(&esw->vports, vport_idx);
	if (!vport) {
		mutex_unlock(&esw->mode_lock);
		return;
	}

	/* Destroy representor if present */
	if (vport->repr) {
		fdice_repr_destroy(vport->repr);
		vport->repr = NULL;
	}

	vport->state = FDICE_VF_STATE_DISABLED;
	esw->num_vfs--;

	/* Tear down switchdev environment when last VF detaches */
	if (esw->is_running && esw->num_vfs == 0)
		fdice_esw_teardown_switchdev_env(esw);

	kfree(vport);

	mutex_unlock(&esw->mode_lock);
}

/* -----------------------------------------------------------------------
 * Representor Ops Registration
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_register_rep_ops - Register representor ops for a protocol type
 * @esw: eswitch instance
 * @type: representor protocol type (ETH, IB)
 * @ops: operations to register
 *
 * Called during driver initialization to register the ETH representor
 * ops. The IB type is reserved for future irdma representor support.
 */
void fdice_esw_register_rep_ops(struct fdice_eswitch *esw,
				enum fdice_rep_type type,
				const struct fdice_rep_ops *ops)
{
	if (!esw || type >= FDICE_NUM_REP_TYPES)
		return;

	esw->rep_ops[type] = ops;
}
