// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_FS_CMD_H_
#define _FDICE_FS_CMD_H_

/**
 * DOC: FrankenDrICE Flow Steering Command Header
 *
 * Defines the command vtable interface that bridges the software
 * flow steering tree to E810 Admin Queue hardware operations.
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include "fdice_fs_core.h"

/* -----------------------------------------------------------------------
 * AQ Opcodes for Switch / Recipe / FDIR Operations
 * ----------------------------------------------------------------------- */

#define FDICE_AQ_OPC_ADD_RECIPE		0x0290
#define FDICE_AQ_OPC_ADD_SW_RULES	0x02A0
#define FDICE_AQ_OPC_UPDATE_SW_RULES	0x02A1
#define FDICE_AQ_OPC_REMOVE_SW_RULES	0x02A2

/* -----------------------------------------------------------------------
 * AQ Descriptor Parameter Layouts for Flow Commands
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_aqp_sw_rule - Parameters for switch rule AQ commands
 * @flags: command flags
 * @num_rules: number of rules in the indirect buffer
 * @rsvd: reserved
 * @addr_high: high 32 bits of indirect buffer DMA address
 * @addr_low: low 32 bits of indirect buffer DMA address
 *
 * Used with opcodes 0x02A0, 0x02A1, 0x02A2.
 */
struct fdice_aqp_sw_rule {
	__le16	flags;
	__le16	num_rules;
	__le32	rsvd;
	__le32	addr_high;
	__le32	addr_low;
} __packed;

/**
 * struct fdice_aqp_add_recipe - Parameters for add recipe AQ command
 * @flags: command flags
 * @num_recipes: number of recipes in indirect buffer
 * @rsvd: reserved
 * @addr_high: high 32 bits of indirect buffer DMA address
 * @addr_low: low 32 bits of indirect buffer DMA address
 *
 * Used with opcode 0x0290.
 */
struct fdice_aqp_add_recipe {
	__le16	flags;
	__le16	num_recipes;
	__le32	rsvd;
	__le32	addr_high;
	__le32	addr_low;
} __packed;

/**
 * struct fdice_sw_rule_elem - Single switch rule element in AQ buffer
 * @type: rule element type
 * @rsvd: reserved
 * @recipe_id: recipe ID for matching
 * @src_vsi: source VSI
 * @act: action data (forward VSI, queue, drop, etc.)
 * @rule_id: assigned rule ID (response field)
 * @rsvd2: reserved trailing bytes
 *
 * Packed element in the indirect buffer for switch rule AQ commands.
 */
struct fdice_sw_rule_elem {
	__le16	type;
	u8	rsvd[2];
	__le16	recipe_id;
	__le16	src_vsi;
	__le32	act;
	__le16	rule_id;
	u8	rsvd2[2];
} __packed;

/* Switch rule element types */
#define FDICE_SW_RULE_TYPE_FILTER	0x0001

/* Switch rule action encoding */
#define FDICE_SW_ACT_FWD_VSI_SHIFT	4
#define FDICE_SW_ACT_FWD_VSI_MASK	GENMASK(17, 4)
#define FDICE_SW_ACT_FWD_Q_SHIFT	18
#define FDICE_SW_ACT_FWD_Q_MASK		GENMASK(25, 18)
#define FDICE_SW_ACT_DROP		BIT(0)
#define FDICE_SW_ACT_FWD		BIT(1)

/**
 * struct fdice_recipe_content - Recipe definition for AQ programming
 * @recipe_idx: recipe index (0-63)
 * @flags: recipe flags
 * @content: extraction field definitions (protocol + offset pairs)
 * @mask: extraction mask bytes
 * @result_idx: result index for recipe chaining
 * @rsvd: padding
 *
 * Element in the indirect buffer for add_recipe AQ command.
 */
struct fdice_recipe_content {
	u8	recipe_idx;
	u8	flags;
	u8	content[30];
	u8	mask[30];
	u8	result_idx;
	u8	rsvd;
} __packed;

/* -----------------------------------------------------------------------
 * Command Vtable
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_flow_cmds - Hardware command interface for flow steering
 * @create_flow_table: create a flow table (software-only for E810)
 * @destroy_flow_table: destroy a flow table
 * @create_flow_group: create flow group, program recipe if needed
 * @destroy_flow_group: destroy flow group, release recipe reference
 * @create_flow_entry: program switch rule via AQ
 * @update_flow_entry: update existing switch rule via AQ
 * @delete_flow_entry: remove switch rule via AQ
 * @create_fdir_entry: program FDIR filter
 * @delete_fdir_entry: remove FDIR filter
 *
 * Single implementation for E810 firmware-managed steering.
 * The vtable pattern enables mock implementations for testing.
 */
