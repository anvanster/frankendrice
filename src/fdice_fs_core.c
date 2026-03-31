// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE Flow Steering Core
 *
 * Tree-based flow steering infrastructure providing hierarchical organization,
 * refcounted lifetime management, per-node locking, and namespace isolation
 * for all E810 flow rules.
 *
 * The tree has five levels: namespace -> priority -> table -> group -> entry.
 * Every object embeds struct fdice_fs_node as its base type, enabling uniform
 * traversal, reference counting, and cascade removal.
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include <linux/slab.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/refcount.h>
#include <linux/idr.h>
#include <linux/device.h>

#include "fdice_dev.h"
#include "fdice_fs_core.h"
#include "fdice_fs_cmd.h"
#include "fdice_fs_recipe.h"

/* -----------------------------------------------------------------------
 * Node Lifecycle: init, get, put
 * ----------------------------------------------------------------------- */

/**
 * fdice_fs_node_init - Initialize a flow steering tree node
 * @node: node to initialize
 * @type: node type discriminator
 * @root: root node of the tree (NULL if this node IS the root)
 *
 * Sets up children list, sibling linkage, locking, and refcount.
 * The refcount starts at 1 (owned by creator).
 */
void fdice_fs_node_init(struct fdice_fs_node *node,
			enum fdice_fs_node_type type,
			struct fdice_fs_node *root)
{
	INIT_LIST_HEAD(&node->children);
	INIT_LIST_HEAD(&node->sibling);
	node->parent = NULL;
	node->root = root ? root : node;
	node->type = type;
	init_rwsem(&node->lock);
	refcount_set(&node->refcount, 1);
	node->del_hw_func = NULL;
	node->del_sw_func = NULL;
}

/**
 * fdice_fs_node_get - Acquire a reference to a tree node
 * @node: node to reference
 *
 * Increments the refcount. Caller must already hold a valid reference.
 *
 * Return: @node for convenience in assignment expressions
 */
struct fdice_fs_node *fdice_fs_node_get(struct fdice_fs_node *node)
{
	if (node)
		refcount_inc(&node->refcount);
	return node;
}

/**
 * fdice_fs_node_put - Release a reference to a tree node
 * @node: node to release
 *
 * Decrements the refcount. When refcount reaches zero, invokes the
 * software destructor (del_sw_func) to free the node's memory.
 * The hardware destructor (del_hw_func) must have been called
 * separately during node removal.
 */
void fdice_fs_node_put(struct fdice_fs_node *node)
{
	if (!node)
		return;

	if (refcount_dec_and_test(&node->refcount)) {
		if (node->del_sw_func)
			node->del_sw_func(node);
	}
}

/* -----------------------------------------------------------------------
 * Tree Manipulation: add_child, remove, remove_children
 * ----------------------------------------------------------------------- */

/**
 * fdice_fs_node_add_child - Add a child node to a parent
 * @parent: parent node (caller must hold parent's write lock)
 * @child: child node to add
 *
 * Links the child into the parent's children list and sets the
 * child's parent pointer. The parent acquires a reference to the
 * child to prevent premature freeing while the child is in the tree.
 */
void fdice_fs_node_add_child(struct fdice_fs_node *parent,
			     struct fdice_fs_node *child)
{
	child->parent = parent;
	child->root = parent->root;
	list_add_tail(&child->sibling, &parent->children);
	fdice_fs_node_get(child);
}

/**
 * fdice_fs_node_remove - Remove a single node from its parent
 * @node: node to remove
 *
 * Unlinks the node from its parent's children list, calls the
 * hardware destructor if present, then drops the parent's reference.
 * Caller must hold the parent's write lock.
 */
void fdice_fs_node_remove(struct fdice_fs_node *node)
{
	if (!node || list_empty(&node->sibling))
		return;

	if (node->del_hw_func)
		node->del_hw_func(node);

	list_del_init(&node->sibling);
	node->parent = NULL;

	/* Drop the reference that the parent held */
	fdice_fs_node_put(node);
}

/**
 * fdice_fs_node_remove_children - Cascade-remove all children bottom-up
 * @node: parent whose children to remove
 *
 * Recursively removes all descendants in post-order (leaves first),
 * ensuring hardware resources are freed bottom-up. Acquires write
 * lock on @node before iterating.
 */
