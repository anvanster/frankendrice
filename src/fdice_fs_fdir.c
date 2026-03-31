// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FDIR Wrapper -- Structured Match to Dummy Packet Translation
 *
 * E810 Flow Director programs filters by submitting a "dummy packet" --
 * a literal packet template that the hardware matches against incoming
 * traffic. Ice exposes this detail to callers, forcing each subsystem
 * (ethtool ntuple, ADQ, sideband) to construct dummy packets directly.
 *
 * FrankenDrICE hides the dummy packet format behind the same structured
 * fdice_match_values API used for all flow steering rules. Callers
 * specify match fields (IP addresses, ports, protocol); this module
 * translates those fields into the dummy packet bytes that E810 expects.
 *
 * FDIR rules live in the NIC RX namespace at priority 1 (below TC
 * offload, above ADQ channel steering).
 *
 * Architecture reference: docs/architecture/04-flow-steering.md
 */

#include <linux/types.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/netdevice.h>
#include <linux/etherdevice.h>
#include <linux/if_ether.h>
#include <linux/ip.h>
#include <linux/ipv6.h>
#include <linux/tcp.h>
#include <linux/udp.h>
#include <linux/ethtool.h>

#include "fdice_dev.h"
#include "fdice_fs_core.h"
#include "fdice_fs_cmd.h"

/* fdice_fdir_params defined in fdice_fs_core.h */

/* -----------------------------------------------------------------------
 * Dummy Packet Construction
 * ----------------------------------------------------------------------- */

/**
 * fdice_fdir_build_ipv4_tcp - Build an IPv4/TCP dummy packet
 * @params: FDIR parameters with packet buffer to populate
 * @match: structured match values containing addresses and ports
 *
 * Constructs a minimal Ethernet + IPv4 + TCP packet template with
 * the match fields placed at the correct offsets. The E810 firmware
 * extracts these fields from the dummy packet during filter programming.
 *
 * Return: total packet length in bytes
 */
static u16 fdice_fdir_build_ipv4_tcp(struct fdice_fdir_params *params,
				     const struct fdice_match_values *match)
{
	struct ethhdr *eth;
	struct iphdr *iph;
	struct tcphdr *tcph;
	u16 pkt_len;

	memset(params->pkt_buf, 0, FDICE_FDIR_MAX_PKT_LEN);

	/* Ethernet header */
	eth = (struct ethhdr *)params->pkt_buf;
	eth->h_proto = htons(ETH_P_IP);

	/* IPv4 header */
	iph = (struct iphdr *)(eth + 1);
	iph->version = 4;
	iph->ihl = 5;
	iph->tot_len = htons(sizeof(*iph) + sizeof(*tcph));
	iph->protocol = IPPROTO_TCP;
	iph->saddr = match->ipv4_src;
	iph->daddr = match->ipv4_dst;

	/* TCP header */
	tcph = (struct tcphdr *)(iph + 1);
	tcph->source = match->sport;
	tcph->dest = match->dport;
	tcph->doff = 5;

	pkt_len = sizeof(*eth) + sizeof(*iph) + sizeof(*tcph);
	return pkt_len;
}

/**
 * fdice_fdir_build_ipv4_udp - Build an IPv4/UDP dummy packet
 * @params: FDIR parameters with packet buffer to populate
 * @match: structured match values containing addresses and ports
 *
 * Return: total packet length in bytes
 */
