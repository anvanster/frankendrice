// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_FS_CORE_H_
#define _FDICE_FS_CORE_H_

/**
 * DOC: FrankenDrICE Flow Steering Core Header
 *
 * Tree-based flow steering infrastructure providing hierarchical organization,
 * refcounted lifetime management, and namespace isolation for E810 flow rules.
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include <linux/types.h>
#include <linux/list.h>
#include <linux/rwsem.h>
#include <linux/refcount.h>
#include <linux/idr.h>
#include <linux/if_ether.h>
#include <linux/in6.h>
#include <linux/rhashtable.h>

/* Forward declarations */
struct fdice_dev;
struct fdice_flow_cmds;
struct fdice_recipe;
struct fdice_recipe_mgr;
struct fdice_ttc_table;
struct ethtool_rxnfc;

/* -----------------------------------------------------------------------
 * Constants
 * ----------------------------------------------------------------------- */

#define FDICE_MAX_RECIPES		64

/* Match criteria enable flags (category-level) */
#define FDICE_MATCH_OUTER_HEADERS	BIT(0)
#define FDICE_MATCH_INNER_HEADERS	BIT(1)
#define FDICE_MATCH_MISC		BIT(2)

/* FDICE_MATCH_* -- per-field flags for outer_headers / inner_headers */
#define FDICE_MATCH_DMAC		BIT(0)
#define FDICE_MATCH_SMAC		BIT(1)
#define FDICE_MATCH_ETHERTYPE		BIT(2)
#define FDICE_MATCH_VLAN_ID		BIT(3)
#define FDICE_MATCH_VLAN_PRIO		BIT(4)
#define FDICE_MATCH_IPV4_SRC		BIT(5)
#define FDICE_MATCH_IPV4_DST		BIT(6)
#define FDICE_MATCH_IPV6_SRC		BIT(7)
#define FDICE_MATCH_IPV6_DST		BIT(8)
#define FDICE_MATCH_IP_PROTO		BIT(9)
#define FDICE_MATCH_IP_TOS		BIT(10)
#define FDICE_MATCH_IP_TTL		BIT(11)
#define FDICE_MATCH_L4_SPORT		BIT(12)
#define FDICE_MATCH_L4_DPORT		BIT(13)
#define FDICE_MATCH_TUNNEL_ID		BIT(14)
#define FDICE_MATCH_TUNNEL_TYPE		BIT(15)

/* NIC RX namespace priority levels */
#define FDICE_FS_PRIO_TC_OFFLOAD	0
#define FDICE_FS_PRIO_FDIR		1
#define FDICE_FS_PRIO_ADQ		2
#define FDICE_FS_PRIO_VLAN		3
#define FDICE_FS_PRIO_L2_MAC		4
#define FDICE_FS_PRIO_RSS_DEFAULT	5
#define FDICE_FS_NIC_RX_NUM_PRIOS	6

/* FDB namespace priority levels */
#define FDICE_FS_PRIO_FDB_TC_FLOWER	0
#define FDICE_FS_PRIO_FDB_BRIDGE	1
#define FDICE_FS_PRIO_FDB_DEFAULT	2
#define FDICE_FS_FDB_NUM_PRIOS		3

/* NIC TX namespace priority levels */
#define FDICE_FS_NIC_TX_NUM_PRIOS	1

/* -----------------------------------------------------------------------
 * Node Types and Base Type
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_fs_node_type - Discriminator for flow steering tree nodes
 * @FDICE_FS_TYPE_NAMESPACE: Traffic domain grouping
 * @FDICE_FS_TYPE_PRIORITY: Ordered priority level
 * @FDICE_FS_TYPE_FLOW_TABLE: Collection of flow groups
 * @FDICE_FS_TYPE_FLOW_GROUP: Shared match criteria / recipe binding
 * @FDICE_FS_TYPE_FLOW_ENTRY: Individual match+action rule
 */
enum fdice_fs_node_type {
	FDICE_FS_TYPE_NAMESPACE,
	FDICE_FS_TYPE_PRIORITY,
	FDICE_FS_TYPE_FLOW_TABLE,
	FDICE_FS_TYPE_FLOW_GROUP,
	FDICE_FS_TYPE_FLOW_ENTRY,
};

/**
 * struct fdice_fs_node - Base type for all flow steering tree nodes
 * @children: child node list head
 * @sibling: linkage in parent's children list
 * @parent: parent node (NULL for root)
 * @root: root namespace node
 * @type: node type discriminator
 * @lock: per-node read/write semaphore
 * @refcount: reference count for lifetime management
 * @del_hw_func: virtual destructor to remove from hardware
 * @del_sw_func: virtual destructor to free software state
 *
 * Provides unified lifetime management, locking, and tree navigation
 * for all flow steering objects. Every flow steering structure embeds
 * this as its first member for container_of access.
 */