void fdice_fs_node_remove_children(struct fdice_fs_node *node)
{
	struct fdice_fs_node *child, *tmp;

	if (!node)
		return;

	down_write(&node->lock);

	list_for_each_entry_safe(child, tmp, &node->children, sibling) {
		/*
		 * Recurse into grandchildren before removing the child.
		 * Release our lock temporarily to avoid AB-BA deadlock
		 * between parent and child locks.
		 */
		fdice_fs_node_get(child);
		up_write(&node->lock);

		fdice_fs_node_remove_children(child);

		down_write(&node->lock);
		fdice_fs_node_remove(child);
		fdice_fs_node_put(child);
	}

	up_write(&node->lock);
}

/* -----------------------------------------------------------------------
 * Software Destructors for Each Node Type
 * ----------------------------------------------------------------------- */

static void fdice_flow_namespace_del_sw(struct fdice_fs_node *node)
{
	struct fdice_flow_namespace *ns;

	ns = container_of(node, struct fdice_flow_namespace, node);
	kfree(ns);
}

static void fdice_flow_priority_del_sw(struct fdice_fs_node *node)
{
	struct fdice_flow_priority *prio;

	prio = container_of(node, struct fdice_flow_priority, node);
	kfree(prio);
}

static void fdice_flow_table_del_sw(struct fdice_fs_node *node)
{
	struct fdice_flow_table *ft;

	ft = container_of(node, struct fdice_flow_table, node);
	ida_destroy(&ft->group_ida);
	kfree(ft);
}

static void fdice_flow_group_del_sw(struct fdice_fs_node *node)
{
	struct fdice_flow_group *fg;

	fg = container_of(node, struct fdice_flow_group, node);
	ida_destroy(&fg->entry_ida);
	kfree(fg);
}

static void fdice_flow_entry_del_sw(struct fdice_fs_node *node)
{
	struct fdice_flow_entry *fe;

	fe = container_of(node, struct fdice_flow_entry, node);
	kfree(fe);
}

/* -----------------------------------------------------------------------
 * Hardware Destructors (via command vtable)
 * ----------------------------------------------------------------------- */

/**
 * fdice_flow_table_del_hw_func - Find namespace ancestor and call vtable
 * @node: flow table node being removed
 *
 * Walks up the tree to find the owning namespace's command vtable,
 * then invokes destroy_flow_table.
 */
static void fdice_flow_table_del_hw_func(struct fdice_fs_node *node)
{
	struct fdice_flow_table *ft;
	struct fdice_flow_priority *prio;
	struct fdice_flow_namespace *ns;

	ft = container_of(node, struct fdice_flow_table, node);

	if (!node->parent || node->parent->type != FDICE_FS_TYPE_PRIORITY)
		return;
	prio = container_of(node->parent, struct fdice_flow_priority, node);

	if (!prio->node.parent ||
	    prio->node.parent->type != FDICE_FS_TYPE_NAMESPACE)
		return;
	ns = container_of(prio->node.parent, struct fdice_flow_namespace, node);

	if (ns->cmds && ns->cmds->destroy_flow_table)
		ns->cmds->destroy_flow_table(ft);
}

/**
 * fdice_flow_group_del_hw_func - Release recipe and call vtable destructor
 * @node: flow group node being removed
 */
static void fdice_flow_group_del_hw_func(struct fdice_fs_node *node)
{
	struct fdice_flow_group *fg;
	struct fdice_flow_table *ft;
	struct fdice_flow_priority *prio;
	struct fdice_flow_namespace *ns;

	fg = container_of(node, struct fdice_flow_group, node);

	/* Walk up: group -> table -> priority -> namespace */
	if (!node->parent || node->parent->type != FDICE_FS_TYPE_FLOW_TABLE)
		return;
	ft = container_of(node->parent, struct fdice_flow_table, node);
	(void)ft; /* used for context in future extensions */

	if (!ft->node.parent ||
	    ft->node.parent->type != FDICE_FS_TYPE_PRIORITY)
		return;
	prio = container_of(ft->node.parent, struct fdice_flow_priority, node);

	if (!prio->node.parent ||
	    prio->node.parent->type != FDICE_FS_TYPE_NAMESPACE)
		return;
	ns = container_of(prio->node.parent, struct fdice_flow_namespace, node);

	if (ns->cmds && ns->cmds->destroy_flow_group)
		ns->cmds->destroy_flow_group(fg);
}

/**
 * fdice_flow_entry_del_hw_func - Call vtable to remove hardware rule
 * @node: flow entry node being removed
 */
