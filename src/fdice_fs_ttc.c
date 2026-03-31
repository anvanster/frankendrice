// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: Type-to-Queue Classification (TTC) -- RSS Default Path
 *
 * The TTC module creates the lowest-priority flow table in the NIC RX
 * namespace (priority 5). It provides the default packet steering path
 * that routes common protocol types to RSS queues when no higher-priority
 * rule (TC offload, FDIR, ADQ, VLAN, MAC) matches.
 *
 * TTC rules are static classification entries: one per recognized packet
 * type (IPv4/TCP, IPv4/UDP, IPv6/TCP, IPv6/UDP, ARP, etc.), each
 * steering to the RSS-configured queue set. When RSS configuration changes
 * (indirection table update, hash key change), the TTC rules are updated
 * to reflect the new queue mapping.
 *
 * This is analogous to mlx5e's TTC table, adapted for E810's flat recipe
 * model. Where mlx5 uses hardware flow table chaining with miss actions,
 * FrankenDrICE uses recipe priority values to achieve the same logical
 * ordering.
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/if_ether.h>
#include <linux/if_arp.h>

#include "fdice_dev.h"
#include "fdice_fs_core.h"
#include "fdice_fs_cmd.h"

/**
 * enum fdice_ttc_rule_type - Recognized packet types for TTC classification
 * @FDICE_TTC_IPV4_TCP: IPv4 + TCP
 * @FDICE_TTC_IPV4_UDP: IPv4 + UDP
 * @FDICE_TTC_IPV6_TCP: IPv6 + TCP
 * @FDICE_TTC_IPV6_UDP: IPv6 + UDP
 * @FDICE_TTC_IPV4_OTHER: IPv4 non-TCP/UDP (ICMP, SCTP, etc.)
 * @FDICE_TTC_IPV6_OTHER: IPv6 non-TCP/UDP (ICMPv6, etc.)
 * @FDICE_TTC_ARP: ARP packets
 * @FDICE_TTC_NUM_RULES: Total number of TTC rule types
 *
 * Each type maps to a distinct flow entry in the TTC table with its
 * own match criteria and queue destination.
 */
enum fdice_ttc_rule_type {
	FDICE_TTC_IPV4_TCP = 0,
	FDICE_TTC_IPV4_UDP,
	FDICE_TTC_IPV6_TCP,
	FDICE_TTC_IPV6_UDP,
	FDICE_TTC_IPV4_OTHER,
	FDICE_TTC_IPV6_OTHER,
	FDICE_TTC_ARP,
	FDICE_TTC_NUM_RULES,
};

/**
 * struct fdice_ttc_table - TTC flow table state
 * @ft: flow table in the NIC RX namespace at RSS/default priority
 * @rules: array of flow entries, one per TTC rule type
 * @fdev: backpointer to device for queue resolution
 *
 * Lifetime: created in fdice_ttc_init(), destroyed in fdice_ttc_cleanup().
 * Individual rules are updated in-place when RSS configuration changes.
 */
struct fdice_ttc_table {
	struct fdice_flow_table		*ft;
	struct fdice_flow_entry		*rules[FDICE_TTC_NUM_RULES];
	struct fdice_dev		*fdev;
};

/**
 * struct fdice_ttc_rule_def - Static definition for a TTC rule type
 * @ethertype: Ethernet protocol to match (network byte order)
 * @ip_proto: IP protocol to match (0 for non-IP or wildcard)
 * @match_ip_proto: whether to include ip_proto in match criteria
 *
 * Static table of known packet types. Used by fdice_ttc_add_default_rules()
 * to populate the TTC flow table with one entry per type.
 */
struct fdice_ttc_rule_def {
	__be16	ethertype;
	u8	ip_proto;
	bool	match_ip_proto;
};

static const struct fdice_ttc_rule_def
fdice_ttc_rule_defs[FDICE_TTC_NUM_RULES] = {
	[FDICE_TTC_IPV4_TCP] = {
		.ethertype	= cpu_to_be16(ETH_P_IP),
		.ip_proto	= IPPROTO_TCP,
		.match_ip_proto	= true,
	},
	[FDICE_TTC_IPV4_UDP] = {
		.ethertype	= cpu_to_be16(ETH_P_IP),
		.ip_proto	= IPPROTO_UDP,
		.match_ip_proto	= true,
	},
	[FDICE_TTC_IPV6_TCP] = {
		.ethertype	= cpu_to_be16(ETH_P_IPV6),
		.ip_proto	= IPPROTO_TCP,
		.match_ip_proto	= true,
	},
	[FDICE_TTC_IPV6_UDP] = {
		.ethertype	= cpu_to_be16(ETH_P_IPV6),
		.ip_proto	= IPPROTO_UDP,
		.match_ip_proto	= true,
	},
	[FDICE_TTC_IPV4_OTHER] = {
		.ethertype	= cpu_to_be16(ETH_P_IP),
		.ip_proto	= 0,
		.match_ip_proto	= false,
	},
	[FDICE_TTC_IPV6_OTHER] = {
		.ethertype	= cpu_to_be16(ETH_P_IPV6),
		.ip_proto	= 0,
		.match_ip_proto	= false,
	},
	[FDICE_TTC_ARP] = {
		.ethertype	= cpu_to_be16(ETH_P_ARP),
		.ip_proto	= 0,
		.match_ip_proto	= false,
	},
};

