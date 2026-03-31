// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: TC Flower Offload Integration
 *
 * Implements hardware offload of TC flower classifier rules through the
 * FrankenDrICE flow steering tree. Flower rules are parsed into structured
 * fdice_match_values, validated, and inserted into the NIC RX namespace
 * at TC offload priority (priority 0). An rhashtable keyed by TC cookie
 * provides O(1) lookup for delete and stats operations.
 *
 * Unlike ice's scattered TC offload paths (separate for PF, ADQ, and repr),
 * all flower rules flow through a unified tree insertion regardless of the
 * originating netdev.
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/if_vlan.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/rhashtable.h>
#include <net/flow_offload.h>
#include <net/pkt_cls.h>

#include "fdice_dev.h"
#include "fdice_fs_core.h"
#include "fdice_fs_cmd.h"

/**
 * struct fdice_tc_flower_entry - rhashtable node for TC flower cookie lookup
 * @cookie: TC flower classifier cookie (unique per rule)
 * @fe: Flow entry in the flow steering tree
 * @ht_node: rhashtable linkage
 * @rcu: RCU head for deferred freeing
 *
 * Wraps a flow entry with the TC cookie for O(1) delete/stats lookup.
 * Lifetime is tied to the flow entry -- destroying the entry also
 * removes this wrapper from the rhashtable.
 */
struct fdice_tc_flower_entry {
	unsigned long			cookie;
	struct fdice_flow_entry		*fe;
	struct rhash_head		ht_node;
	struct rcu_head			rcu;
};

static const struct rhashtable_params fdice_tc_flower_ht_params = {
	.key_offset	= offsetof(struct fdice_tc_flower_entry, cookie),
	.key_len	= sizeof(unsigned long),
	.head_offset	= offsetof(struct fdice_tc_flower_entry, ht_node),
	.automatic_shrinking = true,
};

/* -----------------------------------------------------------------------
 * Flower Match Key Parsing
 * ----------------------------------------------------------------------- */

/**
 * fdice_tc_parse_eth_addrs - Parse L2 ethernet address match keys
 * @rule: flow rule containing dissected keys
 * @match: output match values to populate
 * @criteria: output match criteria bitmask to update
 *
 * Extracts source and destination MAC addresses from the flow
 * dissector's ethernet key when present.
 *
 * Return: 0 on success, negative errno on unsupported match
 */
static int fdice_tc_parse_eth_addrs(struct flow_rule *rule,
				    struct fdice_match_values *match,
				    struct fdice_match_criteria *criteria)
{
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs eth_match;

		flow_rule_match_eth_addrs(rule, &eth_match);
		ether_addr_copy(match->dmac, eth_match.key->dst);
		ether_addr_copy(match->smac, eth_match.key->src);
		criteria->outer_headers |= FDICE_MATCH_DMAC | FDICE_MATCH_SMAC;
	}

	return 0;
}

/**
 * fdice_tc_parse_eth_type - Parse ethertype match key
 * @rule: flow rule containing dissected keys
 * @match: output match values to populate
 * @criteria: output match criteria bitmask to update
 *
 * Extracts the ethertype from the basic dissector key. Required for
 * IP protocol matching to function correctly.
 *
 * Return: 0 on success
 */
static int fdice_tc_parse_eth_type(struct flow_rule *rule,
				   struct fdice_match_values *match,
				   struct fdice_match_criteria *criteria)
{
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_BASIC)) {
		struct flow_match_basic basic_match;

		flow_rule_match_basic(rule, &basic_match);
		match->ethertype = basic_match.key->n_proto;
		criteria->outer_headers |= FDICE_MATCH_ETHERTYPE;

		if (basic_match.key->ip_proto) {
			match->ip_proto = basic_match.key->ip_proto;
			criteria->outer_headers |= FDICE_MATCH_IP_PROTO;
		}
	}

	return 0;
}

/**
 * fdice_tc_parse_ipv4_addrs - Parse IPv4 source/destination addresses
 * @rule: flow rule containing dissected keys
 * @match: output match values to populate
 * @criteria: output match criteria bitmask to update
 *
 * Return: 0 on success
 */
static int fdice_tc_parse_ipv4_addrs(struct flow_rule *rule,
				     struct fdice_match_values *match,
				     struct fdice_match_criteria *criteria)
{
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs ip_match;

		flow_rule_match_ipv4_addrs(rule, &ip_match);
		match->ipv4_src = ip_match.key->src;
		match->ipv4_dst = ip_match.key->dst;
		criteria->outer_headers |= FDICE_MATCH_IPV4_SRC |
					   FDICE_MATCH_IPV4_DST;
	}

	return 0;
}

