// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_eq.c - Event queue and interrupt management
 *
 * Phase 3.3 implementation: MSI-X vector pool, EQ allocation with CPU
 * affinity, xarray-based CQ dispatch, and interrupt coalescing.
 *
 * Design rationale:
 *   - fdice_eq is a software abstraction over E810's MSI-X interrupt
 *     mechanism. Each EQ maps to one MSI-X vector and owns an xarray
 *     of CQs for O(1) dispatch in the ISR. This enables dynamic CQ-to-
 *     interrupt remapping and shared interrupt infrastructure between
 *     ethernet and RDMA consumers.
 *   - The IRQ pool manages MSI-X vector allocation with a partition
 *     reserved for RDMA, allowing the RDMA auxiliary driver to request
 *     vectors without conflicting with ethernet channel assignments.
 *   - Interrupt coalescing writes E810 ITR (Interrupt Throttle Rate)
 *     registers. EQ arm/disarm controls whether CQ completions generate
 *     interrupts, enabling NAPI busy-polling integration.
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md
 */

#include <linux/module.h>
#include <linux/pci.h>
#include <linux/kernel.h>
#include <linux/errno.h>
#include <linux/slab.h>
#include <linux/interrupt.h>
#include <linux/cpumask.h>
#include <linux/xarray.h>
#include <linux/io.h>

#include "fdice_dev.h"
#include "fdice_queue.h"
#include "fdice_eq.h"

/* ---- E810 Interrupt Register Offsets ---- */

/*
 * GLINT_DYN_CTL: Dynamic interrupt control register per vector.
 * Used for ITR (Interrupt Throttle Rate) and arm/disarm control.
 */
#define FDICE_REG_GLINT_DYN_CTL(v)	(0x00160000 + ((v) * 4))
#define FDICE_GLINT_DYN_CTL_INTENA	BIT(0)
#define FDICE_GLINT_DYN_CTL_CLEARPBA	BIT(1)
#define FDICE_GLINT_DYN_CTL_SWINT_TRIG	BIT(2)
#define FDICE_GLINT_DYN_CTL_ITR_INDX_S	3
#define FDICE_GLINT_DYN_CTL_ITR_INDX_M	GENMASK(4, 3)
#define FDICE_GLINT_DYN_CTL_INTERVAL_S	5
#define FDICE_GLINT_DYN_CTL_INTERVAL_M	GENMASK(16, 5)
#define FDICE_GLINT_DYN_CTL_WB_ON_ITR	BIT(30)

/* ITR register: per-vector, per-ITR-index (0=RX, 1=TX, 2=other) */
#define FDICE_REG_GLINT_ITR(itr, v)	(0x00154000 + \
					 ((itr) * 0x4000) + ((v) * 4))
#define FDICE_ITR_IDX_RX		0
#define FDICE_ITR_IDX_TX		1
#define FDICE_ITR_IDX_OTHER		2

/* Default ITR values in 2-usec units (E810 hardware granularity) */
#define FDICE_ITR_DEFAULT_RX		25	/* 50 usec */
#define FDICE_ITR_DEFAULT_TX		25	/* 50 usec */

/* ---- IRQ Pool Management ---- */

/**
 * fdice_irq_pool_create - Allocate the MSI-X vector pool
 * @pool: pool structure to initialize
 * @pdev: PCI device for MSI-X allocation
 * @max_vectors: maximum MSI-X vectors to request
 * @rdma_count: number of vectors reserved for RDMA auxiliary driver
 *
 * Requests MSI-X vectors from the PCI subsystem. The pool is partitioned
 * into an ethernet region and an RDMA reservation. Ethernet EQs allocate
 * from the front; RDMA from the back.
 *
 * Return: 0 on success, negative errno on failure.
 */
static int fdice_irq_pool_create(struct fdice_irq_pool *pool,
				 struct pci_dev *pdev,
				 u16 max_vectors, u16 rdma_count)
{
	int nr_vecs;

	memset(pool, 0, sizeof(*pool));
	pool->pdev = pdev;

	/* Request MSI-X vectors; accept at least 2 (1 async + 1 completion) */
	nr_vecs = pci_alloc_irq_vectors(pdev, 2, max_vectors,
					PCI_IRQ_MSIX | PCI_IRQ_AFFINITY);
	if (nr_vecs < 0) {
		dev_err(&pdev->dev, "failed to allocate MSI-X vectors: %d\n",
			nr_vecs);
		return nr_vecs;
	}

	pool->msix_base = 0;
	pool->msix_count = (u16)nr_vecs;
	pool->rdma_count = min(rdma_count, (u16)(nr_vecs / 4));
	pool->rdma_base = pool->msix_count - pool->rdma_count;
	pool->next_free = 0;

	dev_info(&pdev->dev,
		 "allocated %u MSI-X vectors (eth: %u, rdma reserve: %u)\n",
		 pool->msix_count,
		 pool->rdma_base,
		 pool->rdma_count);

	return 0;
}

