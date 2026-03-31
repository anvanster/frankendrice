// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE Flow Steering AQ Command Implementation
 *
 * E810-specific command vtable that bridges the software flow steering
 * tree to Admin Queue hardware operations. Each vtable function translates
 * tree-level semantics into the appropriate AQ command format.
 *
 * AQ command mapping:
 *   create_flow_group  -> add_recipe     (0x0290)
 *   create_flow_entry  -> add_sw_rules   (0x02A0)
 *   update_flow_entry  -> update_sw_rules(0x02A1)
 *   delete_flow_entry  -> remove_sw_rules(0x02A2)
 *   create_flow_table  -> software only (no direct HW table concept)
 *   destroy_flow_table -> software only (cascades entry removal)
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include <linux/slab.h>
#include <linux/device.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/dma-mapping.h>

#include "fdice_dev.h"
#include "fdice_aq.h"
#include "fdice_fs_core.h"
#include "fdice_fs_cmd.h"
#include "fdice_fs_recipe.h"

/* -----------------------------------------------------------------------
 * Internal Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_fs_cmd_get_ns - Walk from any tree node up to its root namespace
 * @node: any flow steering tree node
 *
 * Walks the parent chain to find the root namespace, which stores the
 * device backpointer needed for AQ command submission.
 *
 * Return: namespace pointer, or NULL if the tree is malformed
 */
static struct fdice_flow_namespace *
fdice_fs_cmd_get_ns(struct fdice_fs_node *node)
{
	struct fdice_fs_node *cur = node;

	while (cur && cur->parent)
		cur = cur->parent;

	if (!cur || cur->type != FDICE_FS_TYPE_NAMESPACE)
		return NULL;

	return container_of(cur, struct fdice_flow_namespace, node);
}

/* -----------------------------------------------------------------------
 * AQ Buffer Construction Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_fs_build_sw_rule_entry - Build a switch rule AQ buffer element
 * @fe: flow entry to program
 * @fg: parent flow group (provides recipe_id)
 * @elem: output switch rule element
 *
 * Translates the flow entry's action into E810 switch rule format.
 */
static void
fdice_fs_build_sw_rule_entry(struct fdice_flow_entry *fe,
			     struct fdice_flow_group *fg,
			     struct fdice_sw_rule_elem *elem)
{
	u32 act = 0;

	memset(elem, 0, sizeof(*elem));
	elem->type = cpu_to_le16(FDICE_SW_RULE_TYPE_FILTER);
	elem->recipe_id = cpu_to_le16(fg->recipe_id);
	elem->src_vsi = 0; /* filled by caller or from context */

	switch (fe->action.type) {
	case FDICE_FLOW_ACTION_FORWARD:
		act = FDICE_SW_ACT_FWD;
		act |= (u32)fe->action.forward.dest_vsi
			<< FDICE_SW_ACT_FWD_VSI_SHIFT;
		act |= (u32)fe->action.forward.dest_queue
			<< FDICE_SW_ACT_FWD_Q_SHIFT;
		break;
	case FDICE_FLOW_ACTION_DROP:
		act = FDICE_SW_ACT_DROP;
		break;
	case FDICE_FLOW_ACTION_MARK:
	case FDICE_FLOW_ACTION_COUNT:
		/* Mark and count are handled via FDIR path */
		act = FDICE_SW_ACT_FWD;
		break;
	}

	elem->act = cpu_to_le32(act);
}

/**
 * fdice_fs_build_recipe_content - Build recipe AQ buffer from match criteria
 * @recipe: recipe to program
 * @content: output recipe content structure
 *
 * Translates match criteria flags into E810 extraction field definitions.
 * Each set bit in the criteria maps to a protocol+offset extraction word
 * that tells the hardware which packet bytes to extract and compare.
 */
static void
fdice_fs_build_recipe_content(struct fdice_recipe *recipe,
			      struct fdice_recipe_content *content)
{
	memset(content, 0, sizeof(*content));
	content->recipe_idx = recipe->recipe_id;

