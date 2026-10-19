// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * fdice_aq.c - Admin Queue implementation for FrankenDrICE
 *
 * Implements the E810 control queue ring protocol with a workqueue-based
 * completion model and token-tracked concurrent command submission.
 *
 * The descriptor format and ring protocol match E810 firmware exactly.
 * The completion model (workqueue-based with token tracking) is the key
 * architectural divergence from ice's single-mutex busy-wait approach,
 * inspired by mlx5's concurrent command slot design.
 *
 * Architecture reference: docs/architecture/01-hal-design.md
 */

#include <linux/dma-mapping.h>
#include <linux/idr.h>
#include <linux/workqueue.h>
#include <linux/completion.h>
#include <linux/slab.h>
#include <linux/delay.h>
#include <linux/io.h>
#include <linux/pci.h>
#include <linux/xarray.h>
#include <linux/jiffies.h>
#include <linux/atomic.h>
#include <linux/log2.h>
#include <linux/bitfield.h>
#include <linux/spinlock.h>
#include <linux/mutex.h>

#include "fdice_dev.h"

/*
 * E810 Admin Queue register offsets — hardware constants from datasheet.
 * Each control queue type (admin, mailbox, sideband) has its own set.
 */

/* Admin Queue (ATQ/ARQ) — primary firmware command interface */
#define FDICE_PF_FW_ATQBAL		0x00080000
#define FDICE_PF_FW_ATQBAH		0x00080100
#define FDICE_PF_FW_ATQLEN		0x00080200
#define FDICE_PF_FW_ATQH		0x00080300
#define FDICE_PF_FW_ATQT		0x00080400

#define FDICE_PF_FW_ARQBAL		0x00080080
#define FDICE_PF_FW_ARQBAH		0x00080180
#define FDICE_PF_FW_ARQLEN		0x00080280
#define FDICE_PF_FW_ARQH		0x00080380
#define FDICE_PF_FW_ARQT		0x00080480

/* Mailbox Queue (MBX ATQ/ARQ) — PF-VF communication */
#define FDICE_PF_MBX_ATQBAL		0x00022200
#define FDICE_PF_MBX_ATQBAH		0x00022300
#define FDICE_PF_MBX_ATQLEN		0x00022400
#define FDICE_PF_MBX_ATQH		0x00022500
#define FDICE_PF_MBX_ATQT		0x00022600

#define FDICE_PF_MBX_ARQBAL		0x00022280
#define FDICE_PF_MBX_ARQBAH		0x00022380
#define FDICE_PF_MBX_ARQLEN		0x00022480
#define FDICE_PF_MBX_ARQH		0x00022580
#define FDICE_PF_MBX_ARQT		0x00022680

/* Sideband Queue (SB ATQ/ARQ) — sideband control channel */
#define FDICE_PF_SB_ATQBAL		0x00210000
#define FDICE_PF_SB_ATQBAH		0x00210100
#define FDICE_PF_SB_ATQLEN		0x00210200
#define FDICE_PF_SB_ATQH		0x00210300
#define FDICE_PF_SB_ATQT		0x00210400

#define FDICE_PF_SB_ARQBAL		0x00210080
#define FDICE_PF_SB_ARQBAH		0x00210180
#define FDICE_PF_SB_ARQLEN		0x00210280
#define FDICE_PF_SB_ARQH		0x00210380
#define FDICE_PF_SB_ARQT		0x00210480

/* Length register enable and critical error bits */
#define FDICE_AQ_LEN_ENABLE		BIT(31)
#define FDICE_AQ_LEN_CRIT_ERR		BIT(30)

/* Default queue parameters per queue type */
#define FDICE_AQ_ADMIN_SQ_ENTRIES	32
#define FDICE_AQ_ADMIN_RQ_ENTRIES	32
#define FDICE_AQ_ADMIN_MAX_BUF		4096
#define FDICE_AQ_ADMIN_MAX_PENDING	16

#define FDICE_AQ_MBX_SQ_ENTRIES		32
#define FDICE_AQ_MBX_RQ_ENTRIES		32
#define FDICE_AQ_MBX_MAX_BUF		4096
#define FDICE_AQ_MBX_MAX_PENDING	8

#define FDICE_AQ_SB_SQ_ENTRIES		16
#define FDICE_AQ_SB_RQ_ENTRIES		16
#define FDICE_AQ_SB_MAX_BUF		512
#define FDICE_AQ_SB_MAX_PENDING		4

/* Timeout for synchronous commands: 1 second */
#define FDICE_AQ_CMD_TIMEOUT		msecs_to_jiffies(1000)

/* Poll interval when draining queue during shutdown */
#define FDICE_AQ_DRAIN_POLL_US		100
#define FDICE_AQ_DRAIN_MAX_POLLS	10000

/* Descriptor index within a ring */
#define FDICE_AQ_DESC(ring, idx)	(&(ring)->desc[(idx)])

/* Count unused descriptors in ring */
#define FDICE_AQ_RING_UNUSED(ring) \
	(((ring)->next_to_clean > (ring)->next_to_use) ? \
	 (ring)->next_to_clean - (ring)->next_to_use - 1 : \
	 (ring)->count + (ring)->next_to_clean - (ring)->next_to_use - 1)

/* ---- Register access helpers ------------------------------------------ */

/**
 * fdice_aq_rd32 - Read a 32-bit register via MMIO
 * @hw: hardware instance
 * @reg: register offset
 *
 * Return: register value
 */
static inline u32 fdice_aq_rd32(struct fdice_hw *hw, u32 reg)
{
	return readl(hw->hw_addr + reg);
}

/**
 * fdice_aq_wr32 - Write a 32-bit register via MMIO
 * @hw: hardware instance
 * @reg: register offset
 * @val: value to write
 */
static inline void fdice_aq_wr32(struct fdice_hw *hw, u32 reg, u32 val)
{
	writel(val, hw->hw_addr + reg);
}

/**
 * fdice_aq_flush - Force posted writes to complete
 * @hw: hardware instance
 *
 * Reads a harmless register to flush the PCI write-posting buffer.
 */
static inline void fdice_aq_flush(struct fdice_hw *hw)
{
	fdice_aq_rd32(hw, FDICE_PF_FW_ATQBAL);
}

/* ---- Per-device helper ------------------------------------------------ */

/**
 * fdice_hw_to_dev - Get struct device from hardware instance
 * @hw: hardware instance
 *
 * Return: pointer to the PCI device's struct device
 */
static inline struct device *fdice_hw_to_dev(struct fdice_hw *hw)
{
	return &hw->pdev->dev;
}

/* ---- Error translation ------------------------------------------------ */

/**
 * fdice_aq_retval_to_err - Translate firmware AQ retval to fdice_err
 * @retval: raw 16-bit return value from the completion descriptor
 *
 * Maps E810 firmware error codes to the fdice internal error enum.
 * Unknown codes are mapped to FDICE_ERR_IO as a conservative default.
 *
 * Return: corresponding fdice_err value
 */