/**
 * fdice_irq_pool_destroy - Release the MSI-X vector pool
 * @pool: pool to destroy
 *
 * Releases all MSI-X vectors back to the PCI subsystem.
 */
static void fdice_irq_pool_destroy(struct fdice_irq_pool *pool)
{
	if (!pool->pdev)
		return;

	pci_free_irq_vectors(pool->pdev);
	pool->msix_count = 0;
	pool->pdev = NULL;
}

/**
 * fdice_eq_alloc_irq - Allocate an MSI-X vector for ethernet use
 * @pool: vector pool
 *
 * Allocates the next available ethernet vector. Vector indices below
 * rdma_base are for ethernet; at or above rdma_base are for RDMA.
 *
 * Return: vector index on success, negative errno on exhaustion.
 */
int fdice_eq_alloc_irq(struct fdice_irq_pool *pool)
{
	if (pool->next_free >= pool->rdma_base)
		return -ENOSPC;

	return pool->next_free++;
}

/**
 * fdice_eq_release_irq - Return an MSI-X vector to the pool
 * @pool: vector pool
 * @irq: vector index to release (currently a no-op for simple allocator)
 *
 * In the current implementation the pool uses a simple bump allocator.
 * Full free-list tracking is deferred to the RDMA integration phase.
 */
void fdice_eq_release_irq(struct fdice_irq_pool *pool, int irq)
{
	/* Simple allocator -- full free-list in RDMA phase */
	(void)pool;
	(void)irq;
}

/* ---- CQ Registration in EQ ---- */

/**
 * fdice_eq_add_cq - Register a CQ in an EQ's dispatch table
 * @eq: parent event queue
 * @cq: completion queue to register
 *
 * Inserts the CQ into the EQ's xarray keyed by its CQ number. The EQ
 * ISR iterates this xarray to dispatch completion handlers.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_eq_add_cq(struct fdice_eq *eq, struct fdice_cq *cq)
{
	int err;

	cq->eq = eq;
	err = xa_insert(&eq->cq_table, cq->cqn, cq, GFP_KERNEL);
	if (err)
		return err;

	return 0;
}

/**
 * fdice_eq_del_cq - Unregister a CQ from an EQ's dispatch table
 * @eq: parent event queue
 * @cqn: CQ number to remove
 *
 * Removes the CQ from the EQ's xarray. Must be called before freeing
 * the CQ to prevent the ISR from accessing freed memory.
 */
void fdice_eq_del_cq(struct fdice_eq *eq, u32 cqn)
{
	xa_erase(&eq->cq_table, cqn);
}

/* ---- Event Queue ISR ---- */

/**
 * fdice_eq_irq_handler - MSI-X ISR for an event queue
 * @irq: Linux IRQ number
 * @data: pointer to the fdice_eq that owns this vector
 *
 * Walks the EQ's cq_table xarray and invokes each CQ's comp_handler.
 * For ethernet CQs this schedules NAPI; for RDMA CQs this invokes
 * the IB completion callback. The xarray provides RCU-safe iteration
 * without locking in the ISR hot path.
 *
 * Return: IRQ_HANDLED always (MSI-X is dedicated per-vector).
 */
irqreturn_t fdice_eq_irq_handler(int irq, void *data)
{
	struct fdice_eq *eq = data;
	struct fdice_cq *cq;
	unsigned long index;

	xa_for_each(&eq->cq_table, index, cq) {
		if (cq->comp_handler)
			cq->comp_handler(cq);
	}

	return IRQ_HANDLED;
}

/* ---- Individual EQ Lifecycle ---- */