static void fdice_flow_entry_del_hw_func(struct fdice_fs_node *node)
{
	struct fdice_flow_entry *fe;
	struct fdice_flow_group *fg;
	struct fdice_flow_table *ft;
	struct fdice_flow_priority *prio;
	struct fdice_flow_namespace *ns;

	fe = container_of(node, struct fdice_flow_entry, node);

	/* Walk up: entry -> group -> table -> priority -> namespace */
	if (!node->parent || node->parent->type != FDICE_FS_TYPE_FLOW_GROUP)
		return;
	fg = container_of(node->parent, struct fdice_flow_group, node);

	if (!fg->node.parent ||
	    fg->node.parent->type != FDICE_FS_TYPE_FLOW_TABLE)
		return;
	ft = container_of(fg->node.parent, struct fdice_flow_table, node);

	if (!ft->node.parent ||
	    ft->node.parent->type != FDICE_FS_TYPE_PRIORITY)
		return;
	prio = container_of(ft->node.parent, struct fdice_flow_priority, node);

	if (!prio->node.parent ||
	    prio->node.parent->type != FDICE_FS_TYPE_NAMESPACE)
		return;
	ns = container_of(prio->node.parent, struct fdice_flow_namespace, node);

	if (ns->cmds && ns->cmds->delete_flow_entry)
		ns->cmds->delete_flow_entry(fe);
}

/* -----------------------------------------------------------------------
 * Namespace and Priority Construction
 * ----------------------------------------------------------------------- */

/**
 * fdice_flow_namespace_create - Create a root namespace
 * @fdev: core device
 * @ns_type: namespace type (NIC RX, NIC TX, FDB, RDMA RX)
 * @cmds: command vtable for this namespace
 *
 * Allocates and initializes a flow namespace node. Root namespaces
 * have no parent in the tree; they are attached to the flow_steering
 * container directly.
 *
 * Return: pointer to new namespace, or ERR_PTR on failure
 */
struct fdice_flow_namespace *
fdice_flow_namespace_create(struct fdice_dev *fdev,
			    enum fdice_flow_ns_type ns_type,
			    const struct fdice_flow_cmds *cmds)
{
	struct fdice_flow_namespace *ns;

	ns = kzalloc(sizeof(*ns), GFP_KERNEL);
	if (!ns)
		return ERR_PTR(-ENOMEM);

	fdice_fs_node_init(&ns->node, FDICE_FS_TYPE_NAMESPACE, NULL);
	ns->node.del_sw_func = fdice_flow_namespace_del_sw;
	ns->dev = fdev;
	ns->cmds = cmds;
	ns->ns_type = ns_type;
	ns->num_prios = 0;

	return ns;
}

/**
 * fdice_flow_priority_create - Add a priority level to a namespace
 * @ns: parent namespace
 * @level: priority level number (lower = higher priority)
 *
 * Allocates a priority node and attaches it to the namespace.
 * The caller must not hold any locks on @ns.
 *
 * Return: pointer to new priority, or ERR_PTR on failure
 */
struct fdice_flow_priority *
fdice_flow_priority_create(struct fdice_flow_namespace *ns, int level)
{
	struct fdice_flow_priority *prio;

	prio = kzalloc(sizeof(*prio), GFP_KERNEL);
	if (!prio)
		return ERR_PTR(-ENOMEM);

	fdice_fs_node_init(&prio->node, FDICE_FS_TYPE_PRIORITY,
			   ns->node.root);
	prio->node.del_sw_func = fdice_flow_priority_del_sw;
	prio->level = level;
	prio->max_tables = 1;
	prio->num_tables = 0;

	down_write(&ns->node.lock);
	fdice_fs_node_add_child(&ns->node, &prio->node);
	ns->num_prios++;
	up_write(&ns->node.lock);

	return prio;
}

/* -----------------------------------------------------------------------
 * Flow Table Create / Destroy
 * ----------------------------------------------------------------------- */

/**
 * fdice_flow_table_create - Create a flow table at a priority level
 * @prio: parent priority node
 * @max_groups: maximum number of flow groups in this table
 *
 * Allocates a flow table, calls the command vtable's create_flow_table
 * (which is software-only for E810), and attaches it to the priority.
 *
 * Return: pointer to new table, or ERR_PTR on failure
 */