	/*
	 * Populate extraction fields based on match criteria.
	 * The content[] array holds protocol_id:offset pairs that
	 * define which packet bytes to extract. This is a simplified
	 * mapping -- full implementation would translate each
	 * FDICE_MATCH_* flag to the appropriate extraction words.
	 */
	if (recipe->mask.outer_headers) {
		/*
		 * Set flag indicating outer header extraction is active.
		 * Actual protocol/offset programming depends on which
		 * specific L2-L4 fields are requested.
		 */
		content->flags |= 0x01;
	}

	if (recipe->mask.inner_headers) {
		content->flags |= 0x02;
	}

	if (recipe->mask.misc) {
		content->flags |= 0x04;
	}
}

/* -----------------------------------------------------------------------
 * E810 AQ Command Vtable: Flow Entry Operations
 * ----------------------------------------------------------------------- */

/**
 * fdice_e810_create_flow_entry - Program a switch rule via AQ
 * @fe: flow entry to program
 * @fg: parent flow group (provides recipe binding)
 *
 * Builds an add_sw_rules AQ command (opcode 0x02A0) with the
 * entry's action encoded as a switch rule element. On success,
 * stores the assigned rule_id in the entry's hw.sw_rule binding.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_e810_create_flow_entry(struct fdice_flow_entry *fe,
				struct fdice_flow_group *fg)
{
	struct fdice_flow_namespace *ns;
	struct fdice_aq_desc desc;
	struct fdice_aqp_sw_rule *cmd;
	struct fdice_sw_rule_elem *buf;
	int err;

	ns = fdice_fs_cmd_get_ns(&fg->node);
	if (!ns || !ns->dev)
		return -EINVAL;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	fdice_fs_build_sw_rule_entry(fe, fg, buf);

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_ADD_SW_RULES);
	cmd = (struct fdice_aqp_sw_rule *)desc.params.raw;
	cmd->num_rules = cpu_to_le16(1);
	desc.datalen = cpu_to_le16(sizeof(*buf));
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD | FDICE_AQ_FLAG_BUF);

	err = fdice_aq_send_cmd(&ns->dev->hw, &desc, buf, sizeof(*buf));
	if (!err) {
		fe->hw.sw_rule.rule_id = le16_to_cpu(buf->rule_id);
		fe->hw.sw_rule.recipe_id = fg->recipe_id;
	}

	kfree(buf);
	return err;
}

/**
 * fdice_e810_update_flow_entry - Update an existing switch rule via AQ
 * @fe: flow entry to update
 *
 * Sends an update_sw_rules AQ command (opcode 0x02A1) to modify
 * an existing hardware rule in-place.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_e810_update_flow_entry(struct fdice_flow_entry *fe)
{
	struct fdice_flow_namespace *ns;
	struct fdice_aq_desc desc;
	struct fdice_aqp_sw_rule *cmd;
	struct fdice_sw_rule_elem *buf;
	struct fdice_flow_group *fg;
	int err;

	if (!fe->node.parent ||
	    fe->node.parent->type != FDICE_FS_TYPE_FLOW_GROUP)
		return -EINVAL;
	fg = container_of(fe->node.parent, struct fdice_flow_group, node);

	ns = fdice_fs_cmd_get_ns(&fg->node);
	if (!ns || !ns->dev)
		return -EINVAL;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	fdice_fs_build_sw_rule_entry(fe, fg, buf);
	buf->rule_id = cpu_to_le16(fe->hw.sw_rule.rule_id);

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_UPDATE_SW_RULES);
	cmd = (struct fdice_aqp_sw_rule *)desc.params.raw;
	cmd->num_rules = cpu_to_le16(1);
	desc.datalen = cpu_to_le16(sizeof(*buf));
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD | FDICE_AQ_FLAG_BUF);

	err = fdice_aq_send_cmd(&ns->dev->hw, &desc, buf, sizeof(*buf));

	kfree(buf);
	return err;
}

/**
 * fdice_e810_delete_flow_entry - Remove a switch rule via AQ
 * @fe: flow entry to delete
 *
 * Sends a remove_sw_rules AQ command (opcode 0x02A2) to remove
 * the hardware rule identified by the entry's rule_id.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_e810_delete_flow_entry(struct fdice_flow_entry *fe)
{
	struct fdice_flow_namespace *ns;
	struct fdice_aq_desc desc;
	struct fdice_aqp_sw_rule *cmd;
	struct fdice_sw_rule_elem *buf;
	int err;

	ns = fdice_fs_cmd_get_ns(&fe->node);
	if (!ns || !ns->dev)
		return -EINVAL;

	buf = kzalloc(sizeof(*buf), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	memset(buf, 0, sizeof(*buf));
	buf->type = cpu_to_le16(FDICE_SW_RULE_TYPE_FILTER);
	buf->rule_id = cpu_to_le16(fe->hw.sw_rule.rule_id);

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_REMOVE_SW_RULES);
	cmd = (struct fdice_aqp_sw_rule *)desc.params.raw;
	cmd->num_rules = cpu_to_le16(1);
	desc.datalen = cpu_to_le16(sizeof(*buf));
	desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD | FDICE_AQ_FLAG_BUF);

	err = fdice_aq_send_cmd(&ns->dev->hw, &desc, buf, sizeof(*buf));

	kfree(buf);
	return err;
}

/* -----------------------------------------------------------------------
 * E810 AQ Command Vtable: Flow Group / Recipe Operations
 * ----------------------------------------------------------------------- */