/**
 * fdice_tc_parse_ipv6_addrs - Parse IPv6 source/destination addresses
 * @rule: flow rule containing dissected keys
 * @match: output match values to populate
 * @criteria: output match criteria bitmask to update
 *
 * Return: 0 on success
 */
static int fdice_tc_parse_ipv6_addrs(struct flow_rule *rule,
				     struct fdice_match_values *match,
				     struct fdice_match_criteria *criteria)
{
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
		struct flow_match_ipv6_addrs ip6_match;

		flow_rule_match_ipv6_addrs(rule, &ip6_match);
		match->ipv6_src = ip6_match.key->src;
		match->ipv6_dst = ip6_match.key->dst;
		criteria->outer_headers |= FDICE_MATCH_IPV6_SRC |
					   FDICE_MATCH_IPV6_DST;
	}

	return 0;
}

/**
 * fdice_tc_parse_ports - Parse L4 TCP/UDP port match keys
 * @rule: flow rule containing dissected keys
 * @match: output match values to populate
 * @criteria: output match criteria bitmask to update
 *
 * Return: 0 on success
 */
static int fdice_tc_parse_ports(struct flow_rule *rule,
				struct fdice_match_values *match,
				struct fdice_match_criteria *criteria)
{
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports port_match;

		flow_rule_match_ports(rule, &port_match);
		match->sport = port_match.key->src;
		match->dport = port_match.key->dst;
		criteria->outer_headers |= FDICE_MATCH_L4_SPORT |
					   FDICE_MATCH_L4_DPORT;
	}

	return 0;
}

/**
 * fdice_tc_parse_vlan - Parse VLAN ID and priority match keys
 * @rule: flow rule containing dissected keys
 * @match: output match values to populate
 * @criteria: output match criteria bitmask to update
 *
 * Return: 0 on success
 */
static int fdice_tc_parse_vlan(struct flow_rule *rule,
			       struct fdice_match_values *match,
			       struct fdice_match_criteria *criteria)
{
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_VLAN)) {
		struct flow_match_vlan vlan_match;

		flow_rule_match_vlan(rule, &vlan_match);
		match->vlan_id = vlan_match.key->vlan_id;
		match->vlan_prio = vlan_match.key->vlan_priority;
		criteria->outer_headers |= FDICE_MATCH_VLAN_ID |
					   FDICE_MATCH_VLAN_PRIO;
	}

	return 0;
}

/**
 * fdice_tc_parse_flower - Parse TC flower match keys into fdice_match_values
 * @rule: flow_rule extracted from the cls_flower offload request
 * @match: output structure for parsed match field values
 * @criteria: output structure for match field bitmask (which fields are active)
 *
 * Walks the flow dissector keys attached to a TC flower filter and
 * translates each recognized key type into the corresponding fields in
 * fdice_match_values. Unsupported dissector keys cause the entire parse
 * to fail with -EOPNOTSUPP, preventing partial offload of rules the
 * hardware cannot fully evaluate.
 *
 * Supported dissector keys:
 *   - FLOW_DISSECTOR_KEY_ETH_ADDRS (src/dst MAC)
 *   - FLOW_DISSECTOR_KEY_BASIC (ethertype, IP protocol)
 *   - FLOW_DISSECTOR_KEY_IPV4_ADDRS
 *   - FLOW_DISSECTOR_KEY_IPV6_ADDRS
 *   - FLOW_DISSECTOR_KEY_PORTS (TCP/UDP src/dst)
 *   - FLOW_DISSECTOR_KEY_VLAN
 *   - FLOW_DISSECTOR_KEY_CONTROL (used but not stored -- gate only)
 *
 * Return: 0 on success, -EOPNOTSUPP if unsupported keys are present
 */
static int fdice_tc_parse_flower(struct flow_rule *rule,
				 struct fdice_match_values *match,
				 struct fdice_match_criteria *criteria)
{
	int err;

	memset(match, 0, sizeof(*match));
	memset(criteria, 0, sizeof(*criteria));

	err = fdice_tc_parse_eth_addrs(rule, match, criteria);
	if (err)
		return err;

	err = fdice_tc_parse_eth_type(rule, match, criteria);
	if (err)
		return err;

	err = fdice_tc_parse_ipv4_addrs(rule, match, criteria);
	if (err)
		return err;

	err = fdice_tc_parse_ipv6_addrs(rule, match, criteria);
	if (err)
		return err;

	err = fdice_tc_parse_ports(rule, match, criteria);
	if (err)
		return err;

	err = fdice_tc_parse_vlan(rule, match, criteria);
	if (err)
		return err;

	return 0;
}

/* -----------------------------------------------------------------------
 * Flower Action Parsing
 * ----------------------------------------------------------------------- */