int fdice_aq_retval_to_err(u16 retval)
{
	/* Strip off any firmware-internal high bits */
	u8 code = retval & 0xFF;

	switch (code) {
	case 0:  return FDICE_ERR_OK;
	case 1:  return FDICE_ERR_PERM;
	case 2:  return FDICE_ERR_NOENT;
	case 3:  return FDICE_ERR_BAD_OPCODE;
	case 5:  return FDICE_ERR_IO;
	case 8:  return FDICE_ERR_AGAIN;
	case 9:  return FDICE_ERR_NOMEM;
	case 10: return FDICE_ERR_ACCESS;
	case 12: return FDICE_ERR_BUSY;
	case 13: return FDICE_ERR_EXIST;
	case 14: return FDICE_ERR_INVAL;
	case 16: return FDICE_ERR_NOSPC;
	case 17: return FDICE_ERR_NOSYS;
	case 21: return FDICE_ERR_MODE;
	case 24: return FDICE_ERR_NOSEC;
	case 25: return FDICE_ERR_BADSIG;
	case 26: return FDICE_ERR_SVN;
	case 27: return FDICE_ERR_BADMAN;
	case 28: return FDICE_ERR_BADBUF;
	default: return FDICE_ERR_IO;
	}
}

/**
 * fdice_err_to_errno - Translate fdice_err to standard kernel errno
 * @err: fdice internal error code
 *
 * Used at the boundary between HAL consumers and kernel subsystems
 * (netdev, devlink, ethtool) that expect standard errno values.
 *
 * Return: negative errno value, or 0 on success
 */
int fdice_err_to_errno(int err)
{
	switch (err) {
	case FDICE_ERR_OK:         return 0;
	case FDICE_ERR_PERM:       return -EPERM;
	case FDICE_ERR_NOENT:      return -ENOENT;
	case FDICE_ERR_BAD_OPCODE: return -EOPNOTSUPP;
	case FDICE_ERR_IO:         return -EIO;
	case FDICE_ERR_AGAIN:      return -EAGAIN;
	case FDICE_ERR_NOMEM:      return -ENOMEM;
	case FDICE_ERR_ACCESS:     return -EACCES;
	case FDICE_ERR_BUSY:       return -EBUSY;
	case FDICE_ERR_EXIST:      return -EEXIST;
	case FDICE_ERR_INVAL:      return -EINVAL;
	case FDICE_ERR_NOSPC:      return -ENOSPC;
	case FDICE_ERR_NOSYS:      return -EOPNOTSUPP;
	case FDICE_ERR_MODE:       return -EPERM;
	case FDICE_ERR_NOSEC:      return -EACCES;
	case FDICE_ERR_BADSIG:     return -EACCES;
	case FDICE_ERR_SVN:        return -EPERM;
	case FDICE_ERR_BADMAN:     return -EINVAL;
	case FDICE_ERR_BADBUF:     return -EINVAL;
	case FDICE_ERR_TIMEOUT:    return -ETIMEDOUT;
	case FDICE_ERR_RESET:      return -ECONNRESET;
	case FDICE_ERR_QUEUE_FULL: return -ENOSPC;
	case FDICE_ERR_QUEUE_SHUT: return -ESHUTDOWN;
	default:                   return -EIO;
	}
}

/* ---- Statistics ------------------------------------------------------- */

/**
 * fdice_aq_stats_get - Get or create per-opcode statistics entry
 * @aq: admin queue instance
 * @opcode: AQ command opcode
 *
 * Looks up the statistics entry for @opcode in the xarray, allocating
 * a new zero-initialized entry if one does not yet exist.
 *
 * Return: pointer to stats entry, or NULL on allocation failure
 */
static struct fdice_aq_cmd_stats *
fdice_aq_stats_get(struct fdice_aq *aq, u16 opcode)
{
	struct fdice_aq_cmd_stats *stats;

	stats = xa_load(&aq->stats, opcode);
	if (stats)
		return stats;

	stats = kzalloc(sizeof(*stats), GFP_ATOMIC);
	if (!stats)
		return NULL;

	if (xa_err(xa_store(&aq->stats, opcode, stats, GFP_ATOMIC))) {
		kfree(stats);
		return NULL;
	}

	return stats;
}

/**
 * fdice_aq_stats_record - Update statistics after command completion
 * @aq: admin queue instance
 * @opcode: AQ command opcode
 * @elapsed_us: execution time in microseconds
 * @err: completion error code (0 for success)
 */
static void fdice_aq_stats_record(struct fdice_aq *aq, u16 opcode,
				  u64 elapsed_us, int err)
{
	struct fdice_aq_cmd_stats *stats;

	stats = fdice_aq_stats_get(aq, opcode);
	if (!stats)
		return;

	atomic64_inc(&stats->count);
	atomic64_add(elapsed_us, &stats->total_usec);

	/*
	 * Racy max update is acceptable for diagnostics — the worst case
	 * is a slightly stale maximum, which is fine for profiling.
	 */
	if (elapsed_us > (u64)atomic64_read(&stats->max_usec))
		atomic64_set(&stats->max_usec, elapsed_us);

	if (err) {
		atomic64_inc(&stats->error);
		stats->last_err = (u16)err;
	} else {
		atomic64_inc(&stats->success);
	}
}

/**
 * fdice_aq_stats_record_timeout - Record a timeout in statistics
 * @aq: admin queue instance
 * @opcode: AQ command opcode
 */
static void fdice_aq_stats_record_timeout(struct fdice_aq *aq, u16 opcode)
{
	struct fdice_aq_cmd_stats *stats;

	stats = fdice_aq_stats_get(aq, opcode);
	if (!stats)
		return;

	atomic64_inc(&stats->timeout);
}

/**
 * fdice_aq_stats_destroy - Free all per-opcode statistics entries
 * @aq: admin queue instance
 */
static void fdice_aq_stats_destroy(struct fdice_aq *aq)
{
	struct fdice_aq_cmd_stats *stats;
	unsigned long opcode;

	xa_for_each(&aq->stats, opcode, stats) {
		xa_erase(&aq->stats, opcode);
		kfree(stats);
	}
	xa_destroy(&aq->stats);
}

/* ---- Descriptor helpers ----------------------------------------------- */

/**
 * fdice_aq_prep_desc - Zero a descriptor and set its opcode
 * @desc: descriptor to prepare
 * @opcode: AQ command opcode to set
 *
 * Zeroes the entire 32-byte descriptor and writes the opcode field.
 * Caller is responsible for filling command-specific parameters and
 * flags after this call.
 */
