<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Exploration 14: ADQ (Application Device Queues)

## Overview

ADQ is ice's mechanism for dedicating hardware queue resources to specific
application traffic classes. It builds on the Linux TC mqprio qdisc in
`TC_MQPRIO_MODE_CHANNEL` mode, creating per-TC VSIs (called "channels") with
dedicated queue pairs and optional bandwidth limits. TC flower filters then
steer matching traffic into the appropriate channel VSI.

## Key Source Files

| File | Role |
|------|------|
| `ice_main.c` | `ice_setup_tc` (ndo_setup_tc), mqprio qdisc setup, channel creation |
| `ice_tc_lib.c` | TC flower filter parsing, add/delete, advanced switch rule creation |
| `ice_tc_lib.h` | `ice_tc_flower_fltr`, `ice_tc_flower_action`, match header structs |
| `ice.h` | `ice_channel`, `ice_tc_info`, `ice_tc_cfg`, `ice_is_adq_active()` |
| `ice_lib.c` | `ice_vsi_cfg_tc()`, `ice_vsi_rebuild()`, `ice_vsi_cfg_netdev_tc()` |

## ADQ Architecture

### Entry Point: `ice_setup_tc` (ndo_setup_tc)

Registered as `.ndo_setup_tc` in `net_device_ops`. Dispatches three TC setup
types:

1. **TC_SETUP_BLOCK** -- TC flower filter offload (ingress/egress cls_flower)
2. **TC_SETUP_QDISC_MQPRIO** -- ADQ queue allocation via mqprio qdisc
3. **TC_SETUP_QDISC_ETF** -- Earliest TxTime First (txtime offload)

Critical constraint: **ADQ and switchdev are mutually exclusive**.
`ice_setup_tc` rejects `TC_SETUP_QDISC_MQPRIO` when
`ice_is_eswitch_mode_switchdev()` is true, and `ice_eswitch_mode_set` rejects
switchdev when `ice_is_adq_active()` is true.

### ADQ Detection: `ice_is_adq_active`

```
ADQ is active when:
  vsi->tc_cfg.numtc > ICE_CHNL_START_TC  (more than default TC0)
  AND ICE_FLAG_TC_MQPRIO is set in pf->flags
```

This check is used throughout the driver (12+ call sites) to guard against
conflicting operations: ethtool channel changes, RSS reconfiguration, eswitch
mode changes, devlink reload, FDIR rule programming, and feature toggles.

### TC-to-Queue Mapping via mqprio

The `ice_setup_tc_mqprio_qdisc` function (169 LOC, complexity 35) is the core
ADQ configuration path:

**Phase 1 -- Validation** (`ice_validate_mqprio_qopt`):
- Offset[0] must be 0; num_tc range [1, ICE_CHNL_MAX_TC]
- Queue counts: power-of-2 preferred; at most one non-power-of-2 count allowed
- Non-power-of-2 count must be >= all power-of-2 counts
- Bandwidth rates: min_rate must be multiple of ICE_MIN_BW_LIMIT (in Kbps)
- max_rate cannot exceed link speed; sum of min_rates cannot exceed link speed
- Queue offsets must be contiguous (offset[i+1] == offset[i] + count[i])
- Total queues must fit within VSI's existing Tx/Rx queue count

**Phase 2 -- VSI Rebuild**:
- Pauses VSI via `ice_dis_vsi`
- If disabling ADQ (hw=0): removes channel VSIs, restores queue count
- If enabling ADQ: calculates total queues from mqprio offsets + counts
- Saves original RSS size for later restoration
- Rebuilds main VSI via `ice_vsi_rebuild(vsi, ICE_VSI_FLAG_NO_INIT)`
- Falls back to current queue count if rebuild fails

**Phase 3 -- TC Configuration**:
- Sets `vsi->all_numtc` and `vsi->all_enatc` (TC enable bitmap)
- Calls `ice_vsi_cfg_tc` to configure hardware TC mapping
- Tears down devlink rate tree (ADQ takes over BW management)

**Phase 4 -- Channel Creation** (`ice_create_q_channels`):
- For each TC beyond TC0, creates an `ice_channel` structure
- Each channel maps to a new child VSI (`ice_setup_channel`)
- Per-channel BW limits via `ice_set_bw_limit` (max_tx_rate, min_tx_rate)
- Decrements `vsi->cnt_q_avail` per channel