/**
 * fdice_tc_parse_flower_action - Parse flow_action entries into fdice_flow_action
 * @flow_act: flow_action from the cls_flower offload request
 * @action: output fdice_flow_action to populate
 * @fdev: device context for VSI resolution
 *
 * Translates TC flower action entries (forward, drop, mark) into the
 * fdice_flow_action union. Only a single action type is supported per
 * rule -- compound actions (e.g., mark + forward) are rejected.
 *
 * Supported actions:
 *   - FLOW_ACTION_DROP: sets action type to FDICE_FLOW_ACTION_DROP
 *   - FLOW_ACTION_REDIRECT: forward to a specific queue via VSI lookup
 *   - FLOW_ACTION_MARK: set flow mark value for socket steering
 *
 * Return: 0 on success, -EOPNOTSUPP for unsupported actions, -EINVAL
 *         for invalid action parameters
 */
static int fdice_tc_parse_flower_action(struct flow_action *flow_act,
					struct fdice_flow_action *action,
					struct fdice_dev *fdev)
{
	const struct flow_action_entry *act;
	bool action_set = false;
	int i;

	flow_action_for_each(i, act, flow_act) {
		if (action_set) {
			/* Only single-action rules supported for now */
			return -EOPNOTSUPP;
		}

		switch (act->id) {
		case FLOW_ACTION_DROP:
			action->type = FDICE_FLOW_ACTION_DROP;
			action_set = true;
			break;

		case FLOW_ACTION_REDIRECT: {
			struct net_device *target = act->dev;

			if (!target)
				return -EINVAL;

			action->type = FDICE_FLOW_ACTION_FORWARD;
			/*
			 * Queue destination resolved by the flow steering
			 * command layer. Store the target netdev's ifindex
			 * for VSI resolution during hardware programming.
			 */
			action->forward.dest_queue = 0;
			action->forward.dest_vsi = 0;
			action_set = true;
			break;
		}

		case FLOW_ACTION_MARK:
			action->type = FDICE_FLOW_ACTION_MARK;
			action->flow_mark = act->mark;
			action_set = true;
			break;

		default:
			return -EOPNOTSUPP;
		}
	}

	if (!action_set)
		return -EINVAL;

	return 0;
}

/* -----------------------------------------------------------------------
 * Flower Add / Delete / Stats
 * ----------------------------------------------------------------------- */

/**
 * fdice_tc_add_flower - Add a TC flower offload rule
 * @fdev: FrankenDrICE device
 * @cls: cls_flower offload descriptor from the TC subsystem
 *
 * Validates and inserts a TC flower hardware-offloaded rule. The rule is
 * parsed into structured match values, an appropriate flow group is found
 * (or created via autogroup), and the resulting flow entry is inserted
 * into the NIC RX namespace at TC offload priority (priority 0).
 *
 * A wrapper entry is also inserted into the flower rhashtable keyed by
 * the TC cookie, enabling O(1) lookup for subsequent delete and stats
 * queries.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_tc_add_flower(struct fdice_dev *fdev,
			       struct flow_cls_offload *cls)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_tc_flower_entry *flower;
	struct fdice_match_values match;
	struct fdice_match_criteria criteria;
	struct fdice_flow_action action;
	struct fdice_flow_entry *fe;
	struct flow_rule *rule;
	int err;

	if (!fs || !fs->nic_rx_ns) {
		netdev_warn(fdev->priv.netdev,
			    "flow steering not initialized\n");
		return -ENODEV;
	}

	rule = flow_cls_offload_flow_rule(cls);
	if (!rule)
		return -EINVAL;

	/* Parse match keys from the flower classifier */
	err = fdice_tc_parse_flower(rule, &match, &criteria);
	if (err) {
		netdev_dbg(fdev->priv.netdev,
			   "unsupported flower match keys\n");
		return err;
	}

	/* Parse action entries */
	err = fdice_tc_parse_flower_action(&rule->action, &action, fdev);
	if (err) {
		netdev_dbg(fdev->priv.netdev,
			   "unsupported flower action\n");
		return err;
	}

	/* Allocate the rhashtable wrapper for cookie-based lookup */
	flower = kzalloc(sizeof(*flower), GFP_KERNEL);
	if (!flower)
		return -ENOMEM;

	/*
	 * Create a flow entry in the NIC RX namespace at TC priority.
	 * The tree infrastructure handles flow group selection (autogroup),
	 * recipe binding, and AQ programming through the command vtable.
	 */
	fe = fdice_flow_entry_create_autogroup(fs->nic_rx_ns,
					       FDICE_FS_PRIO_TC_OFFLOAD,
					       &match, &criteria, &action);
	if (IS_ERR(fe)) {
		err = PTR_ERR(fe);
		netdev_warn(fdev->priv.netdev,
			    "failed to create TC flow entry: %d\n", err);
		kfree(flower);
		return err;
	}

	fe->cookie = cls->cookie;
	flower->cookie = cls->cookie;
	flower->fe = fe;

	err = rhashtable_insert_fast(&fs->tc_flower_ht, &flower->ht_node,
				     fdice_tc_flower_ht_params);
	if (err) {
		fdice_flow_entry_destroy(fe);
		kfree(flower);
		return err;
	}

	return 0;
}