struct fdice_flow_table *
fdice_flow_table_create(struct fdice_flow_priority *prio, u32 max_groups)
{
	struct fdice_flow_namespace *ns;
	struct fdice_flow_table *ft;
	static atomic_t table_id_gen = ATOMIC_INIT(0);
	int err;

	if (!prio->node.parent ||
	    prio->node.parent->type != FDICE_FS_TYPE_NAMESPACE)
		return ERR_PTR(-EINVAL);

	ns = container_of(prio->node.parent, struct fdice_flow_namespace, node);

	ft = kzalloc(sizeof(*ft), GFP_KERNEL);
	if (!ft)
		return ERR_PTR(-ENOMEM);

	fdice_fs_node_init(&ft->node, FDICE_FS_TYPE_FLOW_TABLE,
			   ns->node.root);
	ft->node.del_hw_func = fdice_flow_table_del_hw_func;
	ft->node.del_sw_func = fdice_flow_table_del_sw;
	ft->table_id = atomic_inc_return(&table_id_gen);
	ft->priority = prio->level;
	ft->max_groups = max_groups;
	ft->num_groups = 0;
	ida_init(&ft->group_ida);

	/* Software-only table creation for E810 */
	if (ns->cmds && ns->cmds->create_flow_table) {
		err = ns->cmds->create_flow_table(ft, ns);
		if (err) {
			ida_destroy(&ft->group_ida);
			kfree(ft);
			return ERR_PTR(err);
		}
	}

	down_write(&prio->node.lock);
	fdice_fs_node_add_child(&prio->node, &ft->node);
	prio->num_tables++;
	up_write(&prio->node.lock);

	return ft;
}

/**
 * fdice_flow_table_destroy - Destroy a flow table and all its children
 * @ft: flow table to destroy
 *
 * Cascade-removes all child groups and entries, then removes the
 * table from its parent priority.
 */
void fdice_flow_table_destroy(struct fdice_flow_table *ft)
{
	struct fdice_flow_priority *prio;

	if (!ft)
		return;

	/* Remove all child groups (and their entries) first */
	fdice_fs_node_remove_children(&ft->node);

	if (ft->node.parent &&
	    ft->node.parent->type == FDICE_FS_TYPE_PRIORITY) {
		prio = container_of(ft->node.parent,
				    struct fdice_flow_priority, node);
		down_write(&prio->node.lock);
		prio->num_tables--;
		fdice_fs_node_remove(&ft->node);
		up_write(&prio->node.lock);
	}

	fdice_fs_node_put(&ft->node);
}

/* -----------------------------------------------------------------------
 * Flow Group Create / Destroy
 * ----------------------------------------------------------------------- */

/**
 * fdice_flow_group_create - Create a flow group with match criteria
 * @ft: parent flow table
 * @mask: match criteria defining extraction pattern
 * @match_criteria_enable: bitmask of active criteria categories
 * @max_entries: maximum entries in this group
 *
 * Allocates a flow group, binds it to an E810 recipe (shared or new)
 * via the command vtable, and attaches it to the table.
 *
 * Return: pointer to new group, or ERR_PTR on failure
 */
struct fdice_flow_group *
fdice_flow_group_create(struct fdice_flow_table *ft,
			const struct fdice_match_criteria *mask,
			u32 match_criteria_enable,
			u32 max_entries)
{
	struct fdice_flow_group *fg;
	struct fdice_flow_priority *prio;
	struct fdice_flow_namespace *ns;
	int err;

	if (ft->num_groups >= ft->max_groups)
		return ERR_PTR(-ENOSPC);

	/* Walk up to find the namespace for the command vtable */
	if (!ft->node.parent ||
	    ft->node.parent->type != FDICE_FS_TYPE_PRIORITY)
		return ERR_PTR(-EINVAL);
	prio = container_of(ft->node.parent, struct fdice_flow_priority, node);

	if (!prio->node.parent ||
	    prio->node.parent->type != FDICE_FS_TYPE_NAMESPACE)
		return ERR_PTR(-EINVAL);
	ns = container_of(prio->node.parent, struct fdice_flow_namespace, node);

	fg = kzalloc(sizeof(*fg), GFP_KERNEL);
	if (!fg)
		return ERR_PTR(-ENOMEM);

	fdice_fs_node_init(&fg->node, FDICE_FS_TYPE_FLOW_GROUP,
			   ft->node.root);
	fg->node.del_hw_func = fdice_flow_group_del_hw_func;
	fg->node.del_sw_func = fdice_flow_group_del_sw;
	fg->mask = *mask;
	fg->match_criteria_enable = match_criteria_enable;
	fg->max_entries = max_entries;
	fg->num_entries = 0;
	fg->recipe_id = 0;
	fg->recipe = NULL;
	fg->is_autogroup = false;
	ida_init(&fg->entry_ida);

	/* Program recipe via command vtable */
	if (ns->cmds && ns->cmds->create_flow_group) {
		err = ns->cmds->create_flow_group(fg, ft);
		if (err) {
			ida_destroy(&fg->entry_ida);
			kfree(fg);
			return ERR_PTR(err);
		}
	}

	down_write(&ft->node.lock);
	fdice_fs_node_add_child(&ft->node, &fg->node);
	ft->num_groups++;
	up_write(&ft->node.lock);

	return fg;
}

