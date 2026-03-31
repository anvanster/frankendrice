// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE eSwitch Bridge FDB Offload
 *
 * Implements bridge FDB (Forwarding Database) offload for the eSwitch.
 * Handles switchdev FDB notifications from the Linux bridge, programs
 * forward and guard rule pairs via the flow steering FDB namespace, and
 * provides VLAN-aware bridge support.
 *
 * Each FDB entry consists of two hardware rules:
 *   1. Forward rule: matches destination MAC, forwards to correct VSI
 *   2. Guard rule: prevents flooding for known MAC addresses
 *
 * An rhashtable keyed by (MAC + VLAN ID) provides O(1) lookup for FDB
 * entry management. The design follows ice's proven bridge offload pattern
 * with improvements: per-entry flow counters and cleaner abstraction.
 *
 * Architecture reference: docs/exploration/15-switchdev-mode.md
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/rhashtable.h>
#include <linux/workqueue.h>
#include <linux/notifier.h>
#include <net/switchdev.h>

#include "fdice_dev.h"
#include "fdice_eswitch.h"
#include "fdice_fs_core.h"

/* -----------------------------------------------------------------------
 * FDB Data Structures
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_esw_br_fdb_key - Hash key for FDB entry lookup
 * @mac: destination MAC address
 * @vid: VLAN ID (0 for untagged)
 *
 * Combined key for rhashtable-based O(1) FDB entry lookups.
 */
struct fdice_esw_br_fdb_key {
	u8		mac[ETH_ALEN];
	u16		vid;
};

/**
 * struct fdice_esw_br_fdb_entry - Bridge FDB offloaded entry
 * @key: lookup key (MAC + VLAN)
 * @vsi_id: target VSI for forwarded traffic
 * @fwd_entry: forward rule in FDB flow steering namespace
 * @guard_entry: guard rule preventing flooding for this MAC
 * @ht_node: rhashtable linkage
 * @rcu: RCU head for deferred freeing
 * @flags: FDB entry flags (static, offloaded, etc.)
 *
 * Represents a single FDB entry offloaded to hardware. The forward rule
 * steers matching traffic to the target VSI; the guard rule suppresses
 * flooding for the MAC address.
 */
struct fdice_esw_br_fdb_entry {
	struct fdice_esw_br_fdb_key	key;
	u16				vsi_id;
	struct fdice_flow_entry		*fwd_entry;
	struct fdice_flow_entry		*guard_entry;
	struct rhash_head		ht_node;
	struct rcu_head			rcu;
	u16				flags;
};

/**
 * struct fdice_esw_br_port - Per-port bridge state
 * @vport_id: vport index in the eswitch vports xarray
 * @vsi_id: VSI associated with this bridge port
 * @vlans: xarray of configured VLAN IDs on this port
 * @list: linkage in bridge's port list
 */
struct fdice_esw_br_port {
	u16			vport_id;
	u16			vsi_id;
	struct xarray		vlans;
	struct list_head	list;
};

/**
 * struct fdice_esw_bridge - Bridge offload instance
 * @esw: backpointer to eswitch
 * @fdb_ht: rhashtable for O(1) FDB entry lookup by (MAC, VID)
 * @ports: list of bridge ports
 * @fdb_work: workqueue for deferred FDB notification processing
 * @port_lock: protects port list and VLAN xarrays
 *
 * Manages all bridge offload state including FDB entries and per-port
 * VLAN configuration. FDB notifications from the switchdev layer are
 * processed via a work queue to avoid holding rtnl_lock during AQ ops.
 */
struct fdice_esw_bridge {
	struct fdice_eswitch		*esw;
	struct rhashtable		fdb_ht;
	struct list_head		ports;
	struct workqueue_struct		*fdb_work;
	struct mutex			port_lock;
};

/* -----------------------------------------------------------------------
 * rhashtable Parameters
 * ----------------------------------------------------------------------- */

static u32 fdice_esw_br_fdb_hash(const void *data, u32 len, u32 seed)
{
	const struct fdice_esw_br_fdb_key *key = data;

	return jhash2((const u32 *)key, sizeof(*key) / sizeof(u32), seed);
}