/* -----------------------------------------------------------------------
 * TTC Rule Creation Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_ttc_build_match - Build match values for a TTC rule type
 * @def: static rule definition for the packet type
 * @match: output match values to populate
 * @criteria: output match criteria bitmask to populate
 *
 * Translates a TTC rule definition into the structured match format
 * used by the flow steering tree. Only ethertype and optionally IP
 * protocol are matched -- TTC rules are broad classifiers, not
 * specific flow filters.
 */
static void fdice_ttc_build_match(const struct fdice_ttc_rule_def *def,
				  struct fdice_match_values *match,
				  struct fdice_match_criteria *criteria)
{
	memset(match, 0, sizeof(*match));
	memset(criteria, 0, sizeof(*criteria));

	match->ethertype = def->ethertype;
	criteria->outer_headers = FDICE_MATCH_ETHERTYPE;

	if (def->match_ip_proto) {
		match->ip_proto = def->ip_proto;
		criteria->outer_headers |= FDICE_MATCH_IP_PROTO;
	}
}

/**
 * fdice_ttc_create_rule - Create a single TTC flow entry
 * @ttc: TTC table containing the flow table
 * @rule_type: packet type index (enum fdice_ttc_rule_type)
 *
 * Creates a flow entry in the TTC flow table for the given packet type.
 * The entry steers matched packets to queue 0 (the base RSS queue);
 * actual RSS distribution is handled by the hardware RSS indirection
 * table, which is programmed separately.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_ttc_create_rule(struct fdice_ttc_table *ttc,
				 enum fdice_ttc_rule_type rule_type)
{
	const struct fdice_ttc_rule_def *def = &fdice_ttc_rule_defs[rule_type];
	struct fdice_flow_steering *fs = ttc->fdev->flow_steering;
	struct fdice_match_values match;
	struct fdice_match_criteria criteria;
	struct fdice_flow_action action;
	struct fdice_flow_entry *fe;

	fdice_ttc_build_match(def, &match, &criteria);

	/*
	 * TTC rules forward to the base RSS queue (queue 0). The E810
	 * RSS indirection table distributes packets across the actual
	 * queue set. This keeps TTC rules stable across RSS reconfig --
	 * only the indirection table changes, not the flow entries.
	 */
	action.type = FDICE_FLOW_ACTION_FORWARD;
	action.forward.dest_vsi = 0;
	action.forward.dest_queue = 0;

	fe = fdice_flow_entry_create_autogroup(fs->nic_rx_ns,
					       FDICE_FS_PRIO_RSS_DEFAULT,
					       &match, &criteria, &action);
	if (IS_ERR(fe))
		return PTR_ERR(fe);

	ttc->rules[rule_type] = fe;
	return 0;
}

/* -----------------------------------------------------------------------
 * TTC Lifecycle
 * ----------------------------------------------------------------------- */

/**
 * fdice_ttc_add_default_rules - Populate TTC table with default rules
 * @ttc: TTC table to populate
 *
 * Creates one flow entry per recognized packet type. Rules are created
 * in order; if any rule fails, previously created rules remain (partial
 * success). The caller can still call fdice_ttc_cleanup() to remove
 * whatever was created.
 *
 * Return: 0 if all rules created successfully, negative errno on first
 *         failure (with partial rules remaining in the table)
 */
static int fdice_ttc_add_default_rules(struct fdice_ttc_table *ttc)
{
	int err;
	int i;

	for (i = 0; i < FDICE_TTC_NUM_RULES; i++) {
		err = fdice_ttc_create_rule(ttc, i);
		if (err) {
			netdev_warn(ttc->fdev->priv.netdev,
				    "TTC rule %d creation failed: %d\n",
				    i, err);
			return err;
		}
	}

	return 0;
}

/**
 * fdice_ttc_init - Create and populate the TTC flow table
 * @fdev: FrankenDrICE device
 *
 * Allocates the TTC table structure, creates a flow table in the NIC RX
 * namespace at RSS/default priority (the lowest priority level), and
 * populates it with default classification rules for common packet types.
 *
 * The TTC table is stored in the flow steering root structure for access
 * by RSS reconfiguration and cleanup paths.
 *
 * Return: 0 on success, negative errno on failure. On failure, all
 *         partially created resources are cleaned up.
 */
