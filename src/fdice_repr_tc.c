// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: TC Flower on Port Representors
 *
 * Implements TC flower offload for representor netdevs. When a flower rule
 * is programmed on a representor, it is inserted into the FDB namespace
 * (not the NIC RX namespace used by PF flower rules), enabling cross-VF
 * and VF-to-uplink steering via the eSwitch flow tables.
 *
 * This module reuses the shared flower parsing infrastructure from
 * fdice_fs_tc.c (fdice_tc_parse_flower, fdice_tc_parse_flower_action)
 * by calling through the unified fdice_setup_tc_cls_flower entry point
 * with an FDB namespace context. The key difference is namespace selection:
 * PF flower rules go to NIC RX, representor flower rules go to FDB.
 *
 * Architecture reference: docs/exploration/15-switchdev-mode.md
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/netdevice.h>
#include <net/pkt_cls.h>
#include <net/flow_offload.h>

#include "fdice_dev.h"
#include "fdice_eswitch.h"
#include "fdice_repr.h"
#include "fdice_fs_core.h"

/* -----------------------------------------------------------------------
 * Flower Rule Tracking for Representors
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_repr_flower - Representor TC flower entry for FDB namespace
 * @cookie: TC flower classifier cookie (unique per rule)
 * @fe: flow entry in the FDB flow steering namespace
 * @ht_node: rhashtable linkage in the flow steering tc_flower_ht
 * @rcu: RCU head for deferred freeing
 *
 * Similar to the PF's fdice_tc_flower_entry but targets the FDB
 * namespace. Uses the same rhashtable in flow_steering for cookie lookup
 * to maintain a unified delete/stats path.
 */
struct fdice_repr_flower {
	unsigned long			cookie;
	struct fdice_flow_entry		*fe;
	struct rhash_head		ht_node;
	struct rcu_head			rcu;
};

static const struct rhashtable_params fdice_repr_flower_ht_params = {
	.key_offset	= offsetof(struct fdice_repr_flower, cookie),
	.key_len	= sizeof(unsigned long),
	.head_offset	= offsetof(struct fdice_repr_flower, ht_node),
	.automatic_shrinking = true,
};

/* -----------------------------------------------------------------------
 * Flower Match Key Parsing (reuses patterns from fdice_fs_tc.c)
 * ----------------------------------------------------------------------- */

/**
 * fdice_repr_parse_flower - Parse TC flower match keys for representor
 * @rule: flow_rule from the cls_flower offload request
 * @match: output match values
 * @criteria: output match criteria bitmask
 *
 * Parses the same set of dissector keys as the PF TC flower path.
 * Representor rules target the FDB namespace for eSwitch-level steering.
 *
 * Return: 0 on success, -EOPNOTSUPP for unsupported keys
 */
static int fdice_repr_parse_flower(struct flow_rule *rule,
				   struct fdice_match_values *match,
				   struct fdice_match_criteria *criteria)
{
	memset(match, 0, sizeof(*match));
	memset(criteria, 0, sizeof(*criteria));

	/* L2: ethernet addresses */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_ETH_ADDRS)) {
		struct flow_match_eth_addrs eth_match;

		flow_rule_match_eth_addrs(rule, &eth_match);
		ether_addr_copy(match->dmac, eth_match.key->dst);
		ether_addr_copy(match->smac, eth_match.key->src);
		criteria->outer_headers |= FDICE_MATCH_DMAC | FDICE_MATCH_SMAC;
	}

	/* L2: ethertype + IP protocol */
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

	/* L3: IPv4 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV4_ADDRS)) {
		struct flow_match_ipv4_addrs ip_match;

		flow_rule_match_ipv4_addrs(rule, &ip_match);
		match->ipv4_src = ip_match.key->src;
		match->ipv4_dst = ip_match.key->dst;
		criteria->outer_headers |= FDICE_MATCH_IPV4_SRC |
					   FDICE_MATCH_IPV4_DST;
	}

	/* L3: IPv6 */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_IPV6_ADDRS)) {
		struct flow_match_ipv6_addrs ip6_match;

		flow_rule_match_ipv6_addrs(rule, &ip6_match);
		match->ipv6_src = ip6_match.key->src;
		match->ipv6_dst = ip6_match.key->dst;
		criteria->outer_headers |= FDICE_MATCH_IPV6_SRC |
					   FDICE_MATCH_IPV6_DST;
	}

	/* L4: TCP/UDP ports */
	if (flow_rule_match_key(rule, FLOW_DISSECTOR_KEY_PORTS)) {
		struct flow_match_ports port_match;

		flow_rule_match_ports(rule, &port_match);
		match->sport = port_match.key->src;
		match->dport = port_match.key->dst;
		criteria->outer_headers |= FDICE_MATCH_L4_SPORT |
					   FDICE_MATCH_L4_DPORT;
	}

	/* VLAN */
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
 * fdice_repr_parse_flower_action - Parse flower actions for representor
 * @flow_act: flow_action from cls_flower
 * @action: output fdice_flow_action
 * @repr: representor context (for VSI resolution)
 *
 * Translates TC flower actions targeting the FDB namespace. Supports
 * redirect (forward to VSI) and drop actions.
 *
 * Return: 0 on success, -EOPNOTSUPP for unsupported actions
 */