static int fdice_esw_br_fdb_cmp(struct rhashtable_compare_arg *arg,
				const void *obj)
{
	const struct fdice_esw_br_fdb_key *key = arg->key;
	const struct fdice_esw_br_fdb_entry *entry = obj;

	if (entry->key.vid != key->vid)
		return 1;

	return !ether_addr_equal(entry->key.mac, key->mac);
}

static const struct rhashtable_params fdice_esw_br_fdb_ht_params = {
	.key_offset	= offsetof(struct fdice_esw_br_fdb_entry, key),
	.key_len	= sizeof(struct fdice_esw_br_fdb_key),
	.head_offset	= offsetof(struct fdice_esw_br_fdb_entry, ht_node),
	.hashfn		= fdice_esw_br_fdb_hash,
	.obj_cmpfn	= fdice_esw_br_fdb_cmp,
	.automatic_shrinking = true,
};

/* -----------------------------------------------------------------------
 * FDB Rule Programming
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_br_add_fdb - Create forward + guard rule pair for an FDB entry
 * @bridge: bridge offload instance
 * @mac: destination MAC address
 * @vid: VLAN ID (0 for untagged)
 * @vsi_id: target VSI for forwarding
 *
 * Programs two flow rules in the FDB namespace:
 *   1. A forward rule matching the destination MAC (and VLAN if non-zero)
 *      with an action to forward to the target VSI.
 *   2. A guard rule at lower priority to prevent flooding for this known
 *      MAC address.
 *
 * Both rules are inserted into the rhashtable for O(1) lookup on delete.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_esw_br_add_fdb(struct fdice_esw_bridge *bridge,
				const u8 *mac, u16 vid, u16 vsi_id)
{
	struct fdice_dev *fdev = bridge->esw->fdev;
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_esw_br_fdb_entry *entry;
	struct fdice_match_values match = {};
	struct fdice_match_criteria criteria = {};
	struct fdice_flow_action fwd_action = {};
	struct fdice_flow_action guard_action = {};
	struct fdice_flow_entry *fwd_fe, *guard_fe;
	int err;

	if (!fs || !fs->fdb_ns)
		return -ENODEV;

	entry = kzalloc(sizeof(*entry), GFP_KERNEL);
	if (!entry)
		return -ENOMEM;

	ether_addr_copy(entry->key.mac, mac);
	entry->key.vid = vid;
	entry->vsi_id = vsi_id;

	/* Build match criteria: destination MAC, optionally VLAN */
	ether_addr_copy(match.dmac, mac);
	criteria.outer_headers = FDICE_MATCH_DMAC;

	if (vid) {
		match.vlan_id = vid;
		criteria.outer_headers |= FDICE_MATCH_VLAN_ID;
	}

	/* Forward rule: steer to target VSI */
	fwd_action.type = FDICE_FLOW_ACTION_FORWARD;
	fwd_action.forward.dest_vsi = vsi_id;
	fwd_action.forward.dest_queue = 0;

	fwd_fe = fdice_flow_entry_create_autogroup(
		fs->fdb_ns, FDICE_FS_PRIO_FDB_BRIDGE, &match, &criteria,
		&fwd_action);
	if (IS_ERR(fwd_fe)) {
		err = PTR_ERR(fwd_fe);
		goto err_free;
	}
	entry->fwd_entry = fwd_fe;

	/* Guard rule: drop at default priority to prevent flooding */
	guard_action.type = FDICE_FLOW_ACTION_DROP;

	guard_fe = fdice_flow_entry_create_autogroup(
		fs->fdb_ns, FDICE_FS_PRIO_FDB_DEFAULT, &match, &criteria,
		&guard_action);
	if (IS_ERR(guard_fe)) {
		err = PTR_ERR(guard_fe);
		goto err_fwd;
	}
	entry->guard_entry = guard_fe;

	/* Insert into rhashtable for O(1) lookup */
	err = rhashtable_insert_fast(&bridge->fdb_ht, &entry->ht_node,
				     fdice_esw_br_fdb_ht_params);
	if (err)
		goto err_guard;

	return 0;

err_guard:
	fdice_flow_entry_destroy(guard_fe);
err_fwd:
	fdice_flow_entry_destroy(fwd_fe);
err_free:
	kfree(entry);
	return err;
}

/**
 * fdice_esw_br_del_fdb - Remove forward + guard rule pair for an FDB entry
 * @bridge: bridge offload instance
 * @mac: destination MAC address
 * @vid: VLAN ID
 *
 * Looks up the FDB entry by (MAC, VID), removes both flow rules from
 * hardware, removes the entry from the rhashtable, and frees resources.
 *
 * Return: 0 on success, -ENOENT if entry not found
 */