/**
 * fdice_flow_group_destroy - Destroy a flow group and release recipe
 * @fg: flow group to destroy
 *
 * Cascade-removes all child entries, releases the recipe reference,
 * then removes the group from its parent table.
 */
void fdice_flow_group_destroy(struct fdice_flow_group *fg)
{
	struct fdice_flow_table *ft;

	if (!fg)
		return;

	/* Remove all child entries first */
	fdice_fs_node_remove_children(&fg->node);

	if (fg->node.parent &&
	    fg->node.parent->type == FDICE_FS_TYPE_FLOW_TABLE) {
		ft = container_of(fg->node.parent,
				  struct fdice_flow_table, node);
		down_write(&ft->node.lock);
		ft->num_groups--;
		fdice_fs_node_remove(&fg->node);
		up_write(&ft->node.lock);
	}

	fdice_fs_node_put(&fg->node);
}

/* -----------------------------------------------------------------------
 * Flow Entry Create / Destroy
 * ----------------------------------------------------------------------- */

/**
 * fdice_flow_entry_create - Create an individual match+action flow rule
 * @fg: parent flow group
 * @match: match values for this entry
 * @action: action specification
 *
 * Allocates a flow entry and programs it into hardware via the
 * command vtable's create_flow_entry.
 *
 * Return: pointer to new entry, or ERR_PTR on failure
 */
struct fdice_flow_entry *
fdice_flow_entry_create(struct fdice_flow_group *fg,
			const struct fdice_match_values *match,
			struct fdice_flow_action *action)
{
	struct fdice_flow_entry *fe;
	struct fdice_flow_table *ft;
	struct fdice_flow_priority *prio;
	struct fdice_flow_namespace *ns;
	int err;

	if (fg->num_entries >= fg->max_entries)
		return ERR_PTR(-ENOSPC);

	/* Walk up to find the namespace */
	if (!fg->node.parent ||
	    fg->node.parent->type != FDICE_FS_TYPE_FLOW_TABLE)
		return ERR_PTR(-EINVAL);
	ft = container_of(fg->node.parent, struct fdice_flow_table, node);

	if (!ft->node.parent ||
	    ft->node.parent->type != FDICE_FS_TYPE_PRIORITY)
		return ERR_PTR(-EINVAL);
	prio = container_of(ft->node.parent, struct fdice_flow_priority, node);

	if (!prio->node.parent ||
	    prio->node.parent->type != FDICE_FS_TYPE_NAMESPACE)
		return ERR_PTR(-EINVAL);
	ns = container_of(prio->node.parent, struct fdice_flow_namespace, node);

	fe = kzalloc(sizeof(*fe), GFP_KERNEL);
	if (!fe)
		return ERR_PTR(-ENOMEM);

	fdice_fs_node_init(&fe->node, FDICE_FS_TYPE_FLOW_ENTRY,
			   fg->node.root);
	fe->node.del_hw_func = fdice_flow_entry_del_hw_func;
	fe->node.del_sw_func = fdice_flow_entry_del_sw;
	fe->match = *match;
	fe->action = *action;
	fe->cookie = 0;
	fe->packets = 0;
	fe->bytes = 0;

	/* Program rule into hardware via AQ */
	if (ns->cmds && ns->cmds->create_flow_entry) {
		err = ns->cmds->create_flow_entry(fe, fg);
		if (err) {
			kfree(fe);
			return ERR_PTR(err);
		}
	}

	down_write(&fg->node.lock);
	fdice_fs_node_add_child(&fg->node, &fe->node);
	fg->num_entries++;
	up_write(&fg->node.lock);

	return fe;
}