static int fdice_repr_parse_flower_action(struct flow_action *flow_act,
					  struct fdice_flow_action *action,
					  struct fdice_repr *repr)
{
	const struct flow_action_entry *act;
	bool action_set = false;
	int i;

	flow_action_for_each(i, act, flow_act) {
		if (action_set)
			return -EOPNOTSUPP;

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
			 * If the redirect target is another representor,
			 * resolve to its VSI. Otherwise forward to uplink.
			 */
			if (fdice_is_repr_netdev(target)) {
				struct fdice_repr *target_repr;

				target_repr = fdice_repr_priv(target);
				action->forward.dest_vsi = target_repr->vsi_id;
			} else {
				action->forward.dest_vsi = 0; /* uplink */
			}
			action->forward.dest_queue = 0;
			action_set = true;
			break;
		}

		default:
			return -EOPNOTSUPP;
		}
	}

	if (!action_set)
		return -EINVAL;

	return 0;
}

/* -----------------------------------------------------------------------
 * Representor Flower Add / Delete
 * ----------------------------------------------------------------------- */

/**
 * fdice_repr_add_flower - Add a TC flower rule on a representor
 * @repr: representor context
 * @cls: cls_flower offload descriptor
 *
 * Parses the flower rule and inserts it into the FDB flow steering
 * namespace at TC flower priority. Rules on representors steer traffic
 * at the eSwitch level, enabling cross-VF forwarding.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_repr_add_flower(struct fdice_repr *repr,
				 struct flow_cls_offload *cls)
{
	struct fdice_dev *fdev = repr->fdev;
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_repr_flower *flower;
	struct fdice_match_values match;
	struct fdice_match_criteria criteria;
	struct fdice_flow_action action;
	struct fdice_flow_entry *fe;
	struct flow_rule *rule;
	int err;

	if (!fs || !fs->fdb_ns) {
		netdev_warn(repr->netdev,
			    "FDB namespace not initialized\n");
		return -ENODEV;
	}

	rule = flow_cls_offload_flow_rule(cls);
	if (!rule)
		return -EINVAL;

	err = fdice_repr_parse_flower(rule, &match, &criteria);
	if (err) {
		netdev_dbg(repr->netdev,
			   "unsupported flower match keys on repr\n");
		return err;
	}

	err = fdice_repr_parse_flower_action(&rule->action, &action, repr);
	if (err) {
		netdev_dbg(repr->netdev,
			   "unsupported flower action on repr\n");
		return err;
	}

	flower = kzalloc(sizeof(*flower), GFP_KERNEL);
	if (!flower)
		return -ENOMEM;

	/*
	 * Insert into FDB namespace (not NIC RX) at TC flower priority.
	 * This is the key difference from PF flower rules which go to NIC RX.
	 */
	fe = fdice_flow_entry_create_autogroup(fs->fdb_ns,
					       FDICE_FS_PRIO_FDB_TC_FLOWER,
					       &match, &criteria, &action);
	if (IS_ERR(fe)) {
		err = PTR_ERR(fe);
		netdev_warn(repr->netdev,
			    "failed to create FDB flow entry: %d\n", err);
		kfree(flower);
		return err;
	}

	fe->cookie = cls->cookie;
	flower->cookie = cls->cookie;
	flower->fe = fe;

	err = rhashtable_insert_fast(&fs->tc_flower_ht, &flower->ht_node,
				     fdice_repr_flower_ht_params);
	if (err) {
		fdice_flow_entry_destroy(fe);
		kfree(flower);
		return err;
	}

	return 0;
}