struct fdice_fs_node {
	struct list_head	children;
	struct list_head	sibling;
	struct fdice_fs_node	*parent;
	struct fdice_fs_node	*root;
	enum fdice_fs_node_type	type;

	struct rw_semaphore	lock;
	refcount_t		refcount;

	void (*del_hw_func)(struct fdice_fs_node *);
	void (*del_sw_func)(struct fdice_fs_node *);
};

/* -----------------------------------------------------------------------
 * Tunnel and Action Enums
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_tunnel_type - Supported tunnel encapsulation types
 */
enum fdice_tunnel_type {
	FDICE_TUN_NONE = 0,
	FDICE_TUN_VXLAN,
	FDICE_TUN_GRE,
	FDICE_TUN_GENEVE,
	FDICE_TUN_GTP,
};

/**
 * enum fdice_flow_action_type - Flow rule action types
 */
enum fdice_flow_action_type {
	FDICE_FLOW_ACTION_FORWARD,
	FDICE_FLOW_ACTION_DROP,
	FDICE_FLOW_ACTION_MARK,
	FDICE_FLOW_ACTION_COUNT,
};

/* -----------------------------------------------------------------------
 * Match Criteria, Values, and Actions
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_match_criteria - Bitmask defining which fields to match
 * @outer_headers: FDICE_MATCH_* flags for L2-L4 outer fields
 * @inner_headers: flags for tunnel inner headers
 * @misc: miscellaneous match fields
 *
 * Used in flow groups to define the match pattern (mask).
 * Maps to E810 recipe extraction words.
 */
struct fdice_match_criteria {
	u32	outer_headers;
	u32	inner_headers;
	u32	misc;
};

/**
 * struct fdice_match_values - Concrete field values for a flow entry
 * @dmac: destination MAC address
 * @smac: source MAC address
 * @ethertype: Ethernet type
 * @vlan_id: VLAN identifier
 * @vlan_prio: VLAN priority
 * @ipv4_src: IPv4 source address
 * @ipv6_src: IPv6 source address
 * @ipv4_dst: IPv4 destination address
 * @ipv6_dst: IPv6 destination address
 * @ip_proto: IP protocol number
 * @tos: type of service / DSCP
 * @ttl: time to live
 * @sport: L4 source port
 * @dport: L4 destination port
 * @tunnel_id: tunnel identifier (VNI, GRE key, TEID)
 * @tun_type: tunnel encapsulation type
 */
struct fdice_match_values {
	u8		dmac[ETH_ALEN];
	u8		smac[ETH_ALEN];
	__be16		ethertype;
	u16		vlan_id;
	u8		vlan_prio;

	union {
		__be32		ipv4_src;
		struct in6_addr	ipv6_src;
	};
	union {
		__be32		ipv4_dst;
		struct in6_addr	ipv6_dst;
	};
	u8		ip_proto;
	u8		tos;
	u8		ttl;

	__be16		sport;
	__be16		dport;

	__be32			tunnel_id;
	enum fdice_tunnel_type	tun_type;
};

/**
 * struct fdice_flow_action - Action specification for a flow entry
 * @type: action type (forward, drop, mark, count)
 * @forward: forward action parameters
 * @forward.dest_vsi: destination VSI index
 * @forward.dest_queue: specific queue within VSI
 * @flow_mark: mark value for FDIR
 */
struct fdice_flow_action {
	enum fdice_flow_action_type type;
	union {
		struct {
			u16	dest_vsi;
			u16	dest_queue;
		} forward;
		u32	flow_mark;
	};
};

/* Maximum dummy packet size: ETH + IPv6 + TCP with no payload */
#define FDICE_FDIR_MAX_PKT_LEN		128

/**
 * struct fdice_fdir_params - FDIR programming parameters
 * @pkt_buf: dummy packet buffer for E810 FDIR programming
 * @pkt_len: length of constructed dummy packet in bytes
 * @q_index: target RX queue index for matched packets
 * @flow_id: software flow identifier for tracking
 * @is_ipv6: true if this is an IPv6 rule
 * @ip_proto: IP protocol number (IPPROTO_TCP, IPPROTO_UDP, etc.)
 *
 * Intermediate representation between structured match values and
 * the E810 FDIR AQ command format. Built by fdice_fdir_params_from_match()
 * and consumed by the flow command vtable's create_fdir_entry().
 */
struct fdice_fdir_params {
	u8		pkt_buf[FDICE_FDIR_MAX_PKT_LEN];
	u16		pkt_len;
	u16		q_index;
	u32		flow_id;
	bool		is_ipv6;
	u8		ip_proto;
};