void fdice_aq_prep_desc(struct fdice_aq_desc *desc, u16 opcode)
{
	memset(desc, 0, sizeof(*desc));
	desc->opcode = cpu_to_le16(opcode);
}

/* ---- Ring allocation -------------------------------------------------- */

/**
 * fdice_aq_alloc_ring - Allocate DMA-coherent descriptor ring
 * @hw: hardware instance
 * @ring: ring structure to populate
 * @count: number of descriptors (must be power of 2)
 *
 * Allocates a devres-managed DMA-coherent array of descriptors.
 * The ring is zeroed on allocation. Uses dmam_alloc_coherent so
 * the ring is automatically freed on device removal.
 *
 * Return: 0 on success, -ENOMEM on allocation failure
 */
static int fdice_aq_alloc_ring(struct fdice_hw *hw,
			       struct fdice_aq_ring *ring,
			       u16 count)
{
	struct device *dev = fdice_hw_to_dev(hw);
	size_t ring_size;

	if (!is_power_of_2(count)) {
		dev_err(dev, "AQ ring count %u is not a power of 2\n", count);
		return -EINVAL;
	}

	ring_size = (size_t)count * sizeof(struct fdice_aq_desc);

	ring->desc = dmam_alloc_coherent(dev, ring_size,
					 &ring->desc_dma, GFP_KERNEL);
	if (!ring->desc) {
		dev_err(dev, "AQ ring alloc failed (count=%u, size=%zu)\n",
			count, ring_size);
		return -ENOMEM;
	}

	ring->count = count;
	ring->next_to_use = 0;
	ring->next_to_clean = 0;

	return 0;
}

/**
 * fdice_aq_alloc_rq_bufs - Allocate per-descriptor DMA buffers for RQ
 * @hw: hardware instance
 * @ring: receive queue ring
 * @buf_size: maximum buffer size per descriptor
 *
 * Pre-allocates a DMA buffer for each receive queue descriptor slot.
 * The firmware writes incoming async events into these buffers. Uses
 * devres-managed allocation for automatic cleanup.
 *
 * Also writes the buffer DMA addresses into the corresponding RQ
 * descriptors so the firmware knows where to DMA event data.
 *
 * Return: 0 on success, -ENOMEM on allocation failure
 */
static int fdice_aq_alloc_rq_bufs(struct fdice_hw *hw,
				   struct fdice_aq_ring *ring,
				   u16 buf_size)
{
	struct device *dev = fdice_hw_to_dev(hw);
	struct fdice_aq_desc *desc;
	int i;

	ring->bufs = devm_kcalloc(dev, ring->count,
				  sizeof(struct fdice_aq_buf), GFP_KERNEL);
	if (!ring->bufs)
		return -ENOMEM;

	for (i = 0; i < ring->count; i++) {
		ring->bufs[i].va = dmam_alloc_coherent(dev, buf_size,
						       &ring->bufs[i].dma,
						       GFP_KERNEL);
		if (!ring->bufs[i].va) {
			dev_err(dev, "AQ RQ buf[%d] alloc failed\n", i);
			/*
			 * devres handles cleanup of earlier successful
			 * allocations on probe failure / device removal.
			 */
			return -ENOMEM;
		}
		ring->bufs[i].len = buf_size;

		/*
		 * Populate the RQ descriptor with the buffer address so
		 * firmware can DMA event data directly into our buffers.
		 * Set the BUF flag to indicate an indirect data buffer
		 * is attached.
		 */
		desc = FDICE_AQ_DESC(ring, i);
		desc->flags = cpu_to_le16(FDICE_AQ_FLAG_BUF);
		desc->datalen = cpu_to_le16(buf_size);
		desc->params.generic.addr_high =
			cpu_to_le32(upper_32_bits(ring->bufs[i].dma));
		desc->params.generic.addr_low =
			cpu_to_le32(lower_32_bits(ring->bufs[i].dma));
	}

	return 0;
}

/* ---- Register programming --------------------------------------------- */

/**
 * fdice_aq_cfg_ring_regs - Program ring base address and enable queue
 * @hw: hardware instance
 * @ring: ring whose registers to program
 *
 * Programs a control queue ring following the E810 firmware's required
 * register write sequence: clear HEAD and TAIL first, then write LEN
 * with enable bit, then BAL and BAH. This ordering is critical —
 * firmware latches the enable on the LEN write and expects HEAD/TAIL
 * to be zeroed before the ring goes live.
 *
 * Return: 0 on success, -EIO if readback verification fails
 */
static int fdice_aq_cfg_ring_regs(struct fdice_hw *hw,
				  struct fdice_aq_ring *ring)
{
	u32 len_val;

	/* Step 1: Clear head and tail to reset ring position */
	fdice_aq_wr32(hw, ring->reg_head, 0);
	fdice_aq_wr32(hw, ring->reg_tail, 0);

	/* Step 2: Write length with enable bit — firmware latches here */
	len_val = (u32)ring->count | FDICE_AQ_LEN_ENABLE;
	fdice_aq_wr32(hw, ring->reg_len, len_val);

	/* Step 3: Write base address low and high */
	fdice_aq_wr32(hw, ring->reg_bal,
		      lower_32_bits(ring->desc_dma));
	fdice_aq_wr32(hw, ring->reg_bah,
		      upper_32_bits(ring->desc_dma));

	/* Flush posted writes */
	fdice_aq_flush(hw);

	/* Readback verification — confirm the BAL write landed */
	if (fdice_aq_rd32(hw, ring->reg_bal) !=
	    lower_32_bits(ring->desc_dma)) {
		dev_err(fdice_hw_to_dev(hw),
			"AQ ring BAL readback mismatch (reg=0x%x)\n",
			ring->reg_bal);
		return -EIO;
	}

	return 0;
}

/**
 * fdice_aq_disable_ring_regs - Disable a ring and clear its registers
 * @hw: hardware instance
 * @ring: ring to disable
 *
 * Clears the LEN register (removing the enable bit) then zeroes all
 * ring registers. This halts firmware access to the descriptor ring.
 */
static void fdice_aq_disable_ring_regs(struct fdice_hw *hw,
				       struct fdice_aq_ring *ring)
{
	fdice_aq_wr32(hw, ring->reg_head, 0);
	fdice_aq_wr32(hw, ring->reg_tail, 0);
	fdice_aq_wr32(hw, ring->reg_len, 0);
	fdice_aq_wr32(hw, ring->reg_bal, 0);
	fdice_aq_wr32(hw, ring->reg_bah, 0);
	fdice_aq_flush(hw);
}

/* ---- Register offset assignment --------------------------------------- */

/**
 * fdice_aq_set_admin_regs - Assign register offsets for admin queue
 * @aq: admin queue instance
 *
 * Sets the MMIO register offsets for the admin queue's send and
 * receive rings. These offsets are E810 datasheet constants.
 */