/**
 * fdice_flow_entry_update - Update an existing flow entry's action
 * @fe: flow entry to update
 * @action: new action specification
 *
 * Updates the entry's action in software and reprograms the hardware
 * rule via the command vtable's update_flow_entry.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_flow_entry_update(struct fdice_flow_entry *fe,
			    struct fdice_flow_action *action)
{
	struct fdice_flow_group *fg;
	struct fdice_flow_table *ft;
	struct fdice_flow_priority *prio;
	struct fdice_flow_namespace *ns;
	int err;

	if (!fe->node.parent ||
	    fe->node.parent->type != FDICE_FS_TYPE_FLOW_GROUP)
		return -EINVAL;
	fg = container_of(fe->node.parent, struct fdice_flow_group, node);

	if (!fg->node.parent ||
	    fg->node.parent->type != FDICE_FS_TYPE_FLOW_TABLE)
		return -EINVAL;
	ft = container_of(fg->node.parent, struct fdice_flow_table, node);

	if (!ft->node.parent ||
	    ft->node.parent->type != FDICE_FS_TYPE_PRIORITY)
		return -EINVAL;
	prio = container_of(ft->node.parent, struct fdice_flow_priority, node);

	if (!prio->node.parent ||
	    prio->node.parent->type != FDICE_FS_TYPE_NAMESPACE)
		return -EINVAL;
	ns = container_of(prio->node.parent, struct fdice_flow_namespace, node);

	fe->action = *action;

	if (ns->cmds && ns->cmds->update_flow_entry) {
		err = ns->cmds->update_flow_entry(fe);
		if (err)
			return err;
	}

	return 0;
}

/**
 * fdice_flow_entry_destroy - Destroy a flow entry
 * @fe: flow entry to destroy
 *
 * Removes the entry from hardware and its parent group.
 */
void fdice_flow_entry_destroy(struct fdice_flow_entry *fe)
{
	struct fdice_flow_group *fg;

	if (!fe)
		return;

	if (fe->node.parent &&
	    fe->node.parent->type == FDICE_FS_TYPE_FLOW_GROUP) {
		fg = container_of(fe->node.parent,
				  struct fdice_flow_group, node);
		down_write(&fg->node.lock);
		fg->num_entries--;
		fdice_fs_node_remove(&fe->node);
		up_write(&fg->node.lock);
	}

	fdice_fs_node_put(&fe->node);
}

/* -----------------------------------------------------------------------
 * Priority Lookup and Autogroup Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_flow_namespace_find_prio - Find a priority node by level
 * @ns: namespace to search
 * @level: priority level number to find
 *
 * Walks the namespace's children list to find the priority node at
 * the requested level. Caller must not hold any locks on @ns.
 *
 * Return: pointer to the priority node, or NULL if not found
 */
struct fdice_flow_priority *
fdice_flow_namespace_find_prio(struct fdice_flow_namespace *ns, int level)
{
	struct fdice_fs_node *child;
	struct fdice_flow_priority *prio;

	if (!ns)
		return NULL;

	down_read(&ns->node.lock);
	list_for_each_entry(child, &ns->node.children, sibling) {
		if (child->type != FDICE_FS_TYPE_PRIORITY)
			continue;
		prio = container_of(child, struct fdice_flow_priority, node);
		if (prio->level == level) {
			up_read(&ns->node.lock);
			return prio;
		}
	}
	up_read(&ns->node.lock);

	return NULL;
}

/**
 * fdice_flow_table_find_or_create - Find existing or create new flow table
 * @prio: priority node to search/create under
 * @max_groups: maximum groups if creating a new table
 *
 * Returns the first existing flow table at this priority, or creates
 * a new one if none exists.
 *
 * Return: pointer to flow table, or ERR_PTR on failure
 */
struct fdice_flow_table *
fdice_flow_table_find_or_create(struct fdice_flow_priority *prio,
				u32 max_groups)
{
	struct fdice_fs_node *child;
	struct fdice_flow_table *ft;

	if (!prio)
		return ERR_PTR(-EINVAL);

	down_read(&prio->node.lock);
	list_for_each_entry(child, &prio->node.children, sibling) {
		if (child->type != FDICE_FS_TYPE_FLOW_TABLE)
			continue;
		ft = container_of(child, struct fdice_flow_table, node);
		up_read(&prio->node.lock);
		return ft;
	}
	up_read(&prio->node.lock);

	return fdice_flow_table_create(prio, max_groups);
}