/* -----------------------------------------------------------------------
 * Tree Node Structures
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_flow_namespace - Traffic domain grouping
 * @node: base tree node
 * @dev: backpointer to core device (for AQ command submission)
 * @cmds: command vtable for this namespace
 * @ns_type: namespace type identifier
 * @num_prios: number of priority levels
 */
struct fdice_flow_namespace {
	struct fdice_fs_node		node;
	struct fdice_dev		*dev;
	const struct fdice_flow_cmds	*cmds;
	int				ns_type;
	int				num_prios;
};

/**
 * enum fdice_flow_ns_type - Namespace type identifiers
 */
enum fdice_flow_ns_type {
	FDICE_FLOW_NS_NIC_RX,
	FDICE_FLOW_NS_NIC_TX,
	FDICE_FLOW_NS_FDB,
	FDICE_FLOW_NS_RDMA_RX,
};

/**
 * struct fdice_flow_priority - Ordered priority level
 * @node: base tree node
 * @level: priority level number (lower = higher priority)
 * @max_tables: maximum number of tables at this priority
 * @num_tables: current number of tables
 */
struct fdice_flow_priority {
	struct fdice_fs_node	node;
	int			level;
	int			max_tables;
	int			num_tables;
};

/**
 * struct fdice_flow_table - Collection of flow groups at a priority
 * @node: base tree node
 * @table_id: unique table identifier
 * @priority: priority level of this table
 * @max_groups: maximum flow groups
 * @num_groups: current flow group count
 * @group_ida: group index allocator
 */
struct fdice_flow_table {
	struct fdice_fs_node	node;
	u32			table_id;
	int			priority;
	u32			max_groups;
	u32			num_groups;
	struct ida		group_ida;
};

/**
 * struct fdice_flow_group - Collection of entries sharing match criteria
 * @node: base tree node
 * @mask: match criteria defining extraction pattern
 * @match_criteria_enable: bitmask of active criteria categories
 * @recipe_id: bound E810 recipe ID, or 0 if pending
 * @recipe: recipe reference (shared across groups)
 * @max_entries: maximum entries in this group
 * @num_entries: current entry count
 * @entry_ida: entry index allocator
 * @is_autogroup: true if created by autogroup mechanism
 */
struct fdice_flow_group {
	struct fdice_fs_node		node;

	struct fdice_match_criteria	mask;
	u32				match_criteria_enable;

	u16				recipe_id;
	struct fdice_recipe		*recipe;

	u32				max_entries;
	u32				num_entries;
	struct ida			entry_ida;

	bool				is_autogroup;
};

/**
 * struct fdice_flow_entry - Individual match+action flow rule
 * @node: base tree node
 * @match: match values for this entry
 * @action: action specification
 * @hw: hardware binding (switch rule or FDIR)
 * @hw.sw_rule: switch rule binding
 * @hw.sw_rule.rule_id: switch rule ID from AQ
 * @hw.sw_rule.recipe_id: recipe used
 * @hw.fdir: FDIR filter binding
 * @hw.fdir.fltr_id: FDIR filter ID
 * @hw.fdir.q_index: target queue index
 * @cookie: TC flower cookie / user handle
 * @packets: matched packet counter
 * @bytes: matched byte counter
 */
struct fdice_flow_entry {
	struct fdice_fs_node		node;

	struct fdice_match_values	match;
	struct fdice_flow_action	action;

	union {
		struct {
			u16	rule_id;
			u16	recipe_id;
		} sw_rule;
		struct {
			u32	fltr_id;
			u16	q_index;
		} fdir;
	} hw;

	u64	cookie;
	u64	packets;
	u64	bytes;
};

/* -----------------------------------------------------------------------
 * Root Flow Steering Container
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_flow_steering - Root container for all flow steering state
 * @dev: backpointer to core device
 * @nic_rx_ns: NIC receive steering namespace
 * @nic_tx_ns: NIC transmit steering namespace
 * @fdb_ns: eSwitch FDB namespace
 * @rdma_rx_ns: RDMA receive namespace (future)
 * @cmds: command vtable (single implementation for E810)
 * @recipe_mgr: recipe lifecycle manager
 * @tc_flower_ht: rhashtable for O(1) TC flower cookie lookups
 * @ttc: type-to-queue classification table (RSS default path)
 */
struct fdice_flow_steering {
	struct fdice_dev		*dev;

	struct fdice_flow_namespace	*nic_rx_ns;
	struct fdice_flow_namespace	*nic_tx_ns;
	struct fdice_flow_namespace	*fdb_ns;
	struct fdice_flow_namespace	*rdma_rx_ns;