int fdice_ttc_init(struct fdice_dev *fdev)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_ttc_table *ttc;
	int err;

	if (!fs || !fs->nic_rx_ns)
		return -ENODEV;

	ttc = kzalloc(sizeof(*ttc), GFP_KERNEL);
	if (!ttc)
		return -ENOMEM;

	ttc->fdev = fdev;

	/*
	 * Create the flow table at RSS/default priority. This is the
	 * catch-all table -- packets that don't match any higher-priority
	 * rule (TC, FDIR, ADQ, VLAN, MAC) land here for RSS distribution.
	 */
	{
		struct fdice_flow_priority *prio;

		prio = fdice_flow_namespace_find_prio(fs->nic_rx_ns,
						      FDICE_FS_PRIO_RSS_DEFAULT);
		if (!prio) {
			netdev_err(fdev->priv.netdev,
				   "TTC: RSS default priority not found\n");
			kfree(ttc);
			return -ENOENT;
		}

		ttc->ft = fdice_flow_table_create(prio, FDICE_TTC_NUM_RULES);
	}
	if (IS_ERR(ttc->ft)) {
		err = PTR_ERR(ttc->ft);
		netdev_err(fdev->priv.netdev,
			   "TTC flow table creation failed: %d\n", err);
		kfree(ttc);
		return err;
	}

	err = fdice_ttc_add_default_rules(ttc);
	if (err) {
		netdev_err(fdev->priv.netdev,
			   "TTC default rules failed: %d\n", err);
		fdice_ttc_cleanup(fdev);
		kfree(ttc);
		return err;
	}

	fs->ttc = ttc;
	return 0;
}

/**
 * fdice_ttc_cleanup - Destroy the TTC flow table and all its rules
 * @fdev: FrankenDrICE device
 *
 * Removes all TTC rules and destroys the flow table. The flow steering
 * tree's cascade mechanism ensures that destroying the table also removes
 * all child flow entries from hardware.
 *
 * Safe to call even if fdice_ttc_init() was never called or failed
 * partway through.
 */
void fdice_ttc_cleanup(struct fdice_dev *fdev)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_ttc_table *ttc;
	int i;

	if (!fs || !fs->ttc)
		return;

	ttc = fs->ttc;

	/* Destroy individual rules first for clean per-entry teardown */
	for (i = 0; i < FDICE_TTC_NUM_RULES; i++) {
		if (ttc->rules[i]) {
			fdice_flow_entry_destroy(ttc->rules[i]);
			ttc->rules[i] = NULL;
		}
	}

	/* Destroy the flow table (cascades any remaining children) */
	if (ttc->ft) {
		fdice_flow_table_destroy(ttc->ft);
		ttc->ft = NULL;
	}

	fs->ttc = NULL;
	kfree(ttc);
}

/**
 * fdice_ttc_update_rules - Update TTC rules after RSS reconfiguration
 * @fdev: FrankenDrICE device
 *
 * Called when RSS configuration changes (indirection table, hash key,
 * or queue count). Updates the action destinations of all TTC rules
 * to reflect the new queue mapping.
 *
 * For E810, RSS distribution is handled by the hardware indirection
 * table rather than per-rule queue assignment. Therefore, TTC rule
 * updates are typically a no-op -- the indirection table is reprogrammed
 * separately through the AQ. This function exists to handle any
 * structural changes that require rule updates, such as changing from
 * RSS to a single-queue configuration.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_ttc_update_rules(struct fdice_dev *fdev)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_ttc_table *ttc;
	int err;
	int i;

	if (!fs || !fs->ttc)
		return -ENODEV;

	ttc = fs->ttc;

	for (i = 0; i < FDICE_TTC_NUM_RULES; i++) {
		struct fdice_flow_entry *fe = ttc->rules[i];

		if (!fe)
			continue;

		/*
		 * Update the forwarding action. For standard RSS, queue 0
		 * remains the destination and the indirection table handles
		 * actual distribution. For non-RSS modes, this would change
		 * to the specific target queue.
		 */
		fe->action.type = FDICE_FLOW_ACTION_FORWARD;
		fe->action.forward.dest_queue = 0;

		/*
		 * Notify the command vtable of the action update so it
		 * can reprogram the hardware rule if necessary.
		 */
		if (fs->cmds && fs->cmds->update_flow_entry) {
			err = fs->cmds->update_flow_entry(fe);
			if (err) {
				netdev_warn(fdev->priv.netdev,
					    "TTC rule %d update failed: %d\n",
					    i, err);
				return err;
			}
		}
	}

	return 0;
}