static u16 fdice_fdir_build_ipv4_udp(struct fdice_fdir_params *params,
				     const struct fdice_match_values *match)
{
	struct ethhdr *eth;
	struct iphdr *iph;
	struct udphdr *udph;
	u16 pkt_len;

	memset(params->pkt_buf, 0, FDICE_FDIR_MAX_PKT_LEN);

	eth = (struct ethhdr *)params->pkt_buf;
	eth->h_proto = htons(ETH_P_IP);

	iph = (struct iphdr *)(eth + 1);
	iph->version = 4;
	iph->ihl = 5;
	iph->tot_len = htons(sizeof(*iph) + sizeof(*udph));
	iph->protocol = IPPROTO_UDP;
	iph->saddr = match->ipv4_src;
	iph->daddr = match->ipv4_dst;

	udph = (struct udphdr *)(iph + 1);
	udph->source = match->sport;
	udph->dest = match->dport;
	udph->len = htons(sizeof(*udph));

	pkt_len = sizeof(*eth) + sizeof(*iph) + sizeof(*udph);
	return pkt_len;
}

/**
 * fdice_fdir_build_ipv6_tcp - Build an IPv6/TCP dummy packet
 * @params: FDIR parameters with packet buffer to populate
 * @match: structured match values containing addresses and ports
 *
 * Return: total packet length in bytes
 */
static u16 fdice_fdir_build_ipv6_tcp(struct fdice_fdir_params *params,
				     const struct fdice_match_values *match)
{
	struct ethhdr *eth;
	struct ipv6hdr *ip6h;
	struct tcphdr *tcph;
	u16 pkt_len;

	memset(params->pkt_buf, 0, FDICE_FDIR_MAX_PKT_LEN);

	eth = (struct ethhdr *)params->pkt_buf;
	eth->h_proto = htons(ETH_P_IPV6);

	ip6h = (struct ipv6hdr *)(eth + 1);
	ip6h->version = 6;
	ip6h->payload_len = htons(sizeof(*tcph));
	ip6h->nexthdr = IPPROTO_TCP;
	ip6h->hop_limit = 64;
	ip6h->saddr = match->ipv6_src;
	ip6h->daddr = match->ipv6_dst;

	tcph = (struct tcphdr *)(ip6h + 1);
	tcph->source = match->sport;
	tcph->dest = match->dport;
	tcph->doff = 5;

	pkt_len = sizeof(*eth) + sizeof(*ip6h) + sizeof(*tcph);
	return pkt_len;
}

/**
 * fdice_fdir_build_ipv6_udp - Build an IPv6/UDP dummy packet
 * @params: FDIR parameters with packet buffer to populate
 * @match: structured match values containing addresses and ports
 *
 * Return: total packet length in bytes
 */
static u16 fdice_fdir_build_ipv6_udp(struct fdice_fdir_params *params,
				     const struct fdice_match_values *match)
{
	struct ethhdr *eth;
	struct ipv6hdr *ip6h;
	struct udphdr *udph;
	u16 pkt_len;

	memset(params->pkt_buf, 0, FDICE_FDIR_MAX_PKT_LEN);

	eth = (struct ethhdr *)params->pkt_buf;
	eth->h_proto = htons(ETH_P_IPV6);

	ip6h = (struct ipv6hdr *)(eth + 1);
	ip6h->version = 6;
	ip6h->payload_len = htons(sizeof(*udph));
	ip6h->nexthdr = IPPROTO_UDP;
	ip6h->hop_limit = 64;
	ip6h->saddr = match->ipv6_src;
	ip6h->daddr = match->ipv6_dst;

	udph = (struct udphdr *)(ip6h + 1);
	udph->source = match->sport;
	udph->dest = match->dport;
	udph->len = htons(sizeof(*udph));

	pkt_len = sizeof(*eth) + sizeof(*ip6h) + sizeof(*udph);
	return pkt_len;
}

/**
 * fdice_fdir_build_dummy_pkt - Construct a dummy packet from match values
 * @params: FDIR parameters struct to populate with the dummy packet
 * @match: structured match values specifying the flow to filter
 *
 * Dispatches to the appropriate protocol-specific builder based on
 * the ethertype and IP protocol in the match values. This is the
 * central translation point that hides E810's dummy-packet programming
 * model from callers.
 *
 * Return: 0 on success, -EOPNOTSUPP if the protocol combination
 *         cannot be expressed as a dummy packet
 */