	const struct fdice_flow_cmds	*cmds;
	struct fdice_recipe_mgr		*recipe_mgr;

	struct rhashtable		tc_flower_ht;
	struct fdice_ttc_table		*ttc;
};

/* -----------------------------------------------------------------------
 * Node Lifecycle API
 * ----------------------------------------------------------------------- */

void fdice_fs_node_init(struct fdice_fs_node *node,
			enum fdice_fs_node_type type,
			struct fdice_fs_node *root);
struct fdice_fs_node *fdice_fs_node_get(struct fdice_fs_node *node);
void fdice_fs_node_put(struct fdice_fs_node *node);
void fdice_fs_node_add_child(struct fdice_fs_node *parent,
			     struct fdice_fs_node *child);
void fdice_fs_node_remove(struct fdice_fs_node *node);
void fdice_fs_node_remove_children(struct fdice_fs_node *node);

/* -----------------------------------------------------------------------
 * Tree Construction API
 * ----------------------------------------------------------------------- */

struct fdice_flow_namespace *
fdice_flow_namespace_create(struct fdice_dev *fdev,
			    enum fdice_flow_ns_type ns_type,
			    const struct fdice_flow_cmds *cmds);

struct fdice_flow_priority *
fdice_flow_priority_create(struct fdice_flow_namespace *ns, int level);

struct fdice_flow_table *
fdice_flow_table_create(struct fdice_flow_priority *prio, u32 max_groups);

void fdice_flow_table_destroy(struct fdice_flow_table *ft);

struct fdice_flow_group *
fdice_flow_group_create(struct fdice_flow_table *ft,
			const struct fdice_match_criteria *mask,
			u32 match_criteria_enable,
			u32 max_entries);

void fdice_flow_group_destroy(struct fdice_flow_group *fg);

struct fdice_flow_entry *
fdice_flow_entry_create(struct fdice_flow_group *fg,
			const struct fdice_match_values *match,
			struct fdice_flow_action *action);

int fdice_flow_entry_update(struct fdice_flow_entry *fe,
			    struct fdice_flow_action *action);

void fdice_flow_entry_destroy(struct fdice_flow_entry *fe);

/* -----------------------------------------------------------------------
 * Priority Lookup and Autogroup API
 * ----------------------------------------------------------------------- */

struct fdice_flow_priority *
fdice_flow_namespace_find_prio(struct fdice_flow_namespace *ns, int level);

struct fdice_flow_entry *
fdice_flow_entry_create_autogroup(struct fdice_flow_namespace *ns,
				  int prio_level,
				  const struct fdice_match_values *match,
				  const struct fdice_match_criteria *criteria,
				  struct fdice_flow_action *action);

struct fdice_flow_table *
fdice_flow_table_find_or_create(struct fdice_flow_priority *prio,
				u32 max_groups);

/* -----------------------------------------------------------------------
 * TC Flower Offload API
 * ----------------------------------------------------------------------- */

struct flow_cls_offload;

int fdice_setup_tc_cls_flower(struct fdice_dev *fdev,
			      struct flow_cls_offload *cls);
int fdice_tc_flower_ht_init(struct fdice_flow_steering *fs);
void fdice_tc_flower_ht_cleanup(struct fdice_flow_steering *fs);

/* -----------------------------------------------------------------------
 * TTC (Type-to-Queue Classification) API
 * ----------------------------------------------------------------------- */

int fdice_ttc_init(struct fdice_dev *fdev);
void fdice_ttc_cleanup(struct fdice_dev *fdev);
int fdice_ttc_update_rules(struct fdice_dev *fdev);

/* -----------------------------------------------------------------------
 * FDIR API
 * ----------------------------------------------------------------------- */

struct fdice_flow_entry *
fdice_fdir_add_rule(struct fdice_dev *fdev,
		    const struct fdice_match_values *match,
		    const struct fdice_match_criteria *criteria,
		    u16 q_index);
int fdice_fdir_del_rule(struct fdice_dev *fdev,
			struct fdice_flow_entry *fe);
int fdice_fdir_set_ntuple(struct fdice_dev *fdev,
			  struct ethtool_rxnfc *cmd);
int fdice_fdir_get_ntuple(struct fdice_dev *fdev,
			  struct ethtool_rxnfc *cmd);

/* -----------------------------------------------------------------------
 * Top-Level Init / Cleanup
 * ----------------------------------------------------------------------- */

int fdice_flow_steering_init(struct fdice_dev *fdev);
void fdice_flow_steering_cleanup(struct fdice_dev *fdev);

#endif /* _FDICE_FS_CORE_H_ */