static void fdice_aq_set_admin_regs(struct fdice_aq *aq)
{
	aq->sq.reg_bal  = FDICE_PF_FW_ATQBAL;
	aq->sq.reg_bah  = FDICE_PF_FW_ATQBAH;
	aq->sq.reg_len  = FDICE_PF_FW_ATQLEN;
	aq->sq.reg_head = FDICE_PF_FW_ATQH;
	aq->sq.reg_tail = FDICE_PF_FW_ATQT;

	aq->rq.reg_bal  = FDICE_PF_FW_ARQBAL;
	aq->rq.reg_bah  = FDICE_PF_FW_ARQBAH;
	aq->rq.reg_len  = FDICE_PF_FW_ARQLEN;
	aq->rq.reg_head = FDICE_PF_FW_ARQH;
	aq->rq.reg_tail = FDICE_PF_FW_ARQT;
}

/**
 * fdice_aq_set_mbx_regs - Assign register offsets for mailbox queue
 * @aq: admin queue instance
 */
static void fdice_aq_set_mbx_regs(struct fdice_aq *aq)
{
	aq->sq.reg_bal  = FDICE_PF_MBX_ATQBAL;
	aq->sq.reg_bah  = FDICE_PF_MBX_ATQBAH;
	aq->sq.reg_len  = FDICE_PF_MBX_ATQLEN;
	aq->sq.reg_head = FDICE_PF_MBX_ATQH;
	aq->sq.reg_tail = FDICE_PF_MBX_ATQT;

	aq->rq.reg_bal  = FDICE_PF_MBX_ARQBAL;
	aq->rq.reg_bah  = FDICE_PF_MBX_ARQBAH;
	aq->rq.reg_len  = FDICE_PF_MBX_ARQLEN;
	aq->rq.reg_head = FDICE_PF_MBX_ARQH;
	aq->rq.reg_tail = FDICE_PF_MBX_ARQT;
}

/**
 * fdice_aq_set_sb_regs - Assign register offsets for sideband queue
 * @aq: admin queue instance
 */
static void fdice_aq_set_sb_regs(struct fdice_aq *aq)
{
	aq->sq.reg_bal  = FDICE_PF_SB_ATQBAL;
	aq->sq.reg_bah  = FDICE_PF_SB_ATQBAH;
	aq->sq.reg_len  = FDICE_PF_SB_ATQLEN;
	aq->sq.reg_head = FDICE_PF_SB_ATQH;
	aq->sq.reg_tail = FDICE_PF_SB_ATQT;

	aq->rq.reg_bal  = FDICE_PF_SB_ARQBAL;
	aq->rq.reg_bah  = FDICE_PF_SB_ARQBAH;
	aq->rq.reg_len  = FDICE_PF_SB_ARQLEN;
	aq->rq.reg_head = FDICE_PF_SB_ARQH;
	aq->rq.reg_tail = FDICE_PF_SB_ARQT;
}

/* ---- Completion work -------------------------------------------------- */

/**
 * fdice_aq_complete_pending - Complete a pending command slot
 * @hw: hardware instance
 * @aq: admin queue instance
 * @slot: pending command slot
 * @resp: writeback descriptor from firmware
 * @status: translated error status
 *
 * Copies the firmware's writeback descriptor into the pending slot,
 * records statistics, and either invokes the async callback or
 * signals the synchronous completion waiter.
 */
static void fdice_aq_complete_pending(struct fdice_hw *hw,
				      struct fdice_aq *aq,
				      struct fdice_aq_pending *slot,
				      struct fdice_aq_desc *resp,
				      int status)
{
	u64 elapsed_us;
	u16 opcode;

	opcode = le16_to_cpu(slot->desc.opcode);
	elapsed_us = jiffies_to_usecs(jiffies - slot->submit_jiffies);

	/* Copy the response descriptor so callers can inspect it */
	memcpy(&slot->desc, resp, sizeof(*resp));
	slot->status = status;

	fdice_aq_stats_record(aq, opcode, elapsed_us, status);

	if (slot->cb && slot->cb->fn) {
		/* Async path: invoke callback from workqueue context */
		slot->cb->fn(hw, &slot->desc, slot->buf,
			     status, slot->cb->ctx);
		ida_free(&aq->tokens, slot->token);
		atomic_dec(&aq->pending_count);
	} else {
		/* Sync path: wake the blocked submitter */
		complete(&slot->completion);
	}
}

/**
 * fdice_aq_process_completions - Scan SQ for completed descriptors
 * @hw: hardware instance
 * @aq: admin queue instance
 *
 * Scans the send queue from next_to_clean up to the hardware head
 * register. For each descriptor with the DD (Descriptor Done) flag
 * set, extracts the completion token, looks up the pending slot,
 * translates the firmware retval, and completes the command.
 *
 * Called from the completion workqueue or the polling path.
 *
 * Return: number of completions processed
 */
int fdice_aq_process_completions(struct fdice_hw *hw, struct fdice_aq *aq)
{
	struct fdice_aq_ring *sq = &aq->sq;
	struct fdice_aq_desc *desc;
	struct fdice_aq_pending *slot;
	u32 head;
	u16 ntc, token, retval;
	int completed = 0;
	int status;
	unsigned long flags;

	spin_lock_irqsave(&aq->sq_lock, flags);

	if (!aq->initialized) {
		spin_unlock_irqrestore(&aq->sq_lock, flags);
		return 0;
	}

	head = fdice_aq_rd32(hw, sq->reg_head);
	ntc = sq->next_to_clean;

	while (ntc != head) {
		desc = FDICE_AQ_DESC(sq, ntc);

		/* Verify firmware marked this descriptor as done */
		if (!(le16_to_cpu(desc->flags) & FDICE_AQ_FLAG_DD))
			break;

		/* Extract token from cookie fields */
		token = (u16)le32_to_cpu(desc->cookie_low);

		if (token >= aq->max_pending) {
			dev_warn(fdice_hw_to_dev(hw),
				 "AQ completion with invalid token %u\n",
				 token);
			goto advance;
		}

		slot = &aq->pending[token];

		/* Translate firmware retval to fdice error */
		retval = le16_to_cpu(desc->retval);
		if (le16_to_cpu(desc->flags) & FDICE_AQ_FLAG_ERR)
			status = fdice_aq_retval_to_err(retval);
		else
			status = FDICE_ERR_OK;

		/*
		 * If an indirect buffer was attached, copy response data
		 * back from the SQ DMA buffer to the caller's buffer.
		 */
		if (slot->buf && slot->buf_len &&
		    (le16_to_cpu(desc->flags) & FDICE_AQ_FLAG_BUF)) {
			u16 copy_len = min_t(u16,
					     le16_to_cpu(desc->datalen),
					     slot->buf_len);
			struct fdice_aq_buf *dma_buf =
				&sq->bufs[ntc];

			if (dma_buf && dma_buf->va)
				memcpy(slot->buf, dma_buf->va, copy_len);
		}

		fdice_aq_complete_pending(hw, aq, slot, desc, status);
		completed++;

advance:
		/* Clear the descriptor for reuse */
		memset(desc, 0, sizeof(*desc));

		ntc++;
		if (ntc == sq->count)
			ntc = 0;
	}

	sq->next_to_clean = ntc;

	spin_unlock_irqrestore(&aq->sq_lock, flags);

	return completed;
}