/**
 * fdice_flow_entry_create_autogroup - Create flow entry with automatic grouping
 * @ns: namespace in which to create the entry
 * @prio_level: priority level within the namespace
 * @match: match values for the flow entry
 * @criteria: match criteria bitmask (determines flow group selection)
 * @action: action specification for the entry
 *
 * High-level entry creation that handles the full tree path: finds (or
 * creates) the flow table at the given priority, finds (or creates) a
 * flow group matching the criteria, then creates the flow entry.
 *
 * This is the primary API for TC flower, FDIR, and TTC callers that
 * do not want to manage the table/group hierarchy directly.
 *
 * Return: pointer to the created flow entry, or ERR_PTR on failure
 */
struct fdice_flow_entry *
fdice_flow_entry_create_autogroup(struct fdice_flow_namespace *ns,
				  int prio_level,
				  const struct fdice_match_values *match,
				  const struct fdice_match_criteria *criteria,
				  struct fdice_flow_action *action)
{
	struct fdice_flow_priority *prio;
	struct fdice_flow_table *ft;
	struct fdice_flow_group *fg;
	struct fdice_fs_node *child;
	u32 match_enable;

	prio = fdice_flow_namespace_find_prio(ns, prio_level);
	if (!prio)
		return ERR_PTR(-ENOENT);

	ft = fdice_flow_table_find_or_create(prio, 256);
	if (IS_ERR(ft))
		return ERR_CAST(ft);

	/* Build match_criteria_enable from which criteria categories are set */
	match_enable = 0;
	if (criteria->outer_headers)
		match_enable |= FDICE_MATCH_OUTER_HEADERS;
	if (criteria->inner_headers)
		match_enable |= FDICE_MATCH_INNER_HEADERS;
	if (criteria->misc)
		match_enable |= FDICE_MATCH_MISC;

	/*
	 * Search for an existing flow group with matching criteria.
	 * This enables recipe sharing across rules with the same
	 * extraction pattern.
	 */
	down_read(&ft->node.lock);
	list_for_each_entry(child, &ft->node.children, sibling) {
		if (child->type != FDICE_FS_TYPE_FLOW_GROUP)
			continue;
		fg = container_of(child, struct fdice_flow_group, node);
		if (fg->match_criteria_enable == match_enable &&
		    fg->mask.outer_headers == criteria->outer_headers &&
		    fg->mask.inner_headers == criteria->inner_headers &&
		    fg->mask.misc == criteria->misc &&
		    fg->num_entries < fg->max_entries) {
			up_read(&ft->node.lock);
			return fdice_flow_entry_create(fg, match, action);
		}
	}
	up_read(&ft->node.lock);

	/* No matching group found -- create a new autogroup */
	fg = fdice_flow_group_create(ft, criteria, match_enable, 256);
	if (IS_ERR(fg))
		return ERR_CAST(fg);

	fg->is_autogroup = true;

	return fdice_flow_entry_create(fg, match, action);
}

/* -----------------------------------------------------------------------
 * Namespace Pipeline Initialization Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_fs_init_nic_rx_ns - Create NIC RX namespace with priority pipeline
 * @fdev: core device
 * @fs: flow steering root container
 *
 * Builds the NIC RX namespace with six priority levels:
 * TC offload, FDIR, ADQ, VLAN, L2 MAC, RSS/default.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_fs_init_nic_rx_ns(struct fdice_dev *fdev,
				   struct fdice_flow_steering *fs)
{
	struct fdice_flow_namespace *ns;
	struct fdice_flow_priority *prio;
	int i;

	ns = fdice_flow_namespace_create(fdev, FDICE_FLOW_NS_NIC_RX,
					 fs->cmds);
	if (IS_ERR(ns))
		return PTR_ERR(ns);

	fs->nic_rx_ns = ns;

	for (i = 0; i < FDICE_FS_NIC_RX_NUM_PRIOS; i++) {
		prio = fdice_flow_priority_create(ns, i);
		if (IS_ERR(prio))
			return PTR_ERR(prio);
	}

	return 0;
}

/**
 * fdice_fs_init_nic_tx_ns - Create NIC TX namespace
 * @fdev: core device
 * @fs: flow steering root container
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_fs_init_nic_tx_ns(struct fdice_dev *fdev,
				   struct fdice_flow_steering *fs)
{
	struct fdice_flow_namespace *ns;
	struct fdice_flow_priority *prio;
	int i;

	ns = fdice_flow_namespace_create(fdev, FDICE_FLOW_NS_NIC_TX,
					 fs->cmds);
	if (IS_ERR(ns))
		return PTR_ERR(ns);

	fs->nic_tx_ns = ns;

	for (i = 0; i < FDICE_FS_NIC_TX_NUM_PRIOS; i++) {
		prio = fdice_flow_priority_create(ns, i);
		if (IS_ERR(prio))
			return PTR_ERR(prio);
	}

	return 0;
}

/**
 * fdice_fs_init_fdb_ns - Create FDB (eSwitch) namespace
 * @fdev: core device
 * @fs: flow steering root container
 *
 * Builds the FDB namespace with three priority levels:
 * TC flower on representors, bridge FDB, default forwarding.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_fs_init_fdb_ns(struct fdice_dev *fdev,
				struct fdice_flow_steering *fs)
{
	struct fdice_flow_namespace *ns;
	struct fdice_flow_priority *prio;
	int i;

	ns = fdice_flow_namespace_create(fdev, FDICE_FLOW_NS_FDB,
					 fs->cmds);
	if (IS_ERR(ns))
		return PTR_ERR(ns);

	fs->fdb_ns = ns;

	for (i = 0; i < FDICE_FS_FDB_NUM_PRIOS; i++) {
		prio = fdice_flow_priority_create(ns, i);
		if (IS_ERR(prio))
			return PTR_ERR(prio);
	}

	return 0;
}

/* -----------------------------------------------------------------------
 * Top-Level Flow Steering Init / Cleanup
 * ----------------------------------------------------------------------- */