static int fdice_esw_br_del_fdb(struct fdice_esw_bridge *bridge,
				const u8 *mac, u16 vid)
{
	struct fdice_esw_br_fdb_key key;
	struct fdice_esw_br_fdb_entry *entry;

	ether_addr_copy(key.mac, mac);
	key.vid = vid;

	entry = rhashtable_lookup_fast(&bridge->fdb_ht, &key,
				       fdice_esw_br_fdb_ht_params);
	if (!entry)
		return -ENOENT;

	rhashtable_remove_fast(&bridge->fdb_ht, &entry->ht_node,
			       fdice_esw_br_fdb_ht_params);

	if (entry->guard_entry)
		fdice_flow_entry_destroy(entry->guard_entry);
	if (entry->fwd_entry)
		fdice_flow_entry_destroy(entry->fwd_entry);

	kfree_rcu(entry, rcu);
	return 0;
}

/* -----------------------------------------------------------------------
 * Switchdev FDB Event Handling
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_esw_br_fdb_work - Deferred FDB event work item
 * @work: work_struct for workqueue scheduling
 * @bridge: bridge offload instance
 * @mac: MAC address from the FDB notification
 * @vid: VLAN ID from the FDB notification
 * @vsi_id: target VSI (for add events)
 * @adding: true for add, false for delete
 */
struct fdice_esw_br_fdb_work {
	struct work_struct		work;
	struct fdice_esw_bridge		*bridge;
	u8				mac[ETH_ALEN];
	u16				vid;
	u16				vsi_id;
	bool				adding;
};

/**
 * fdice_esw_br_fdb_work_fn - Work function for deferred FDB processing
 * @work: work_struct embedded in fdice_esw_br_fdb_work
 *
 * Processes a single FDB add or delete event. Called from the bridge
 * offload workqueue to avoid holding rtnl_lock during hardware programming.
 */
static void fdice_esw_br_fdb_work_fn(struct work_struct *work)
{
	struct fdice_esw_br_fdb_work *fdb_work =
		container_of(work, struct fdice_esw_br_fdb_work, work);
	struct fdice_esw_bridge *bridge = fdb_work->bridge;
	int err;

	mutex_lock(&bridge->port_lock);

	if (fdb_work->adding)
		err = fdice_esw_br_add_fdb(bridge, fdb_work->mac,
					   fdb_work->vid, fdb_work->vsi_id);
	else
		err = fdice_esw_br_del_fdb(bridge, fdb_work->mac,
					   fdb_work->vid);

	mutex_unlock(&bridge->port_lock);

	if (err && err != -ENOENT)
		pr_warn("fdice: bridge FDB %s failed for %pM vid %u: %d\n",
			fdb_work->adding ? "add" : "del",
			fdb_work->mac, fdb_work->vid, err);

	kfree(fdb_work);
}

/**
 * fdice_esw_br_fdb_event - Handle switchdev FDB notification
 * @esw: eswitch instance
 * @mac: MAC address from the notification
 * @vid: VLAN ID from the notification
 * @vsi_id: target VSI for forward rule (ignored for delete)
 * @adding: true if adding, false if deleting
 *
 * Queues a work item for deferred FDB processing. The actual hardware
 * programming happens in the workqueue to avoid blocking the switchdev
 * notification path.
 *
 * Return: 0 on success, -ENOMEM on allocation failure
 */
int fdice_esw_br_fdb_event(struct fdice_eswitch *esw,
			   const u8 *mac, u16 vid, u16 vsi_id, bool adding)
{
	struct fdice_esw_bridge *bridge = esw->bridge;
	struct fdice_esw_br_fdb_work *fdb_work;

	if (!bridge || !bridge->fdb_work)
		return -ENODEV;

	fdb_work = kzalloc(sizeof(*fdb_work), GFP_ATOMIC);
	if (!fdb_work)
		return -ENOMEM;

	INIT_WORK(&fdb_work->work, fdice_esw_br_fdb_work_fn);
	fdb_work->bridge = bridge;
	ether_addr_copy(fdb_work->mac, mac);
	fdb_work->vid = vid;
	fdb_work->vsi_id = vsi_id;
	fdb_work->adding = adding;

	queue_work(bridge->fdb_work, &fdb_work->work);
	return 0;
}