/**
 * fdice_aq_completion_work - Workqueue handler for completion processing
 * @work: work structure embedded in workqueue data
 *
 * This is the workqueue callback scheduled by the MSI-X interrupt
 * handler or periodic polling timer. It processes all available
 * completions on the admin queue.
 */
static void fdice_aq_completion_work(struct work_struct *work)
{
	struct fdice_aq_work *aw = container_of(work, struct fdice_aq_work,
						work);

	fdice_aq_process_completions(aw->hw, aw->aq);
}

/* ---- Event processing ------------------------------------------------- */

/**
 * fdice_aq_process_events - Scan RQ for received async events
 * @hw: hardware instance
 * @aq: admin queue instance
 * @handler: callback to invoke for each received event
 * @ctx: opaque context passed to handler
 *
 * The receive queue carries unsolicited events from firmware (link
 * state changes, error notifications, etc). This function scans for
 * descriptors with DD set, invokes the handler for each, then
 * repopulates the descriptor and bumps the tail to return the slot
 * to firmware.
 *
 * Return: number of events processed
 */
int fdice_aq_process_events(struct fdice_hw *hw, struct fdice_aq *aq,
			    fdice_aq_event_handler_t handler, void *ctx)
{
	struct fdice_aq_ring *rq = &aq->rq;
	struct fdice_aq_desc *desc;
	void *buf;
	u16 datalen;
	u16 ntu;
	int processed = 0;
	unsigned long flags;

	spin_lock_irqsave(&aq->rq_lock, flags);

	if (!aq->initialized) {
		spin_unlock_irqrestore(&aq->rq_lock, flags);
		return 0;
	}

	ntu = rq->next_to_use;

	while (1) {
		desc = FDICE_AQ_DESC(rq, ntu);

		/* Check if firmware has written an event into this slot */
		if (!(le16_to_cpu(desc->flags) & FDICE_AQ_FLAG_DD))
			break;

		datalen = le16_to_cpu(desc->datalen);
		buf = (rq->bufs && rq->bufs[ntu].va) ?
		      rq->bufs[ntu].va : NULL;

		/* Invoke the registered event handler */
		if (handler)
			handler(hw, desc, buf, datalen, ctx);

		processed++;

		/*
		 * Repopulate the descriptor for firmware reuse:
		 * restore the buffer address and flags, clear the
		 * writeback fields.
		 */
		desc->flags = cpu_to_le16(FDICE_AQ_FLAG_BUF);
		desc->opcode = 0;
		desc->datalen = cpu_to_le16(rq->bufs[ntu].len);
		desc->retval = 0;
		desc->cookie_high = 0;
		desc->cookie_low = 0;
		memset(&desc->params, 0, sizeof(desc->params));
		desc->params.generic.addr_high =
			cpu_to_le32(upper_32_bits(rq->bufs[ntu].dma));
		desc->params.generic.addr_low =
			cpu_to_le32(lower_32_bits(rq->bufs[ntu].dma));

		ntu++;
		if (ntu == rq->count)
			ntu = 0;
	}

	if (processed) {
		/*
		 * Bump the RQ tail register to tell firmware we have
		 * replenished descriptors. Write (ntu - 1) wrapped,
		 * since tail points to the last valid descriptor.
		 */
		u16 tail = (ntu == 0) ? (rq->count - 1) : (ntu - 1);

		rq->next_to_use = ntu;
		fdice_aq_wr32(hw, rq->reg_tail, tail);
	}

	spin_unlock_irqrestore(&aq->rq_lock, flags);

	return processed;
}

/* ---- Command submission ----------------------------------------------- */

/**
 * fdice_aq_alloc_token - Allocate a token for command tracking
 * @aq: admin queue instance
 *
 * Allocates a unique token from the IDA, bounded by max_pending.
 * The token is used as an index into the pending array and stored
 * in the descriptor's cookie fields for firmware echo-back.
 *
 * Return: token value on success, negative errno on failure
 */
static int fdice_aq_alloc_token(struct fdice_aq *aq)
{
	int token;

	if (atomic_read(&aq->pending_count) >= aq->max_pending)
		return -FDICE_ERR_QUEUE_FULL;

	token = ida_alloc_max(&aq->tokens, aq->max_pending - 1, GFP_ATOMIC);
	if (token < 0)
		return -FDICE_ERR_QUEUE_FULL;

	atomic_inc(&aq->pending_count);
	return token;
}

/**
 * fdice_aq_submit_desc - Write descriptor to SQ ring and bump tail
 * @hw: hardware instance
 * @aq: admin queue instance
 * @desc: prepared descriptor to submit
 * @buf: indirect command buffer (NULL for direct commands)
 * @buf_len: length of indirect buffer
 * @token: allocated completion token
 *
 * Copies the descriptor onto the send queue ring, handles indirect
 * buffer DMA address setup, and writes the tail register to notify
 * firmware of the new command.
 *
 * Must be called with sq_lock held.
 *
 * Return: 0 on success, negative error on ring full or head overrun
 */
