// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_EQ_H_
#define _FDICE_EQ_H_

/**
 * DOC: FrankenDrICE Event Queue and Interrupt Management
 *
 * Event queues (EQs) are the top level of the three-level queue hierarchy.
 * Each EQ maps to exactly one MSI-X vector and provides interrupt routing
 * for one or more completion queues via an xarray-based CQ table.
 *
 * The EQ table manages the device's MSI-X vector pool with support for
 * NUMA-aware CPU assignment and RDMA vector sharing.
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md sec 7
 */

#include <linux/types.h>
#include <linux/xarray.h>
#include <linux/cpumask.h>
#include <linux/interrupt.h>

/* Forward declarations */
struct fdice_cq;
struct fdice_dev;
struct fdice_eq_table;

/* -----------------------------------------------------------------------
 * Event Queue Types
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_eq_type - Event queue classification
 * @FDICE_EQ_COMP:  Completion EQ -- one per channel/NAPI context, services
 *                   data-path CQs for TX/RX/XDP completions
 * @FDICE_EQ_ASYNC: Async EQ -- single instance for error events, port
 *                   change notifications, AQ completions, RDMA async events.
 *                   Uses workqueue rather than NAPI for deferred processing.
 */
enum fdice_eq_type {
	FDICE_EQ_COMP = 0,
	FDICE_EQ_ASYNC,
};

/* -----------------------------------------------------------------------
 * Event Queue
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_eq - Event queue (MSI-X vector abstraction)
 * @cq_table: xarray mapping CQ numbers to fdice_cq pointers (RCU-safe)
 * @irq: MSI-X IRQ number assigned to this EQ
 * @cpu: CPU this EQ is affinity-bound to
 * @idx: EQ index within the EQ table
 * @type: EQ type from &enum fdice_eq_type
 * @itr_reg: MMIO pointer to the ITR (Interrupt Throttle Rate) register
 *           for DIM-based adaptive coalescing
 * @table: Back-pointer to parent EQ table
 *
 * Software abstraction over E810's MSI-X vector mechanism. Enables
 * dynamic CQ-to-interrupt remapping without hardware reconfiguration
 * and shared interrupt infrastructure between ethernet and RDMA.
 *
 * IRQ dispatch path:
 *   MSI-X IRQ -> fdice_eq_irq_handler(eq)
 *     -> xa_load(&eq->cq_table, cqn)
 *     -> cq->comp_handler(cq)
 */
struct fdice_eq {
	struct xarray		cq_table;
	int			irq;
	int			cpu;
	u16			idx;
	enum fdice_eq_type	type;
	void __iomem		*itr_reg;
	struct fdice_eq_table	*table;
};

/* -----------------------------------------------------------------------
 * MSI-X IRQ Pool
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_irq_pool - MSI-X vector allocation pool
 * @msix_base: First MSI-X vector index in PCI allocation
 * @msix_count: Total MSI-X vectors available
 * @rdma_base: First vector index reserved for RDMA
 * @rdma_count: Number of vectors reserved for RDMA auxiliary driver
 * @next_free: Next available vector index for allocation
 * @pdev: PCI device for irq_alloc/free operations
 *
 * Manages MSI-X vector allocation with a partition reserved for the
 * RDMA auxiliary driver. Ethernet completion EQs and the async EQ
 * allocate from the ethernet partition; RDMA EQs from the RDMA partition.
 */
struct fdice_irq_pool {
	u16			msix_base;
	u16			msix_count;
	u16			rdma_base;
	u16			rdma_count;
	u16			next_free;
	struct pci_dev		*pdev;
};

/* -----------------------------------------------------------------------
 * Event Queue Table
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_eq_table - Device-level EQ management
 * @comp_eqs: Array of completion EQs (one per channel)
 * @num_comp_eqs: Number of completion EQs allocated
 * @async_eq: Single async EQ for control/error events
 * @irq_pool: MSI-X vector allocation pool
 * @used_cpus: Bitmask tracking which CPUs have EQ affinity assignments
 *
 * Top-level EQ management structure. EQ count matches channel count
 * (one completion EQ per channel) plus one async EQ. Created during
 * device init, torn down on device removal.
 */
struct fdice_eq_table {
	struct fdice_eq		*comp_eqs;
	u16			num_comp_eqs;
	struct fdice_eq		async_eq;
	struct fdice_irq_pool	*irq_pool;
	cpumask_var_t		used_cpus;
};

/* -----------------------------------------------------------------------
 * EQ Function Declarations
 * ----------------------------------------------------------------------- */

/* EQ table lifecycle (device-level) */
int fdice_eq_table_init(struct fdice_dev *fdev, u16 num_comp);
void fdice_eq_table_cleanup(struct fdice_dev *fdev);

/* Individual EQ lifecycle */
int fdice_eq_alloc(struct fdice_eq_table *table, struct fdice_eq *eq,
		   enum fdice_eq_type type, int cpu);
void fdice_eq_free(struct fdice_eq_table *table, struct fdice_eq *eq);

/* CQ registration in EQ */
int fdice_eq_add_cq(struct fdice_eq *eq, struct fdice_cq *cq);
void fdice_eq_del_cq(struct fdice_eq *eq, u32 cqn);

/* IRQ handler */
irqreturn_t fdice_eq_irq_handler(int irq, void *data);

/* IRQ affinity */
int fdice_eq_set_affinity(struct fdice_eq *eq, int cpu);

/* IRQ pool management (for RDMA auxiliary driver) */
int fdice_eq_alloc_irq(struct fdice_irq_pool *pool);
void fdice_eq_release_irq(struct fdice_irq_pool *pool, int irq);

/* EQ table lookup */
struct fdice_eq *fdice_eq_get_comp(struct fdice_eq_table *table, u16 idx);

#endif /* _FDICE_EQ_H_ */