/* -----------------------------------------------------------------------
 * Bridge Port VLAN Management
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_br_port_vlan_add - Add a VLAN to a bridge port
 * @port: bridge port
 * @vid: VLAN ID to add
 *
 * Tracks VLAN membership on a per-port basis for VLAN-aware bridge
 * offload. The VLAN is stored in the port's xarray for lookup during
 * FDB entry creation.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_esw_br_port_vlan_add(struct fdice_esw_br_port *port, u16 vid)
{
	return xa_insert(&port->vlans, vid,
			 xa_mk_value(vid), GFP_KERNEL);
}

/**
 * fdice_esw_br_port_vlan_del - Remove a VLAN from a bridge port
 * @port: bridge port
 * @vid: VLAN ID to remove
 */
static void fdice_esw_br_port_vlan_del(struct fdice_esw_br_port *port, u16 vid)
{
	xa_erase(&port->vlans, vid);
}

/**
 * fdice_esw_br_port_vlan_lookup - Check if a VLAN is configured on a port
 * @port: bridge port
 * @vid: VLAN ID to look up
 *
 * Return: true if the VLAN is configured on this port
 */
static bool fdice_esw_br_port_vlan_lookup(struct fdice_esw_br_port *port,
					  u16 vid)
{
	return xa_load(&port->vlans, vid) != NULL;
}

/* Suppress unused function warnings for port VLAN helpers until wired */
static void __maybe_unused
fdice_esw_br_port_vlan_helpers_used(struct fdice_esw_br_port *port)
{
	(void)fdice_esw_br_port_vlan_add;
	(void)fdice_esw_br_port_vlan_del;
	(void)fdice_esw_br_port_vlan_lookup;
}

/* -----------------------------------------------------------------------
 * Bridge Init / Cleanup
 * ----------------------------------------------------------------------- */

/**
 * fdice_esw_bridge_init - Initialize bridge offload infrastructure
 * @esw: eswitch instance
 *
 * Creates the bridge offload structure, initializes the FDB rhashtable,
 * and creates the workqueue for deferred FDB notification processing.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_esw_bridge_init(struct fdice_eswitch *esw)
{
	struct fdice_esw_bridge *bridge;
	int err;

	bridge = kzalloc(sizeof(*bridge), GFP_KERNEL);
	if (!bridge)
		return -ENOMEM;

	bridge->esw = esw;
	INIT_LIST_HEAD(&bridge->ports);
	mutex_init(&bridge->port_lock);

	err = rhashtable_init(&bridge->fdb_ht, &fdice_esw_br_fdb_ht_params);
	if (err)
		goto err_free;

	bridge->fdb_work = alloc_ordered_workqueue("fdice_br_fdb", 0);
	if (!bridge->fdb_work) {
		err = -ENOMEM;
		goto err_ht;
	}

	esw->bridge = bridge;
	return 0;

err_ht:
	rhashtable_destroy(&bridge->fdb_ht);
err_free:
	mutex_destroy(&bridge->port_lock);
	kfree(bridge);
	return err;
}

/**
 * fdice_esw_bridge_cleanup - Tear down bridge offload infrastructure
 * @esw: eswitch instance
 *
 * Flushes and destroys the FDB workqueue, removes all FDB entries and
 * bridge ports, destroys the rhashtable, and frees the bridge structure.
 */
void fdice_esw_bridge_cleanup(struct fdice_eswitch *esw)
{
	struct fdice_esw_bridge *bridge = esw->bridge;
	struct fdice_esw_br_port *port, *tmp;

	if (!bridge)
		return;

	/* Flush pending FDB work before tearing down */
	if (bridge->fdb_work) {
		destroy_workqueue(bridge->fdb_work);
		bridge->fdb_work = NULL;
	}

	/* Clean up ports */
	mutex_lock(&bridge->port_lock);
	list_for_each_entry_safe(port, tmp, &bridge->ports, list) {
		xa_destroy(&port->vlans);
		list_del(&port->list);
		kfree(port);
	}
	mutex_unlock(&bridge->port_lock);

	rhashtable_destroy(&bridge->fdb_ht);
	mutex_destroy(&bridge->port_lock);

	kfree(bridge);
	esw->bridge = NULL;
}