static int fdice_aq_submit_desc(struct fdice_hw *hw,
				struct fdice_aq *aq,
				struct fdice_aq_desc *desc,
				void *buf, u16 buf_len,
				u16 token)
{
	struct fdice_aq_ring *sq = &aq->sq;
	struct fdice_aq_desc *ring_desc;
	u32 head;

	/* Validate head has not overrun */
	head = fdice_aq_rd32(hw, sq->reg_head);
	if (head >= sq->count) {
		dev_err(fdice_hw_to_dev(hw),
			"AQ SQ head overrun: head=%u count=%u\n",
			head, sq->count);
		return -EIO;
	}

	/* Check for available ring space */
	if (FDICE_AQ_RING_UNUSED(sq) == 0) {
		dev_dbg(fdice_hw_to_dev(hw), "AQ SQ ring full\n");
		return -ENOSPC;
	}

	/* Store token in cookie fields for firmware echo-back */
	desc->cookie_low = cpu_to_le32((u32)token);
	desc->cookie_high = cpu_to_le32(0);

	ring_desc = FDICE_AQ_DESC(sq, sq->next_to_use);
	memcpy(ring_desc, desc, sizeof(*ring_desc));

	/* Handle indirect command buffer */
	if (buf && buf_len > 0) {
		struct fdice_aq_buf *dma_buf;

		ring_desc->flags |= cpu_to_le16(FDICE_AQ_FLAG_BUF);
		if (buf_len > FDICE_AQ_LG_BUF_THRESH)
			ring_desc->flags |= cpu_to_le16(FDICE_AQ_FLAG_LB);

		/*
		 * For SQ indirect commands, we use a per-descriptor
		 * DMA staging buffer. Copy the caller's data into it
		 * and point the descriptor at the DMA address.
		 */
		dma_buf = &sq->bufs[sq->next_to_use];
		if (!dma_buf->va) {
			dev_err(fdice_hw_to_dev(hw),
				"AQ SQ buf[%u] not allocated\n",
				sq->next_to_use);
			return -ENOMEM;
		}

		memcpy(dma_buf->va, buf, min_t(u16, buf_len, dma_buf->len));
		ring_desc->datalen = cpu_to_le16(buf_len);
		ring_desc->params.generic.addr_high =
			cpu_to_le32(upper_32_bits(dma_buf->dma));
		ring_desc->params.generic.addr_low =
			cpu_to_le32(lower_32_bits(dma_buf->dma));
	}

	/* Advance producer index with wrap */
	sq->next_to_use++;
	if (sq->next_to_use == sq->count)
		sq->next_to_use = 0;

	/* Memory barrier before tail write to ensure descriptor is visible */
	wmb();

	/* Bump tail to notify firmware */
	fdice_aq_wr32(hw, sq->reg_tail, sq->next_to_use);

	return 0;
}

/**
 * fdice_aq_send_cmd - Submit an AQ command and wait for completion
 * @hw: hardware instance
 * @desc: descriptor with opcode and parameters filled in
 * @buf: DMA buffer for indirect commands (NULL for direct)
 * @buf_len: length of indirect buffer
 *
 * Allocates a completion token, submits the descriptor to the send
 * queue, and blocks on a kernel completion until the firmware responds
 * or the timeout expires. Copies the writeback descriptor back into
 * @desc on success so the caller can inspect response parameters.
 *
 * Context: May sleep. Must not be called from atomic context.
 * Return: 0 on success, negative fdice_err on failure.
 */
int fdice_aq_send_cmd(struct fdice_hw *hw,
		      struct fdice_aq_desc *desc,
		      void *buf, u16 buf_len)
{
	struct fdice_aq *aq = &hw->adminq;
	struct fdice_aq_pending *slot;
	unsigned long flags;
	long wait_ret;
	int token;
	int err;

	if (!aq->initialized)
		return -FDICE_ERR_QUEUE_SHUT;

	/* Validate buffer arguments */
	if ((buf && !buf_len) || (!buf && buf_len))
		return -FDICE_ERR_INVAL;

	if (buf && buf_len > aq->sq_buf_size)
		return -FDICE_ERR_INVAL;

	token = fdice_aq_alloc_token(aq);
	if (token < 0)
		return token;

	/* Initialize the pending slot for synchronous completion */
	slot = &aq->pending[token];
	memset(slot, 0, sizeof(*slot));
	memcpy(&slot->desc, desc, sizeof(*desc));
	slot->buf = buf;
	slot->buf_len = buf_len;
	slot->cb = NULL;
	slot->token = (u16)token;
	slot->submit_jiffies = jiffies;
	init_completion(&slot->completion);

	/* Submit under the SQ spinlock */
	spin_lock_irqsave(&aq->sq_lock, flags);
	err = fdice_aq_submit_desc(hw, aq, desc, buf, buf_len, (u16)token);
	spin_unlock_irqrestore(&aq->sq_lock, flags);

	if (err) {
		ida_free(&aq->tokens, token);
		atomic_dec(&aq->pending_count);
		return err;
	}

	/*
	 * Synchronous polling fallback: spin reading the SQ head register
	 * until firmware advances past our descriptor. This is essential
	 * during early init when the AQ interrupt and workqueue are not
	 * yet wired up. On each iteration, call process_completions to
	 * check DD flags and signal the completion.
	 */
	{
		unsigned long poll_deadline;
		bool poll_done = false;

		poll_deadline = jiffies + aq->timeout_jiffies;

		while (time_before(jiffies, poll_deadline)) {
			fdice_aq_process_completions(hw, aq);

			if (try_wait_for_completion(&slot->completion)) {
				poll_done = true;
				break;
			}

			usleep_range(50, 100);
		}

		if (!poll_done) {
			/*
			 * Polling did not complete — fall through to
			 * wait_for_completion as a safety net in case
			 * an interrupt fires late.
			 */
			wait_ret = wait_for_completion_timeout(
					&slot->completion,
					msecs_to_jiffies(10));
			if (wait_ret == 0) {
				dev_warn(fdice_hw_to_dev(hw),
					 "AQ cmd 0x%04x token %u timed out after %u ms\n",
					 le16_to_cpu(desc->opcode), token,
					 jiffies_to_msecs(aq->timeout_jiffies));

				fdice_aq_stats_record_timeout(aq,
					le16_to_cpu(desc->opcode));
				slot->status = FDICE_ERR_TIMEOUT;
			}
		}
	}

	/* Copy response descriptor back to caller */
	memcpy(desc, &slot->desc, sizeof(*desc));

	ida_free(&aq->tokens, token);
	atomic_dec(&aq->pending_count);

	/*
	 * Translate FDICE_ERR_* to negative errno before returning.
	 * All callers expect kernel-standard negative errno on failure.
	 * Returning a positive FDICE_ERR_* value would propagate out of
	 * probe() as a non-negative non-zero return, which causes the PCI
	 * subsystem to leave the device in an inconsistent half-bound state
	 * and crash on subsequent rmmod.
	 */
	return fdice_err_to_errno(slot->status);
}

/**
 * fdice_aq_send_cmd_async - Submit an AQ command with async completion
 * @hw: hardware instance
 * @desc: descriptor with opcode and parameters
 * @buf: DMA buffer for indirect commands (NULL for direct)
 * @buf_len: length of indirect buffer
 * @cb: callback to invoke on completion (from workqueue context)
 *
 * Submits the command and returns immediately. The callback is invoked
 * from the completion workqueue when firmware responds. The caller
 * must not free @buf until the callback fires or the command is
 * cancelled during queue shutdown.
 *
 * Context: May be called from any context (uses GFP_ATOMIC internally).
 * Return: 0 if submitted successfully, negative fdice_err on failure.
 */
int fdice_aq_send_cmd_async(struct fdice_hw *hw,
			    struct fdice_aq_desc *desc,
			    void *buf, u16 buf_len,
			    struct fdice_aq_callback *cb)
{
	struct fdice_aq *aq = &hw->adminq;
	struct fdice_aq_pending *slot;
	unsigned long flags;
	int token;
	int err;

	if (!aq->initialized)
		return -FDICE_ERR_QUEUE_SHUT;