static int fdice_fdir_build_dummy_pkt(struct fdice_fdir_params *params,
				      const struct fdice_match_values *match)
{
	bool is_ipv6 = (match->ethertype == htons(ETH_P_IPV6));

	params->is_ipv6 = is_ipv6;
	params->ip_proto = match->ip_proto;

	if (is_ipv6) {
		switch (match->ip_proto) {
		case IPPROTO_TCP:
			params->pkt_len =
				fdice_fdir_build_ipv6_tcp(params, match);
			return 0;
		case IPPROTO_UDP:
			params->pkt_len =
				fdice_fdir_build_ipv6_udp(params, match);
			return 0;
		default:
			return -EOPNOTSUPP;
		}
	}

	/* Default to IPv4 */
	switch (match->ip_proto) {
	case IPPROTO_TCP:
		params->pkt_len = fdice_fdir_build_ipv4_tcp(params, match);
		return 0;
	case IPPROTO_UDP:
		params->pkt_len = fdice_fdir_build_ipv4_udp(params, match);
		return 0;
	default:
		return -EOPNOTSUPP;
	}
}

/* -----------------------------------------------------------------------
 * Match-to-Params Translation
 * ----------------------------------------------------------------------- */

/**
 * fdice_fdir_params_from_match - Translate match values to FDIR params
 * @params: output FDIR parameters to populate
 * @match: input structured match values
 * @q_index: target RX queue index
 *
 * Fills in the fdice_fdir_params structure from structured match values,
 * including building the dummy packet. This is the bridge between the
 * flow steering tree's structured API and E810's packet-template model.
 *
 * Return: 0 on success, negative errno on failure
 */
static int fdice_fdir_params_from_match(struct fdice_fdir_params *params,
					const struct fdice_match_values *match,
					u16 q_index)
{
	int err;

	memset(params, 0, sizeof(*params));
	params->q_index = q_index;

	err = fdice_fdir_build_dummy_pkt(params, match);
	if (err)
		return err;

	return 0;
}

/* -----------------------------------------------------------------------
 * FDIR Rule Add / Delete
 * ----------------------------------------------------------------------- */

/**
 * fdice_fdir_add_rule - Add an FDIR flow entry from structured match values
 * @fdev: FrankenDrICE device
 * @match: structured match values for the flow
 * @criteria: match criteria bitmask
 * @q_index: target RX queue index for matched packets
 *
 * Creates a flow entry in the FDIR flow table (NIC RX namespace,
 * priority 1). The structured match values are translated to a dummy
 * packet internally by fdice_fdir_params_from_match(), then the command
 * vtable's create_fdir_entry() programs the E810 hardware.
 *
 * Return: pointer to the created flow entry, or ERR_PTR on failure
 */
struct fdice_flow_entry *
fdice_fdir_add_rule(struct fdice_dev *fdev,
		    const struct fdice_match_values *match,
		    const struct fdice_match_criteria *criteria,
		    u16 q_index)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;
	struct fdice_fdir_params params;
	struct fdice_flow_action action;
	struct fdice_flow_entry *fe;
	int err;

	if (!fs || !fs->nic_rx_ns)
		return ERR_PTR(-ENODEV);

	/* Build FDIR parameters including dummy packet */
	err = fdice_fdir_params_from_match(&params, match, q_index);
	if (err)
		return ERR_PTR(err);

	/* Set up the forward-to-queue action */
	action.type = FDICE_FLOW_ACTION_FORWARD;
	action.forward.dest_vsi = 0; /* resolved by command layer */
	action.forward.dest_queue = q_index;

	/*
	 * Create the flow entry in the tree at FDIR priority.
	 * The command vtable implementation receives the fdice_fdir_params
	 * via the entry's hw.fdir fields after tree insertion.
	 */
	fe = fdice_flow_entry_create_autogroup(fs->nic_rx_ns,
					       FDICE_FS_PRIO_FDIR,
					       match, criteria, &action);
	if (IS_ERR(fe))
		return fe;

	fe->hw.fdir.q_index = q_index;

	/*
	 * Program the FDIR filter via the command vtable.
	 * This sends the dummy packet to the E810 firmware.
	 */
	if (fs->cmds && fs->cmds->create_fdir_entry) {
		err = fs->cmds->create_fdir_entry(fe, &params);
		if (err) {
			fdice_flow_entry_destroy(fe);
			return ERR_PTR(err);
		}
	}

	return fe;
}