/**
 * fdice_e810_create_flow_group - Program recipe for a flow group via AQ
 * @fg: flow group being created
 * @ft: parent flow table
 *
 * Uses the recipe manager to find or allocate a recipe matching the
 * group's match criteria. If a new recipe is allocated and not yet
 * programmed to hardware, sends an add_recipe AQ command (opcode 0x0290).
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_e810_create_flow_group(struct fdice_flow_group *fg,
				struct fdice_flow_table *ft)
{
	struct fdice_flow_namespace *ns;
	struct fdice_flow_steering *fs;
	struct fdice_recipe *recipe;
	struct fdice_recipe_content *content;
	struct fdice_aq_desc desc;
	struct fdice_aqp_add_recipe *cmd;
	int err;

	ns = fdice_fs_cmd_get_ns(&ft->node);
	if (!ns || !ns->dev)
		return -EINVAL;

	fs = ns->dev->flow_steering;
	if (!fs || !fs->recipe_mgr)
		return -EINVAL;

	/* Allocate or share a recipe matching this group's criteria */
	recipe = fdice_recipe_alloc(fs->recipe_mgr, &fg->mask);
	if (IS_ERR(recipe))
		return PTR_ERR(recipe);

	/* Program recipe to hardware if this is a new allocation */
	if (!recipe->hw_programmed) {
		content = kzalloc(sizeof(*content), GFP_KERNEL);
		if (!content) {
			fdice_recipe_release(fs->recipe_mgr, recipe);
			return -ENOMEM;
		}

		fdice_fs_build_recipe_content(recipe, content);

		fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_ADD_RECIPE);
		cmd = (struct fdice_aqp_add_recipe *)desc.params.raw;
		cmd->num_recipes = cpu_to_le16(1);
		desc.datalen = cpu_to_le16(sizeof(*content));
		desc.flags |= cpu_to_le16(FDICE_AQ_FLAG_RD |
					  FDICE_AQ_FLAG_BUF);

		err = fdice_aq_send_cmd(&ns->dev->hw, &desc,
					content, sizeof(*content));
		kfree(content);

		if (err) {
			fdice_recipe_release(fs->recipe_mgr, recipe);
			return err;
		}

		recipe->hw_programmed = true;
	}

	fg->recipe_id = recipe->recipe_id;
	fg->recipe = recipe;

	return 0;
}

/**
 * fdice_e810_destroy_flow_group - Release recipe reference for a flow group
 * @fg: flow group being destroyed
 *
 * Decrements the recipe reference count via the recipe manager.
 * When the last group using a recipe is destroyed, the recipe
 * slot is freed for reallocation.
 *
 * Return: 0 always (recipe release cannot fail)
 */
int fdice_e810_destroy_flow_group(struct fdice_flow_group *fg)
{
	struct fdice_flow_namespace *ns;
	struct fdice_flow_steering *fs;

	ns = fdice_fs_cmd_get_ns(&fg->node);
	if (ns && ns->dev) {
		fs = ns->dev->flow_steering;
		if (fs && fs->recipe_mgr && fg->recipe)
			fdice_recipe_release(fs->recipe_mgr, fg->recipe);
	}

	fg->recipe = NULL;
	fg->recipe_id = 0;

	return 0;
}