	if (!cb || !cb->fn)
		return -FDICE_ERR_INVAL;

	if ((buf && !buf_len) || (!buf && buf_len))
		return -FDICE_ERR_INVAL;

	if (buf && buf_len > aq->sq_buf_size)
		return -FDICE_ERR_INVAL;

	token = fdice_aq_alloc_token(aq);
	if (token < 0)
		return token;

	/* Initialize the pending slot for async completion */
	slot = &aq->pending[token];
	memset(slot, 0, sizeof(*slot));
	memcpy(&slot->desc, desc, sizeof(*desc));
	slot->buf = buf;
	slot->buf_len = buf_len;
	slot->cb = cb;
	slot->token = (u16)token;
	slot->submit_jiffies = jiffies;

	/* Submit under the SQ spinlock */
	spin_lock_irqsave(&aq->sq_lock, flags);
	err = fdice_aq_submit_desc(hw, aq, desc, buf, buf_len, (u16)token);
	spin_unlock_irqrestore(&aq->sq_lock, flags);

	if (err) {
		ida_free(&aq->tokens, token);
		atomic_dec(&aq->pending_count);
		return err;
	}

	return 0;
}

/* ---- Queue init/shutdown ---------------------------------------------- */

/**
 * fdice_aq_alloc_sq_bufs - Allocate per-descriptor DMA buffers for SQ
 * @hw: hardware instance
 * @ring: send queue ring
 * @buf_size: maximum buffer size per descriptor
 *
 * Pre-allocates staging buffers for indirect SQ commands. The caller's
 * data is copied into these buffers before submission so the firmware
 * can DMA directly from them.
 *
 * Return: 0 on success, -ENOMEM on allocation failure
 */
static int fdice_aq_alloc_sq_bufs(struct fdice_hw *hw,
				  struct fdice_aq_ring *ring,
				  u16 buf_size)
{
	struct device *dev = fdice_hw_to_dev(hw);
	int i;

	ring->bufs = devm_kcalloc(dev, ring->count,
				  sizeof(struct fdice_aq_buf), GFP_KERNEL);
	if (!ring->bufs)
		return -ENOMEM;

	for (i = 0; i < ring->count; i++) {
		ring->bufs[i].va = dmam_alloc_coherent(dev, buf_size,
						       &ring->bufs[i].dma,
						       GFP_KERNEL);
		if (!ring->bufs[i].va) {
			dev_err(dev, "AQ SQ buf[%d] alloc failed\n", i);
			return -ENOMEM;
		}
		ring->bufs[i].len = buf_size;
	}

	return 0;
}

/**
 * fdice_ctlq_init - Initialize a control queue pair (SQ + RQ)
 * @hw: hardware instance
 * @aq: admin queue instance to initialize
 * @type: control queue type (admin, mailbox, sideband)
 *
 * Performs the full initialization sequence for a control queue:
 *   1. Assign register offsets based on queue type
 *   2. Set buffer sizes and entry counts
 *   3. Allocate SQ and RQ descriptor rings (devres-managed DMA)
 *   4. Allocate per-descriptor buffers (RQ pre-populated, SQ staging)
 *   5. Initialize token IDA and pending command slots array
 *   6. Create dedicated completion workqueue
 *   7. Initialize per-opcode statistics xarray
 *   8. Program BAL/BAH/LEN registers to activate queues
 *   9. Set RQ tail to (count - 1) to make all RQ slots available
 *
 * Return: 0 on success, negative errno on failure
 */
int fdice_ctlq_init(struct fdice_hw *hw, struct fdice_aq *aq,
		    enum fdice_ctlq_type type)
{
	struct device *dev = fdice_hw_to_dev(hw);
	int err;

	if (aq->initialized) {
		dev_warn(dev, "AQ already initialized, type=%d\n", type);
		return -EBUSY;
	}

	/* Step 1: Set register offsets based on queue type */
	switch (type) {
	case FDICE_CTL_Q_ADMIN:
		fdice_aq_set_admin_regs(aq);
		aq->num_sq_entries = FDICE_AQ_ADMIN_SQ_ENTRIES;
		aq->num_rq_entries = FDICE_AQ_ADMIN_RQ_ENTRIES;
		aq->sq_buf_size    = FDICE_AQ_ADMIN_MAX_BUF;
		aq->rq_buf_size    = FDICE_AQ_ADMIN_MAX_BUF;
		aq->max_pending    = FDICE_AQ_ADMIN_MAX_PENDING;
		break;

	case FDICE_CTL_Q_MAILBOX:
		fdice_aq_set_mbx_regs(aq);
		aq->num_sq_entries = FDICE_AQ_MBX_SQ_ENTRIES;
		aq->num_rq_entries = FDICE_AQ_MBX_RQ_ENTRIES;
		aq->sq_buf_size    = FDICE_AQ_MBX_MAX_BUF;
		aq->rq_buf_size    = FDICE_AQ_MBX_MAX_BUF;
		aq->max_pending    = FDICE_AQ_MBX_MAX_PENDING;
		break;

	case FDICE_CTL_Q_SIDEBAND:
		fdice_aq_set_sb_regs(aq);
		aq->num_sq_entries = FDICE_AQ_SB_SQ_ENTRIES;
		aq->num_rq_entries = FDICE_AQ_SB_RQ_ENTRIES;
		aq->sq_buf_size    = FDICE_AQ_SB_MAX_BUF;
		aq->rq_buf_size    = FDICE_AQ_SB_MAX_BUF;
		aq->max_pending    = FDICE_AQ_SB_MAX_PENDING;
		break;

	default:
		dev_err(dev, "Unknown control queue type %d\n", type);
		return -EINVAL;
	}

	/* Step 2: Validate configuration */
	if (!aq->num_sq_entries || !aq->num_rq_entries ||
	    !aq->sq_buf_size || !aq->rq_buf_size) {
		dev_err(dev, "AQ invalid configuration\n");
		return -EINVAL;
	}

	/* Step 3: Initialize concurrency primitives */
	spin_lock_init(&aq->sq_lock);
	spin_lock_init(&aq->rq_lock);
	aq->timeout_jiffies = FDICE_AQ_CMD_TIMEOUT;

	/* Step 4: Allocate SQ descriptor ring */
	err = fdice_aq_alloc_ring(hw, &aq->sq, aq->num_sq_entries);
	if (err) {
		dev_err(dev, "AQ SQ ring alloc failed: %d\n", err);
		return err;
	}

	/* Step 5: Allocate RQ descriptor ring */
	err = fdice_aq_alloc_ring(hw, &aq->rq, aq->num_rq_entries);
	if (err) {
		dev_err(dev, "AQ RQ ring alloc failed: %d\n", err);
		return err;
	}

