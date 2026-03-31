// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_IIDC_H_
#define _FDICE_IIDC_H_

/**
 * DOC: FrankenDrICE IIDC (Inter-Driver Communication) Interface
 *
 * Public header consumed by the RDMA provider auxiliary module.
 * Defines the capability snapshot, ops vtable, and event notification
 * structures that form the contract between the PF driver and the
 * RDMA auxiliary driver.
 *
 * Key improvement over ice/irdma IIDC: exported GPL symbols are replaced
 * by an ops vtable passed at probe time, providing compile-time type
 * safety and eliminating symbol version coupling between modules.
 *
 * Architecture reference: docs/architecture/03-rdma-integration.md
 */

#include <linux/types.h>
#include <linux/auxiliary_bus.h>
#include <linux/netdevice.h>
#include <linux/pci.h>
#include <linux/list.h>

/* Forward declarations */
struct fdice_dev;
struct fdice_iidc_core;

/* -----------------------------------------------------------------------
 * RDMA Protocol Selection
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_rdma_protocol - RDMA transport protocol type
 * @FDICE_RDMA_PROTOCOL_IWARP: iWARP transport
 * @FDICE_RDMA_PROTOCOL_ROCE:  RoCEv2 transport
 */
enum fdice_rdma_protocol {
	FDICE_RDMA_PROTOCOL_IWARP = 0,
	FDICE_RDMA_PROTOCOL_ROCE,
};

/* -----------------------------------------------------------------------
 * RDMA Feature Flags
 * ----------------------------------------------------------------------- */

#define FDICE_RDMA_FEAT_IWARP		BIT(0)
#define FDICE_RDMA_FEAT_ROCE		BIT(1)
#define FDICE_RDMA_FEAT_SRQ		BIT(2)
#define FDICE_RDMA_FEAT_MR_LARGE	BIT(3)
#define FDICE_RDMA_FEAT_INLINE_DATA	BIT(4)

/* -----------------------------------------------------------------------
 * RDMA Capability Snapshot
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_rdma_caps - RDMA capability snapshot from HW discovery
 * @max_qps: maximum queue pairs available
 * @max_cqs: maximum completion queues
 * @max_mrs: maximum memory regions
 * @max_ceqs: maximum completion event queues
 * @max_pd: maximum protection domains
 * @max_srqs: maximum shared receive queues
 * @hw_rev: E810 hardware stepping revision
 * @features: bitmask of FDICE_RDMA_FEAT_* flags
 * @max_mtu: maximum supported MTU for RDMA
 * @rdma_msix_allocated: number of pre-partitioned MSI-X vectors for RDMA
 *
 * Populated during fdice_init_once() from AQ capability queries.
 * Immutable after probe; updated only during auxiliary device replug.
 * Richer than ice IIDC -- the RDMA driver can size resources optimally
 * at probe time without runtime queries to the PF driver.
 */
struct fdice_rdma_caps {
	u32	max_qps;
	u32	max_cqs;
	u32	max_mrs;
	u32	max_ceqs;
	u32	max_pd;
	u32	max_srqs;
	u16	hw_rev;
	u32	features;
	u16	max_mtu;
	u16	rdma_msix_allocated;
};

/* -----------------------------------------------------------------------
 * QoS Snapshot
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_rdma_qos - QoS configuration snapshot for RDMA
 * @num_tc: number of active traffic classes
 * @up_to_tc_map: user priority to traffic class mapping
 * @tc_to_bw_map: traffic class to relative bandwidth weight
 * @pfc_enabled: PFC enable bitmask (per TC)
 * @dscp_mode: true if DSCP QoS, false if VLAN PCP
 * @dscp_map: DSCP value to user priority mapping (when dscp_mode)
 *
 * Updated and re-delivered to RDMA driver on TC change events.
 */
struct fdice_rdma_qos {
	u8	num_tc;
	u8	up_to_tc_map[8];
	u8	tc_to_bw_map[8];
	u8	pfc_enabled;
	bool	dscp_mode;
	u8	dscp_map[64];
};