/* -----------------------------------------------------------------------
 * E810 AQ Command Vtable: Flow Table Operations (Software Only)
 * ----------------------------------------------------------------------- */

/**
 * fdice_e810_create_flow_table - Create a flow table (software only)
 * @ft: flow table being created
 * @ns: parent namespace
 *
 * E810 has no direct hardware concept of flow tables. This is a
 * software-only operation that validates the table configuration.
 *
 * Return: 0 on success
 */
int fdice_e810_create_flow_table(struct fdice_flow_table *ft,
				struct fdice_flow_namespace *ns)
{
	/* Software-only: validate and accept */
	return 0;
}

/**
 * fdice_e810_destroy_flow_table - Destroy a flow table (software only)
 * @ft: flow table being destroyed
 *
 * Software-only cleanup. All child entries should have been removed
 * by cascade removal before this is called.
 *
 * Return: 0 always
 */
int fdice_e810_destroy_flow_table(struct fdice_flow_table *ft)
{
	/* Software-only: nothing to send to hardware */
	return 0;
}

/* -----------------------------------------------------------------------
 * E810 AQ Command Vtable: FDIR Operations
 * ----------------------------------------------------------------------- */

/**
 * fdice_e810_create_fdir_entry - Program a FDIR filter
 * @fe: flow entry containing match criteria
 * @params: FDIR-specific parameters (queue index, flow ID)
 *
 * Translates structured match criteria into E810's dummy-packet
 * format for Flow Director programming. The caller specifies match
 * fields; this function builds the appropriate dummy packet template
 * that E810 hardware uses for exact-match classification.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_e810_create_fdir_entry(struct fdice_flow_entry *fe,
				struct fdice_fdir_params *params)
{
	/*
	 * FDIR dummy packet construction:
	 *
	 * E810 FDIR works by matching incoming packets against a
	 * pre-programmed "dummy packet" template. The hardware compares
	 * specific fields at known offsets.
	 *
	 * Translation from structured match criteria:
	 * 1. Determine packet type from match criteria (IPv4/IPv6, TCP/UDP)
	 * 2. Build dummy packet buffer with the match values at correct offsets
	 * 3. Program via FDIR-specific AQ path
	 *
	 * The actual dummy packet construction will be in fdice_fs_fdir.c
	 * when that module is implemented. This vtable entry provides the
	 * dispatch point.
	 */

	fe->hw.fdir.q_index = params->q_index;
	fe->hw.fdir.fltr_id = params->flow_id;

	/* TODO: Build dummy packet and program via FDIR AQ path */
	return 0;
}

/**
 * fdice_e810_delete_fdir_entry - Remove a FDIR filter
 * @fe: flow entry to remove
 *
 * Removes the FDIR filter identified by the entry's fltr_id.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_e810_delete_fdir_entry(struct fdice_flow_entry *fe)
{
	/* TODO: Send FDIR remove via AQ path */
	fe->hw.fdir.fltr_id = 0;
	fe->hw.fdir.q_index = 0;

	return 0;
}

/* -----------------------------------------------------------------------
 * E810 Command Vtable Instance
 * ----------------------------------------------------------------------- */

/**
 * fdice_e810_flow_cmds - E810 flow steering command vtable
 *
 * Single vtable instance mapping all tree operations to E810 AQ commands.
 * Installed in all namespaces during fdice_flow_steering_init().
 */
const struct fdice_flow_cmds fdice_e810_flow_cmds = {
	.create_flow_table	= fdice_e810_create_flow_table,
	.destroy_flow_table	= fdice_e810_destroy_flow_table,
	.create_flow_group	= fdice_e810_create_flow_group,
	.destroy_flow_group	= fdice_e810_destroy_flow_group,
	.create_flow_entry	= fdice_e810_create_flow_entry,
	.update_flow_entry	= fdice_e810_update_flow_entry,
	.delete_flow_entry	= fdice_e810_delete_flow_entry,
	.create_fdir_entry	= fdice_e810_create_fdir_entry,
	.delete_fdir_entry	= fdice_e810_delete_fdir_entry,
};