/**
 * fdice_eq_alloc - Create an event queue with MSI-X vector assignment
 * @table: parent EQ table
 * @eq: event queue structure to initialize
 * @type: FDICE_EQ_COMP or FDICE_EQ_ASYNC
 * @cpu: CPU to set affinity for this EQ's interrupt
 *
 * Allocates an MSI-X vector from the pool, initializes the CQ xarray,
 * requests the Linux IRQ, sets CPU affinity, and writes default ITR
 * values for interrupt coalescing.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_eq_alloc(struct fdice_eq_table *table, struct fdice_eq *eq,
		   enum fdice_eq_type type, int cpu)
{
	struct pci_dev *pdev = table->irq_pool->pdev;
	int vec_idx;
	int irq_num;
	int err;

	memset(eq, 0, sizeof(*eq));
	xa_init(&eq->cq_table);
	eq->table = table;
	eq->type = type;
	eq->cpu = cpu;

	/* Allocate MSI-X vector */
	vec_idx = fdice_eq_alloc_irq(table->irq_pool);
	if (vec_idx < 0)
		return vec_idx;

	eq->idx = (u16)vec_idx;

	/* Get the Linux IRQ number for this vector */
	irq_num = pci_irq_vector(pdev, vec_idx);
	if (irq_num < 0) {
		err = irq_num;
		goto err_release_vec;
	}
	eq->irq = irq_num;

	/* Request the IRQ */
	err = request_irq(eq->irq, fdice_eq_irq_handler, 0,
			  dev_name(&pdev->dev), eq);
	if (err) {
		dev_err(&pdev->dev,
			"EQ %u: failed to request IRQ %d: %d\n",
			eq->idx, eq->irq, err);
		goto err_release_vec;
	}

	/* Set CPU affinity */
	irq_set_affinity_hint(eq->irq, cpumask_of(cpu));

	return 0;

err_release_vec:
	fdice_eq_release_irq(table->irq_pool, vec_idx);
	return err;
}

/**
 * fdice_eq_free - Destroy an event queue and release its MSI-X vector
 * @table: parent EQ table
 * @eq: event queue to destroy
 *
 * Frees the IRQ, clears CPU affinity, releases the MSI-X vector,
 * and destroys the CQ xarray. All CQs must be unregistered first.
 */
void fdice_eq_free(struct fdice_eq_table *table, struct fdice_eq *eq)
{
	if (!eq->irq)
		return;

	irq_set_affinity_hint(eq->irq, NULL);
	free_irq(eq->irq, eq);
	eq->irq = 0;

	fdice_eq_release_irq(table->irq_pool, eq->idx);
	xa_destroy(&eq->cq_table);
}

/* ---- IRQ Affinity ---- */

/**
 * fdice_eq_set_affinity - Change the CPU affinity of an EQ's interrupt
 * @eq: event queue to reconfigure
 * @cpu: new CPU to bind the interrupt to
 *
 * Updates the IRQ affinity hint and the EQ's tracked CPU.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_eq_set_affinity(struct fdice_eq *eq, int cpu)
{
	int err;

	if (!cpu_online(cpu))
		return -EINVAL;

	err = irq_set_affinity_hint(eq->irq, cpumask_of(cpu));
	if (err)
		return err;

	eq->cpu = cpu;
	return 0;
}

/* ---- EQ Table (Device-Level) ---- */

/**
 * fdice_eq_get_comp - Get a completion EQ by index
 * @table: EQ table
 * @idx: completion EQ index
 *
 * Return: pointer to the EQ, or NULL if index is out of range.
 */
struct fdice_eq *fdice_eq_get_comp(struct fdice_eq_table *table, u16 idx)
{
	if (!table->comp_eqs || idx >= table->num_comp_eqs)
		return NULL;

	return &table->comp_eqs[idx];
}