	/* Step 6: Allocate SQ per-descriptor staging buffers */
	err = fdice_aq_alloc_sq_bufs(hw, &aq->sq, aq->sq_buf_size);
	if (err) {
		dev_err(dev, "AQ SQ buffer alloc failed: %d\n", err);
		return err;
	}

	/* Step 7: Allocate RQ per-descriptor receive buffers */
	err = fdice_aq_alloc_rq_bufs(hw, &aq->rq, aq->rq_buf_size);
	if (err) {
		dev_err(dev, "AQ RQ buffer alloc failed: %d\n", err);
		return err;
	}

	/* Step 8: Initialize token IDA and pending slots */
	ida_init(&aq->tokens);
	atomic_set(&aq->pending_count, 0);

	aq->pending = devm_kcalloc(dev, aq->max_pending,
				   sizeof(struct fdice_aq_pending),
				   GFP_KERNEL);
	if (!aq->pending) {
		dev_err(dev, "AQ pending array alloc failed\n");
		err = -ENOMEM;
		goto err_ida;
	}

	/* Step 9: Create completion workqueue */
	aq->completion_wq = alloc_ordered_workqueue("fdice_aq_%s",
						    WQ_MEM_RECLAIM,
						    dev_name(dev));
	if (!aq->completion_wq) {
		dev_err(dev, "AQ completion workqueue alloc failed\n");
		err = -ENOMEM;
		goto err_ida;
	}

	/* Step 10: Initialize per-opcode statistics */
	xa_init(&aq->stats);

	/* Step 11: Program SQ registers and enable */
	err = fdice_aq_cfg_ring_regs(hw, &aq->sq);
	if (err) {
		dev_err(dev, "AQ SQ register config failed: %d\n", err);
		goto err_wq;
	}

	/* Step 12: Program RQ registers and enable */
	err = fdice_aq_cfg_ring_regs(hw, &aq->rq);
	if (err) {
		dev_err(dev, "AQ RQ register config failed: %d\n", err);
		goto err_disable_sq;
	}

	/*
	 * Step 13: Set the RQ tail to (count - 1) so firmware knows all
	 * RQ descriptors are available for receiving events.
	 */
	fdice_aq_wr32(hw, aq->rq.reg_tail, aq->rq.count - 1);
	fdice_aq_flush(hw);

	aq->initialized = true;

	dev_dbg(dev, "AQ type=%d initialized: sq=%u rq=%u pending=%u\n",
		type, aq->num_sq_entries, aq->num_rq_entries,
		aq->max_pending);

	return 0;

err_disable_sq:
	fdice_aq_disable_ring_regs(hw, &aq->sq);
err_wq:
	fdice_aq_stats_destroy(aq);
	destroy_workqueue(aq->completion_wq);
	aq->completion_wq = NULL;
err_ida:
	ida_destroy(&aq->tokens);
	return err;
}

/**
 * fdice_ctlq_shutdown - Shut down a control queue pair
 * @hw: hardware instance
 * @aq: admin queue instance to shut down
 *
 * Performs ordered teardown of a control queue:
 *   1. Mark uninitialized to reject new submissions
 *   2. Cancel all pending commands (invoke callbacks with QUEUE_SHUT)
 *   3. Drain the completion workqueue
 *   4. Disable queue registers (clear LEN enable bit)
 *   5. Destroy statistics, token IDA, and workqueue
 *
 * DMA memory is freed automatically by devres when the device is
 * removed, so explicit free calls are not needed here.
 *
 * Critical ordering: pending commands are cancelled BEFORE the queue
 * is disabled in hardware, avoiding the race condition where DMA
 * memory is freed while firmware still has active descriptors.
 */
void fdice_ctlq_shutdown(struct fdice_hw *hw, struct fdice_aq *aq)
{
	struct fdice_aq_pending *slot;
	unsigned long flags;
	int i;

	if (!aq->initialized)
		return;

	/*
	 * Step 1: Mark queue as not initialized. This prevents any new
	 * command submissions from entering the queue after this point.
	 */
	aq->initialized = false;

	/*
	 * Step 2: Cancel all pending commands. Walk the pending array
	 * and complete every outstanding slot with FDICE_ERR_QUEUE_SHUT.
	 * This must happen before we disable the queue in hardware to
	 * avoid leaving waiters blocked forever.
	 */
	spin_lock_irqsave(&aq->sq_lock, flags);
	for (i = 0; i < aq->max_pending; i++) {
		slot = &aq->pending[i];

		/* Skip unused slots (token not allocated in IDA) */
		if (!slot->submit_jiffies)
			continue;

		slot->status = FDICE_ERR_QUEUE_SHUT;

		if (slot->cb && slot->cb->fn) {
			/* Async: invoke callback with shutdown error */
			slot->cb->fn(hw, &slot->desc, slot->buf,
				     FDICE_ERR_QUEUE_SHUT, slot->cb->ctx);
		} else {
			/* Sync: wake blocked waiter */
			complete(&slot->completion);
		}
	}
	spin_unlock_irqrestore(&aq->sq_lock, flags);

	/*
	 * Step 3: Drain the completion workqueue. Any work items already
	 * scheduled will complete before this returns.
	 */
	if (aq->completion_wq) {
		drain_workqueue(aq->completion_wq);
		destroy_workqueue(aq->completion_wq);
		aq->completion_wq = NULL;
	}

	/*
	 * Step 4: Disable queue registers. Clear the enable bit in
	 * the LEN register and zero all ring registers.
	 */
	fdice_aq_disable_ring_regs(hw, &aq->sq);
	fdice_aq_disable_ring_regs(hw, &aq->rq);

	/* Step 5: Destroy per-opcode statistics */
	fdice_aq_stats_destroy(aq);

	/* Step 6: Destroy token allocator */
	ida_destroy(&aq->tokens);
	atomic_set(&aq->pending_count, 0);

	dev_dbg(fdice_hw_to_dev(hw), "AQ shutdown complete\n");
}

/**
 * fdice_aq_schedule_completion - Schedule completion processing
 * @hw: hardware instance
 * @aq: admin queue instance
 *
 * Queues a work item on the completion workqueue to process
 * outstanding completions. Called from the MSI-X interrupt handler
 * or periodic polling timer.
 */
void fdice_aq_schedule_completion(struct fdice_hw *hw, struct fdice_aq *aq)
{
	struct fdice_aq_work *aw;

	if (!aq->initialized || !aq->completion_wq)
		return;

	/*
	 * Allocate a work item. This is a small allocation from the
	 * atomic pool since it may be called from interrupt context.
	 */
	aw = kzalloc(sizeof(*aw), GFP_ATOMIC);
	if (!aw)
		return;

	aw->hw = hw;
	aw->aq = aq;
	INIT_WORK(&aw->work, fdice_aq_completion_work);

	if (!queue_work(aq->completion_wq, &aw->work))
		kfree(aw);
}