/* -----------------------------------------------------------------------
 * QSet Parameters
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_rdma_qset_params - RDMA queue set add/delete parameters
 * @vsi_id: VSI to which the queue set is attached
 * @tc: traffic class for this queue set
 * @num_qs: number of queue pairs in the set
 * @teid: scheduler TEID (output, filled by PF on add)
 * @qs_handle: queue set handle (output, filled by PF on add)
 *
 * Caller-allocated. For add operations the PF fills teid and qs_handle
 * via AQ command 0x0C33.
 */
struct fdice_rdma_qset_params {
	u16	vsi_id;
	u8	tc;
	u16	num_qs;
	u32	teid;
	u16	qs_handle;
};

/* -----------------------------------------------------------------------
 * Event Notification
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_iidc_event_type - IIDC event types with pre/post semantics
 * @FDICE_IIDC_EVENT_PRE_RESET: save state and stop operations before reset
 * @FDICE_IIDC_EVENT_POST_RESET: restore state and restart after reset
 * @FDICE_IIDC_EVENT_MTU_CHANGE: MTU has been changed
 * @FDICE_IIDC_EVENT_PRE_TC_CHANGE: quiesce RDMA traffic before TC reconfig
 * @FDICE_IIDC_EVENT_POST_TC_CHANGE: resume with new QoS after TC reconfig
 * @FDICE_IIDC_EVENT_LINK_CHANGE: link state transition (up or down)
 * @FDICE_IIDC_EVENT_CRITICAL_ERROR: unrecoverable hardware error
 *
 * Synchronous pre/post semantics eliminate the race window present in
 * ice/irdma where the RDMA driver might still be processing traffic
 * during PF reconfiguration.
 */
enum fdice_iidc_event_type {
	FDICE_IIDC_EVENT_PRE_RESET = 0,
	FDICE_IIDC_EVENT_POST_RESET,
	FDICE_IIDC_EVENT_MTU_CHANGE,
	FDICE_IIDC_EVENT_PRE_TC_CHANGE,
	FDICE_IIDC_EVENT_POST_TC_CHANGE,
	FDICE_IIDC_EVENT_LINK_CHANGE,
	FDICE_IIDC_EVENT_CRITICAL_ERROR,
};

/**
 * struct fdice_iidc_event - IIDC event descriptor
 * @type: event type from &enum fdice_iidc_event_type
 * @mtu: new MTU value (valid for MTU_CHANGE)
 * @qos: updated QoS snapshot pointer (valid for POST_TC_CHANGE)
 * @link_up: link state (valid for LINK_CHANGE)
 * @error_code: hardware error code (valid for CRITICAL_ERROR)
 *
 * Passed to event handlers. Union contains type-specific data to
 * avoid unnecessary memory allocation per event dispatch.
 */
struct fdice_iidc_event {
	enum fdice_iidc_event_type	type;
	union {
		u32			mtu;
		struct fdice_rdma_qos	*qos;
		bool			link_up;
		u32			error_code;
	};
};

/**
 * struct fdice_iidc_event_handler - RDMA driver event callback registration
 * @handle_event: synchronous event callback function
 * @priv: opaque private data for the RDMA driver
 * @list: internal linkage for handler list
 *
 * For PRE_* events: return 0 to acknowledge readiness, or -EBUSY
 * to request a delay. The PF retries after brief backoff up to a
 * configurable timeout before proceeding anyway.
 *
 * For POST_* events: return value is logged but does not block the PF.
 */
struct fdice_iidc_event_handler {
	int (*handle_event)(struct fdice_iidc_core *core,
			    struct fdice_iidc_event *event);
	void			*priv;
	struct list_head	list;
};

/* -----------------------------------------------------------------------
 * Ops Vtable
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_iidc_ops - Operations provided by PF driver to RDMA driver
 * @add_qset: issue AQ command to add RDMA queue set
 * @del_qset: issue AQ command to delete RDMA queue set
 * @request_reset: request PF-level or core reset on behalf of RDMA
 * @update_vsi_filter: enable/disable RDMA packet filtering on a VSI
 * @alloc_vectors: allocate MSI-X vectors from RDMA partition
 * @free_vectors: release MSI-X vectors back to RDMA partition
 * @register_event_handler: register for IIDC event notifications
 * @unregister_event_handler: unregister from IIDC event notifications
 *
 * All operations are synchronous unless documented otherwise.
 * The caller must not hold any PF-internal locks when calling these.
 * All functions return 0 on success, negative errno on failure.
 *
 * Replaces ice/irdma's exported GPL symbols with a compile-time safe
 * vtable passed at auxiliary device probe.
 */