**Phase 5 -- RSS Reconfiguration**:
- If `ch_rss_size` is set, reconfigures RSS LUT/key via `ice_vsi_cfg_rss_lut_key`

### The `ice_channel` Structure

```c
struct ice_channel {
    struct list_head list;      /* linked list on main VSI */
    u8 type;                    /* channel type */
    u16 sw_id;                  /* switch ID */
    u16 base_q;                 /* base queue index */
    u16 num_rxq;                /* Rx queues in this channel */
    u16 num_txq;                /* Tx queues in this channel */
    u16 vsi_num;                /* HW VSI number */
    u8 ena_tc;                  /* enabled TC bitmap */
    struct ice_aqc_vsi_props info;  /* AQ VSI properties */
    u64 max_tx_rate;            /* max bandwidth in Kbps */
    u64 min_tx_rate;            /* guaranteed bandwidth in Kbps */
    atomic_t num_sb_fltr;       /* sideband filter count */
    struct ice_vsi *ch_vsi;     /* pointer to channel VSI */
};
```

Each channel beyond TC0 gets its own VSI. The main PF VSI handles TC0, while
channel VSIs handle TC1..TC_MAX. This maps to E810's VSI-based traffic
isolation model.

### TC Information Structures

```c
struct ice_tc_info {
    u16 qoffset;     /* queue offset for this TC */
    u16 qcount_tx;   /* Tx queue count */
    u16 qcount_rx;   /* Rx queue count */
    u8 netdev_tc;    /* netdev TC number */
};

struct ice_tc_cfg {
    u8 numtc;        /* total enabled TCs */
    u16 ena_tc;      /* TC enable bitmap */
    struct ice_tc_info tc_info[ICE_MAX_TRAFFIC_CLASS];
};
```

## TC Flower Offload for ADQ

### Flow: PF netdev flower filters

```
ndo_setup_tc(TC_SETUP_BLOCK)
  -> flow_block_cb_setup_simple (ingress or egress binder)
    -> ice_setup_tc_cls_flower
      -> FLOW_CLS_REPLACE: ice_add_cls_flower
      -> FLOW_CLS_DESTROY: ice_del_cls_flower
```

### `ice_add_cls_flower` Flow

1. Check reset not in progress
2. Reject if FW LLDP agent is active
3. Check NETIF_F_HW_TC or ICE_FLAG_CLS_FLOWER
4. Duplicate check via `ice_find_tc_flower_fltr` (cookie-based lookup)
5. Call `ice_add_tc_fltr` -- parses flower match keys, builds advanced switch
   filter rule, programs via AQ
6. Add to `pf->tc_flower_fltr_list` (hlist)

### `ice_tc_flower_fltr` Structure

The filter structure tracks:
- **cookie** -- becomes filter_rule_id on success
- **rid / rule_id** -- recipe ID and rule ID from AQ (needed for removal)
- **dest_vsi_handle / dest_vsi** -- target VSI (main, channel, or VF)
- **direction** -- eswitch filter direction
- **outer_headers / inner_headers** -- L2-L4 match fields
- **tunnel_type** -- GTP, GRE, VXLAN etc.
- **action** -- forwarding action with TC and queue info

### Indirect Block Callbacks

Ice also supports indirect TC block offload for tunnel devices:
- `ice_indr_setup_tc_block` -- registers indirect block callbacks
- `ice_indr_setup_block_cb` -- handles flower rules on tunnel netdevs
- Maintains `tc_indr_block_priv_list` per netdev_priv

This allows flower rules on VXLAN/GRE tunnel interfaces to be offloaded to
E810 hardware.

### Match Capabilities via `ice_parse_cls_flower`

The parsing function (286 LOC) extracts match fields from TC flower:
- L2: Ethernet src/dst MAC, EtherType, VLAN ID/priority
- L3: IPv4/IPv6 src/dst addresses, TOS/TTL
- L4: TCP/UDP/SCTP src/dst ports
- Tunnel: GTP TEID, GRE key, VXLAN VNI
- Protocol: PFCP, PPPoE