/**
 * fdice_fdir_del_rule - Remove an FDIR flow entry
 * @fdev: FrankenDrICE device
 * @fe: flow entry to remove
 *
 * Removes the FDIR filter from hardware via the command vtable and
 * destroys the flow entry in the steering tree. The tree's cascade
 * mechanism handles all cleanup.
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_fdir_del_rule(struct fdice_dev *fdev,
			struct fdice_flow_entry *fe)
{
	struct fdice_flow_steering *fs = fdev->flow_steering;

	if (!fs || !fe)
		return -EINVAL;

	/* Remove from hardware first */
	if (fs->cmds && fs->cmds->delete_fdir_entry) {
		int err;

		err = fs->cmds->delete_fdir_entry(fe);
		if (err) {
			netdev_warn(fdev->priv.netdev,
				    "FDIR HW delete failed: %d\n", err);
			/* Continue with SW cleanup despite HW error */
		}
	}

	fdice_flow_entry_destroy(fe);
	return 0;
}

/* -----------------------------------------------------------------------
 * Ethtool ntuple Interface
 * ----------------------------------------------------------------------- */

/**
 * fdice_fdir_set_ntuple - Add an FDIR rule from ethtool ntuple command
 * @fdev: FrankenDrICE device
 * @cmd: ethtool rxnfc command containing the rx flow specification
 *
 * Translates an ethtool rx_flow_spec into structured match values and
 * creates an FDIR rule. Supports TCP4, UDP4, TCP6, and UDP6 flow types.
 *
 * The ethtool interface requires location-based management -- each rule
 * occupies a numbered slot. FrankenDrICE maps these locations to flow
 * entry cookies for consistent tracking.
 *
 * Return: 0 on success, -EOPNOTSUPP for unsupported flow types,
 *         -EINVAL for invalid parameters
 */
int fdice_fdir_set_ntuple(struct fdice_dev *fdev,
			  struct ethtool_rxnfc *cmd)
{
	struct ethtool_rx_flow_spec *fsp = &cmd->fs;
	struct fdice_match_values match;
	struct fdice_match_criteria criteria;
	struct fdice_flow_entry *fe;
	u16 q_index;

	memset(&match, 0, sizeof(match));
	memset(&criteria, 0, sizeof(criteria));

	q_index = (u16)fsp->ring_cookie;