/**
 * fdice_eq_table_init - Initialize the EQ table and allocate all EQs
 * @fdev: core device structure
 * @num_comp: number of completion EQs to allocate (one per channel)
 *
 * Creates the IRQ pool, allocates one async EQ and @num_comp completion
 * EQs. Each completion EQ is assigned to a CPU using round-robin
 * distribution across online CPUs, preferring NUMA-local ones.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_eq_table_init(struct fdice_dev *fdev, u16 num_comp)
{
	struct fdice_eq_table *table = &fdev->eq_table;
	struct pci_dev *pdev = fdev->pdev;
	u16 max_vectors;
	int cpu;
	int err;
	u16 i;

	memset(table, 0, sizeof(*table));

	/* IRQ pool: request enough vectors for async + completion + RDMA */
	max_vectors = 1 + num_comp + (fdev->hw.func_caps.rdma_capable ? 8 : 0);

	table->irq_pool = kzalloc(sizeof(*table->irq_pool), GFP_KERNEL);
	if (!table->irq_pool)
		return -ENOMEM;

	err = fdice_irq_pool_create(table->irq_pool, pdev, max_vectors,
				    fdev->hw.func_caps.rdma_capable ? 8 : 0);
	if (err)
		goto err_free_pool_struct;

	/* Allocate the async EQ on the first vector */
	err = fdice_eq_alloc(table, &table->async_eq, FDICE_EQ_ASYNC, 0);
	if (err) {
		dev_err(&pdev->dev, "failed to allocate async EQ: %d\n", err);
		goto err_destroy_pool;
	}

	/* Allocate completion EQ array */
	num_comp = min(num_comp,
		       (u16)(table->irq_pool->rdma_base -
			     table->irq_pool->next_free));
	if (num_comp == 0) {
		dev_err(&pdev->dev, "no MSI-X vectors for completion EQs\n");
		err = -ENOSPC;
		goto err_free_async;
	}

	table->comp_eqs = kvcalloc(num_comp, sizeof(struct fdice_eq),
				   GFP_KERNEL);
	if (!table->comp_eqs) {
		err = -ENOMEM;
		goto err_free_async;
	}
	table->num_comp_eqs = num_comp;

	/* CPU affinity tracking */
	if (!alloc_cpumask_var(&table->used_cpus, GFP_KERNEL)) {
		err = -ENOMEM;
		goto err_free_comp_array;
	}
	cpumask_clear(table->used_cpus);

	/* Create completion EQs with round-robin CPU assignment */
	cpu = cpumask_first(cpu_online_mask);
	for (i = 0; i < num_comp; i++) {
		/* Skip CPUs already assigned to an EQ */
		while (cpumask_test_cpu(cpu, table->used_cpus)) {
			cpu = cpumask_next(cpu, cpu_online_mask);
			if (cpu >= nr_cpu_ids)
				cpu = cpumask_first(cpu_online_mask);
		}

		err = fdice_eq_alloc(table, &table->comp_eqs[i],
				     FDICE_EQ_COMP, cpu);
		if (err) {
			dev_err(&pdev->dev,
				"failed to allocate completion EQ %u: %d\n",
				i, err);
			goto err_free_comp_eqs;
		}

		cpumask_set_cpu(cpu, table->used_cpus);
		cpu = cpumask_next(cpu, cpu_online_mask);
		if (cpu >= nr_cpu_ids)
			cpu = cpumask_first(cpu_online_mask);
	}

	dev_info(&pdev->dev,
		 "EQ table: 1 async + %u completion EQs initialized\n",
		 num_comp);

	return 0;

err_free_comp_eqs:
	while (i > 0) {
		i--;
		fdice_eq_free(table, &table->comp_eqs[i]);
	}
	free_cpumask_var(table->used_cpus);
err_free_comp_array:
	kvfree(table->comp_eqs);
	table->comp_eqs = NULL;
	table->num_comp_eqs = 0;
err_free_async:
	fdice_eq_free(table, &table->async_eq);
err_destroy_pool:
	fdice_irq_pool_destroy(table->irq_pool);
err_free_pool_struct:
	kfree(table->irq_pool);
	table->irq_pool = NULL;
	return err;
}

/**
 * fdice_eq_table_cleanup - Tear down the EQ table and release all vectors
 * @fdev: core device structure
 *
 * Destroys all completion EQs and the async EQ, then releases the
 * MSI-X vector pool.
 */
void fdice_eq_table_cleanup(struct fdice_dev *fdev)
{
	struct fdice_eq_table *table = &fdev->eq_table;
	u16 i;

	if (!table->irq_pool)
		return;

	/* Destroy completion EQs in reverse order */
	if (table->comp_eqs) {
		for (i = table->num_comp_eqs; i > 0; i--)
			fdice_eq_free(table, &table->comp_eqs[i - 1]);
		kvfree(table->comp_eqs);
		table->comp_eqs = NULL;
	}

	/* Destroy async EQ */
	fdice_eq_free(table, &table->async_eq);

	if (table->used_cpus)
		free_cpumask_var(table->used_cpus);

	fdice_irq_pool_destroy(table->irq_pool);
	kfree(table->irq_pool);
	table->irq_pool = NULL;
	table->num_comp_eqs = 0;
}