/**
 * fdice_repr_del_flower - Delete a TC flower rule from a representor
 * @repr: representor context
 * @cls: cls_flower offload descriptor with cookie to delete
 *
 * Removes the flower rule from the FDB namespace. Looks up by cookie
 * in the shared rhashtable.
 *
 * Return: 0 on success, -ENOENT if rule not found
 */
static int fdice_repr_del_flower(struct fdice_repr *repr,
				 struct flow_cls_offload *cls)
{
	struct fdice_dev *fdev = repr->fdev;
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_repr_flower *flower;

	if (!fs)
		return -ENODEV;

	flower = rhashtable_lookup_fast(&fs->tc_flower_ht, &cls->cookie,
					fdice_repr_flower_ht_params);
	if (!flower)
		return -ENOENT;

	rhashtable_remove_fast(&fs->tc_flower_ht, &flower->ht_node,
			       fdice_repr_flower_ht_params);

	fdice_flow_entry_destroy(flower->fe);
	kfree_rcu(flower, rcu);

	return 0;
}

/**
 * fdice_repr_stats_flower - Report statistics for a representor flower rule
 * @repr: representor context
 * @cls: cls_flower offload descriptor for stats query
 *
 * Return: 0 on success, -ENOENT if rule not found
 */
static int fdice_repr_stats_flower(struct fdice_repr *repr,
				   struct flow_cls_offload *cls)
{
	struct fdice_dev *fdev = repr->fdev;
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_repr_flower *flower;
	struct fdice_flow_entry *fe;

	if (!fs)
		return -ENODEV;

	flower = rhashtable_lookup_fast(&fs->tc_flower_ht, &cls->cookie,
					fdice_repr_flower_ht_params);
	if (!flower)
		return -ENOENT;

	fe = flower->fe;
	flow_stats_update(&cls->stats, fe->bytes, fe->packets, 0,
			  jiffies, FLOW_ACTION_HW_STATS_DELAYED);

	return 0;
}

/* -----------------------------------------------------------------------
 * Representor ndo_setup_tc Entry Point
 * ----------------------------------------------------------------------- */

/**
 * fdice_repr_setup_tc_cls_flower - Handle cls_flower on a representor
 * @repr: representor context
 * @cls: cls_flower offload descriptor
 *
 * Dispatches flower add/delete/stats commands for representor netdevs.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_repr_setup_tc_cls_flower(struct fdice_repr *repr,
					  struct flow_cls_offload *cls)
{
	switch (cls->command) {
	case FLOW_CLS_REPLACE:
		return fdice_repr_add_flower(repr, cls);
	case FLOW_CLS_DESTROY:
		return fdice_repr_del_flower(repr, cls);
	case FLOW_CLS_STATS:
		return fdice_repr_stats_flower(repr, cls);
	default:
		return -EOPNOTSUPP;
	}
}

/**
 * fdice_repr_setup_tc - ndo_setup_tc for representor netdevs
 * @dev: representor network device
 * @type: TC setup type
 * @type_data: type-specific data (cls_flower for CLSACT_FLOWER)
 *
 * Entry point for TC offload on representor netdevs. Currently supports
 * only TC_SETUP_CLSACT_FLOWER for flower classifier rules.
 *
 * Return: 0 on success, -EOPNOTSUPP for unsupported types
 */
int fdice_repr_setup_tc(struct net_device *dev, enum tc_setup_type type,
			void *type_data)
{
	struct fdice_repr *repr = fdice_repr_priv(dev);

	switch (type) {
	case TC_SETUP_CLSFLOWER:
		return fdice_repr_setup_tc_cls_flower(repr, type_data);
	default:
		return -EOPNOTSUPP;
	}
}