	switch (fsp->flow_type & ~(FLOW_EXT | FLOW_MAC_EXT)) {
	case TCP_V4_FLOW:
		match.ethertype = htons(ETH_P_IP);
		match.ip_proto = IPPROTO_TCP;
		match.ipv4_src = fsp->h_u.tcp_ip4_spec.ip4src;
		match.ipv4_dst = fsp->h_u.tcp_ip4_spec.ip4dst;
		match.sport = fsp->h_u.tcp_ip4_spec.psrc;
		match.dport = fsp->h_u.tcp_ip4_spec.pdst;
		criteria.outer_headers = FDICE_MATCH_ETHERTYPE |
					 FDICE_MATCH_IP_PROTO |
					 FDICE_MATCH_IPV4_SRC |
					 FDICE_MATCH_IPV4_DST |
					 FDICE_MATCH_L4_SPORT |
					 FDICE_MATCH_L4_DPORT;
		break;

	case UDP_V4_FLOW:
		match.ethertype = htons(ETH_P_IP);
		match.ip_proto = IPPROTO_UDP;
		match.ipv4_src = fsp->h_u.udp_ip4_spec.ip4src;
		match.ipv4_dst = fsp->h_u.udp_ip4_spec.ip4dst;
		match.sport = fsp->h_u.udp_ip4_spec.psrc;
		match.dport = fsp->h_u.udp_ip4_spec.pdst;
		criteria.outer_headers = FDICE_MATCH_ETHERTYPE |
					 FDICE_MATCH_IP_PROTO |
					 FDICE_MATCH_IPV4_SRC |
					 FDICE_MATCH_IPV4_DST |
					 FDICE_MATCH_L4_SPORT |
					 FDICE_MATCH_L4_DPORT;
		break;

	case TCP_V6_FLOW:
		match.ethertype = htons(ETH_P_IPV6);
		match.ip_proto = IPPROTO_TCP;
		memcpy(&match.ipv6_src, fsp->h_u.tcp_ip6_spec.ip6src,
		       sizeof(match.ipv6_src));
		memcpy(&match.ipv6_dst, fsp->h_u.tcp_ip6_spec.ip6dst,
		       sizeof(match.ipv6_dst));
		match.sport = fsp->h_u.tcp_ip6_spec.psrc;
		match.dport = fsp->h_u.tcp_ip6_spec.pdst;
		criteria.outer_headers = FDICE_MATCH_ETHERTYPE |
					 FDICE_MATCH_IP_PROTO |
					 FDICE_MATCH_IPV6_SRC |
					 FDICE_MATCH_IPV6_DST |
					 FDICE_MATCH_L4_SPORT |
					 FDICE_MATCH_L4_DPORT;
		break;

	case UDP_V6_FLOW:
		match.ethertype = htons(ETH_P_IPV6);
		match.ip_proto = IPPROTO_UDP;
		memcpy(&match.ipv6_src, fsp->h_u.udp_ip6_spec.ip6src,
		       sizeof(match.ipv6_src));
		memcpy(&match.ipv6_dst, fsp->h_u.udp_ip6_spec.ip6dst,
		       sizeof(match.ipv6_dst));
		match.sport = fsp->h_u.udp_ip6_spec.psrc;
		match.dport = fsp->h_u.udp_ip6_spec.pdst;
		criteria.outer_headers = FDICE_MATCH_ETHERTYPE |
					 FDICE_MATCH_IP_PROTO |
					 FDICE_MATCH_IPV6_SRC |
					 FDICE_MATCH_IPV6_DST |
					 FDICE_MATCH_L4_SPORT |
					 FDICE_MATCH_L4_DPORT;
		break;

	default:
		return -EOPNOTSUPP;
	}

	fe = fdice_fdir_add_rule(fdev, &match, &criteria, q_index);
	if (IS_ERR(fe))
		return PTR_ERR(fe);

	/*
	 * Store the ethtool location as the cookie for later retrieval.
	 * The flow entry's cookie field provides a uniform handle across
	 * both TC flower and ethtool ntuple interfaces.
	 */
	fe->cookie = fsp->location;

	return 0;
}

/**
 * fdice_fdir_get_ntuple - Report an FDIR rule to ethtool
 * @fdev: FrankenDrICE device
 * @cmd: ethtool rxnfc command with the location to query
 *
 * Placeholder for reporting FDIR filter details back to ethtool.
 * Full implementation requires walking the FDIR flow table to find
 * the entry matching the requested location.
 *
 * Return: -EOPNOTSUPP (full implementation deferred to Phase 3.5b)
 */
int fdice_fdir_get_ntuple(struct fdice_dev *fdev,
			  struct ethtool_rxnfc *cmd)
{
	/*
	 * TODO(Phase 3.5b): Walk FDIR flow table entries and translate
	 * the matching flow entry back to ethtool_rx_flow_spec format.
	 * Requires inverse of the set_ntuple translation.
	 */
	return -EOPNOTSUPP;
}