struct fdice_flow_cmds {
	int (*create_flow_table)(struct fdice_flow_table *ft,
				 struct fdice_flow_namespace *ns);
	int (*destroy_flow_table)(struct fdice_flow_table *ft);

	int (*create_flow_group)(struct fdice_flow_group *fg,
				 struct fdice_flow_table *ft);
	int (*destroy_flow_group)(struct fdice_flow_group *fg);

	int (*create_flow_entry)(struct fdice_flow_entry *fe,
				 struct fdice_flow_group *fg);
	int (*update_flow_entry)(struct fdice_flow_entry *fe);
	int (*delete_flow_entry)(struct fdice_flow_entry *fe);

	int (*create_fdir_entry)(struct fdice_flow_entry *fe,
				 struct fdice_fdir_params *params);
	int (*delete_fdir_entry)(struct fdice_flow_entry *fe);
};

/* E810 command vtable instance (defined in fdice_fs_cmd.c) */
extern const struct fdice_flow_cmds fdice_e810_flow_cmds;

/* -----------------------------------------------------------------------
 * E810 AQ Command Implementations
 *
 * Individual functions backing the fdice_e810_flow_cmds vtable.
 * Each maps a tree operation to E810 Admin Queue commands.
 * ----------------------------------------------------------------------- */

/**
 * fdice_e810_create_flow_table - Create flow table (software-only on E810)
 * @ft: flow table to initialize
 * @ns: parent namespace
 *
 * E810 has no direct hardware table concept; performs software
 * bookkeeping only.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_create_flow_table(struct fdice_flow_table *ft,
				 struct fdice_flow_namespace *ns);

/**
 * fdice_e810_destroy_flow_table - Destroy flow table
 * @ft: flow table to destroy
 *
 * Cascades removal of all child switch rules.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_destroy_flow_table(struct fdice_flow_table *ft);

/**
 * fdice_e810_create_flow_group - Create flow group via recipe management
 * @fg: flow group to create
 * @ft: parent flow table
 *
 * Acquires or shares an E810 recipe matching the group's extraction
 * pattern via add_recipe AQ command (opcode 0x0290).
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_create_flow_group(struct fdice_flow_group *fg,
				 struct fdice_flow_table *ft);

/**
 * fdice_e810_destroy_flow_group - Destroy flow group and release recipe
 * @fg: flow group to destroy
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_destroy_flow_group(struct fdice_flow_group *fg);

/**
 * fdice_e810_create_flow_entry - Create switch rule via AQ
 * @fe: flow entry to program
 * @fg: parent flow group providing recipe binding
 *
 * Programs a forwarding rule using AQ add_sw_rules (opcode 0x02A0).
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_create_flow_entry(struct fdice_flow_entry *fe,
				 struct fdice_flow_group *fg);

/**
 * fdice_e810_update_flow_entry - Update switch rule in-place via AQ
 * @fe: flow entry to update
 *
 * Modifies existing rule using AQ update_sw_rules (opcode 0x02A1).
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_update_flow_entry(struct fdice_flow_entry *fe);

/**
 * fdice_e810_delete_flow_entry - Delete switch rule via AQ
 * @fe: flow entry to remove
 *
 * Removes rule using AQ remove_sw_rules (opcode 0x02A2).
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_delete_flow_entry(struct fdice_flow_entry *fe);

/**
 * fdice_e810_create_fdir_entry - Create FDIR filter
 * @fe: flow entry with structured match criteria
 * @params: FDIR-specific parameters (queue, mark, counters)
 *
 * Translates structured match criteria into dummy-packet format and
 * programs the filter through the FDIR AQ path.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_create_fdir_entry(struct fdice_flow_entry *fe,
				 struct fdice_fdir_params *params);

/**
 * fdice_e810_delete_fdir_entry - Delete FDIR filter
 * @fe: flow entry to remove
 *
 * Removes FDIR filter identified by fe->hw.fdir.fltr_id.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_e810_delete_fdir_entry(struct fdice_flow_entry *fe);

#endif /* _FDICE_FS_CMD_H_ */