/**
 * fdice_flow_steering_init - Initialize the flow steering subsystem
 * @fdev: core device
 *
 * Allocates the root flow_steering container, initializes the recipe
 * manager, creates all root namespaces with their priority pipelines,
 * and installs the E810 command vtable.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_flow_steering_init(struct fdice_dev *fdev)
{
	struct fdice_flow_steering *fs;
	int err;

	fs = kzalloc(sizeof(*fs), GFP_KERNEL);
	if (!fs)
		return -ENOMEM;

	fs->dev = fdev;
	fs->cmds = &fdice_e810_flow_cmds;

	/* Initialize recipe manager */
	fs->recipe_mgr = kzalloc(sizeof(*fs->recipe_mgr), GFP_KERNEL);
	if (!fs->recipe_mgr) {
		err = -ENOMEM;
		goto err_free_fs;
	}

	err = fdice_recipe_mgr_init(fs->recipe_mgr, fdev);
	if (err)
		goto err_free_recipe_mgr;

	fdev->flow_steering = fs;

	/* Create root namespaces */
	err = fdice_fs_init_nic_rx_ns(fdev, fs);
	if (err)
		goto err_cleanup;

	err = fdice_fs_init_nic_tx_ns(fdev, fs);
	if (err)
		goto err_cleanup;

	err = fdice_fs_init_fdb_ns(fdev, fs);
	if (err)
		goto err_cleanup;

	dev_info(&fdev->pdev->dev, "flow steering initialized\n");
	return 0;

err_cleanup:
	fdice_flow_steering_cleanup(fdev);
	return err;

err_free_recipe_mgr:
	kfree(fs->recipe_mgr);
err_free_fs:
	kfree(fs);
	return err;
}

/**
 * fdice_flow_steering_cleanup - Tear down the flow steering subsystem
 * @fdev: core device
 *
 * Removes all namespaces (cascading to all children), cleans up the
 * recipe manager, and frees the root container.
 */
void fdice_flow_steering_cleanup(struct fdice_dev *fdev)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;

	if (!fs)
		return;

	/* Tear down namespaces in reverse order */
	if (fs->fdb_ns) {
		fdice_fs_node_remove_children(&fs->fdb_ns->node);
		fdice_fs_node_put(&fs->fdb_ns->node);
		fs->fdb_ns = NULL;
	}

	if (fs->nic_tx_ns) {
		fdice_fs_node_remove_children(&fs->nic_tx_ns->node);
		fdice_fs_node_put(&fs->nic_tx_ns->node);
		fs->nic_tx_ns = NULL;
	}

	if (fs->nic_rx_ns) {
		fdice_fs_node_remove_children(&fs->nic_rx_ns->node);
		fdice_fs_node_put(&fs->nic_rx_ns->node);
		fs->nic_rx_ns = NULL;
	}

	if (fs->recipe_mgr) {
		fdice_recipe_mgr_cleanup(fs->recipe_mgr);
		kfree(fs->recipe_mgr);
	}

	fdev->flow_steering = NULL;
	kfree(fs);
}