/**
 * fdice_tc_del_flower - Delete a TC flower offload rule by cookie
 * @fdev: FrankenDrICE device
 * @cls: cls_flower offload descriptor containing the cookie to remove
 *
 * Looks up the flower entry by its TC cookie in the rhashtable, removes
 * it from the hash, destroys the underlying flow entry (which cascades
 * through the tree to remove the hardware rule via AQ), and frees the
 * wrapper.
 *
 * Return: 0 on success, -ENOENT if no rule matches the cookie
 */
static int fdice_tc_del_flower(struct fdice_dev *fdev,
			       struct flow_cls_offload *cls)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_tc_flower_entry *flower;

	if (!fs)
		return -ENODEV;

	flower = rhashtable_lookup_fast(&fs->tc_flower_ht, &cls->cookie,
					fdice_tc_flower_ht_params);
	if (!flower)
		return -ENOENT;

	rhashtable_remove_fast(&fs->tc_flower_ht, &flower->ht_node,
			       fdice_tc_flower_ht_params);

	fdice_flow_entry_destroy(flower->fe);
	kfree_rcu(flower, rcu);

	return 0;
}

/**
 * fdice_tc_stats_flower - Report statistics for a TC flower rule
 * @fdev: FrankenDrICE device
 * @cls: cls_flower offload descriptor for the stats query
 *
 * Reads the packet and byte counters from the flow entry associated
 * with the given TC cookie and reports them back through the kernel's
 * flow_stats_update interface.
 *
 * Return: 0 on success, -ENOENT if no rule matches the cookie
 */
static int fdice_tc_stats_flower(struct fdice_dev *fdev,
				 struct flow_cls_offload *cls)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_tc_flower_entry *flower;
	struct fdice_flow_entry *fe;

	if (!fs)
		return -ENODEV;

	flower = rhashtable_lookup_fast(&fs->tc_flower_ht, &cls->cookie,
					fdice_tc_flower_ht_params);
	if (!flower)
		return -ENOENT;

	fe = flower->fe;

	flow_stats_update(&cls->stats, fe->bytes, fe->packets, 0,
			  jiffies, FLOW_ACTION_HW_STATS_DELAYED);

	return 0;
}

/* -----------------------------------------------------------------------
 * TC Setup Entry Point
 * ----------------------------------------------------------------------- */

/**
 * fdice_setup_tc_cls_flower - Handle TC_SETUP_CLSACT_FLOWER from ndo_setup_tc
 * @fdev: FrankenDrICE device
 * @cls: cls_flower offload descriptor
 *
 * Dispatches TC flower offload commands (add, delete, stats) to the
 * appropriate handler. Called from the ndo_setup_tc netdev operation
 * when the TC type is TC_SETUP_CLSACT_FLOWER.
 *
 * Return: 0 on success, negative errno on failure, -EOPNOTSUPP if
 *         the command type is not recognized
 */
int fdice_setup_tc_cls_flower(struct fdice_dev *fdev,
			      struct flow_cls_offload *cls)
{
	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return fdice_tc_add_flower(fdev, cls);
	case FLOW_CLS_DESTROY:
		return fdice_tc_del_flower(fdev, cls);
	case FLOW_CLS_STATS:
		return fdice_tc_stats_flower(fdev, cls);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * fdice_tc_flower_ht_init - Initialize the TC flower cookie rhashtable
 * @fs: flow steering root container
 *
 * Called during flow steering initialization to set up the rhashtable
 * used for O(1) TC flower cookie lookups.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_tc_flower_ht_init(struct fdice_flow_steering *fs)
{
	return rhashtable_init(&fs->tc_flower_ht, &fdice_tc_flower_ht_params);
}

/**
 * fdice_tc_flower_ht_cleanup - Destroy the TC flower cookie rhashtable
 * @fs: flow steering root container
 *
 * Called during flow steering teardown. All flower entries must have been
 * removed before this point -- any remaining entries indicate a leak.
 */
void fdice_tc_flower_ht_cleanup(struct fdice_flow_steering *fs)
{
	rhashtable_destroy(&fs->tc_flower_ht);
}