These map to E810's advanced switch recipe system, which programs extraction
words (protocol ID + offset pairs) for hardware matching.

## ADQ Constraints and Interactions

| Feature | Interaction with ADQ |
|---------|---------------------|
| Switchdev mode | Mutually exclusive -- cannot enable both |
| RDMA (aux device) | RDMA must be inactive to change mqprio |
| Custom Tx scheduler | Cannot configure ADQ when custom TX enabled |
| Devlink rate tree | Torn down when ADQ is configured |
| Ethtool channels | Blocked when ADQ is active |
| RSS configuration | Restricted when ADQ is active |
| FDIR rules | Must account for ADQ channel VSIs |
| Devlink reload | Checks ADQ state |

## Comparison with mlx5 TC/QoS

| Aspect | ice ADQ | mlx5 TC |
|--------|---------|---------|
| TC mechanism | mqprio MODE_CHANNEL + per-TC VSIs | mqprio + HTB qdisc + flow tables |
| Queue isolation | Separate VSI per TC | Queue groups within single netdev |
| BW shaping | Per-channel min/max via Tx scheduler | HTB hierarchy with tc_bw per node |
| Flower offload | Advanced switch recipes (flat) | FDB flow tables (hierarchical) |
| Tunnel offload | Basic (GTP, VXLAN match) | Rich (encap/decap/CT/mod_hdr) |
| Rate limiting | Per-VSI Tx scheduler nodes | Per-SQ rate limiting + TSAR tree |
| Indirect blocks | Supported for tunnel netdevs | Supported with richer actions |

## fdice_ Design Recommendations

### 1. Unified TC Management Layer

Create `fdice_tc_mgr` that abstracts ADQ configuration:
- Decouple TC-to-queue mapping from VSI lifecycle
- Use a `fdice_traffic_class` structure that cleanly owns its queue range,
  BW limits, and filter list
- Separate validation, VSI rebuild, and filter programming into distinct phases
  with proper rollback

### 2. Cleaner Channel Abstraction

Replace ice's channel-as-linked-list pattern:
- Use xarray for channel management (consistent with eswitch repr pattern)
- `fdice_channel` should own its VSI reference, queue range, and associated
  flower filters as a cohesive unit
- Implement proper channel lifecycle (create/configure/activate/deactivate/destroy)

### 3. TC Flower Filter Management

Improve over ice's hlist-based filter tracking:
- Use rhashtable for O(1) cookie lookup (ice uses hlist linear scan via
  `ice_find_tc_flower_fltr`)
- Integrate filter management into the flow steering tree (see Exploration 04)
- Share filter infrastructure between ADQ, switchdev, and FDIR paths

### 4. ADQ/Switchdev Coexistence Path

The mutual exclusion between ADQ and switchdev is a significant limitation:
- Design `fdice_tc_mgr` to work in both modes from the start
- In switchdev mode, TC flower filters go through eswitch FDB path
- In legacy mode, TC flower filters go through PF advanced switch rules
- Same `fdice_tc_flower_fltr` structure should work in both contexts

### 5. BW Management Integration

Unify bandwidth control:
- ADQ per-channel BW limits, devlink rate nodes, and QoS scheduling should
  share the same Tx scheduler tree abstraction (`fdice_sched_node`)
- Avoid the ice pattern where ADQ tears down the devlink rate tree
- Instead, integrate ADQ channels as nodes in the scheduling hierarchy

### 6. Validation as Separate Phase

Extract `ice_validate_mqprio_qopt` pattern into a reusable validator:
- `fdice_tc_validate` -- checks queue counts, BW limits, conflicts
- Returns structured error info (not just -EINVAL)
- Can be called from ethtool, devlink, or netlink paths

## E810 Hardware Considerations

- **Max TCs**: ICE_CHNL_MAX_TC (limited by hardware scheduler levels)
- **Queue granularity**: Power-of-2 preferred for RSS compatibility
- **BW granularity**: ICE_MIN_BW_LIMIT (50 Mbps minimum in Kbps)
- **VSI limit**: Each ADQ channel consumes a VSI; plan allocation carefully
- **Switch recipes**: Each flower filter pattern needs a recipe; max 64 recipes
- **FDIR interaction**: ADQ channels need FDIR group membership for per-flow steering