struct fdice_iidc_ops {
	int (*add_qset)(struct fdice_iidc_core *core,
			struct fdice_rdma_qset_params *params);
	int (*del_qset)(struct fdice_iidc_core *core,
			struct fdice_rdma_qset_params *params);

	int (*request_reset)(struct fdice_iidc_core *core,
			     enum fdice_iidc_event_type type);

	int (*update_vsi_filter)(struct fdice_iidc_core *core,
				 u16 vsi_id, bool enable);

	int (*alloc_vectors)(struct fdice_iidc_core *core,
			     u16 num_vecs, u16 *base_vec);
	void (*free_vectors)(struct fdice_iidc_core *core,
			     u16 num_vecs, u16 base_vec);

	int (*register_event_handler)(struct fdice_iidc_core *core,
				      struct fdice_iidc_event_handler *handler);
	void (*unregister_event_handler)(struct fdice_iidc_core *core,
					 struct fdice_iidc_event_handler *handler);
};

/* -----------------------------------------------------------------------
 * IIDC Core Context
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_iidc_core - Complete RDMA integration context
 * @pdev: PCI device for DMA mapping
 * @netdev: associated network device for L2 info
 * @rdma_hw_addr: RDMA-specific MMIO base (separate from networking BAR)
 * @pf_id: physical function number
 * @vport_id: virtual port identifier
 * @protocol: selected RDMA transport protocol
 * @caps: hardware capability snapshot
 * @qos: QoS configuration snapshot (updated on TC change events)
 * @ops: PF-provided operations vtable
 * @pf_priv: opaque PF driver private data (RDMA driver must not dereference)
 *
 * Passed to the RDMA auxiliary driver at probe time. The PF driver
 * owns this structure; the RDMA driver holds a const reference.
 * Updated atomically during replug cycles.
 */
struct fdice_iidc_core {
	struct pci_dev			*pdev;
	struct net_device		*netdev;
	void __iomem			*rdma_hw_addr;
	u16				pf_id;
	u16				vport_id;

	enum fdice_rdma_protocol	protocol;

	struct fdice_rdma_caps		caps;
	struct fdice_rdma_qos		qos;

	const struct fdice_iidc_ops	*ops;

	void				*pf_priv;
};

/* -----------------------------------------------------------------------
 * Auxiliary Device Wrapper
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_iidc_aux_dev - Auxiliary device with IIDC core reference
 * @adev: embedded auxiliary device for bus registration
 * @core: pointer to the IIDC core capability snapshot
 *
 * Wraps the standard auxiliary_device so the RDMA auxiliary driver can
 * retrieve the fdice_iidc_core via container_of on the auxiliary_device.
 */
struct fdice_iidc_aux_dev {
	struct auxiliary_device		adev;
	struct fdice_iidc_core		*core;
};

/**
 * fdice_iidc_aux_to_core - Extract IIDC core from auxiliary device
 * @adev: auxiliary device pointer received in probe callback
 *
 * Return: pointer to fdice_iidc_core
 */
static inline struct fdice_iidc_core *
fdice_iidc_aux_to_core(struct auxiliary_device *adev)
{
	return container_of(adev, struct fdice_iidc_aux_dev, adev)->core;
}

/* -----------------------------------------------------------------------
 * IIDC Lifecycle Functions (implemented in fdice_iidc.c)
 * ----------------------------------------------------------------------- */

int fdice_iidc_init(struct fdice_dev *fdev);
void fdice_iidc_cleanup(struct fdice_dev *fdev);
int fdice_iidc_notify_event(struct fdice_dev *fdev,
			    struct fdice_iidc_event *event);

#endif /* _FDICE_IIDC_H_ */
