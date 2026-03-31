// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_AQ_H_
#define _FDICE_AQ_H_

/**
 * DOC: FrankenDrICE Admin Queue Header
 *
 * Defines the AQ ring structures, completion tracking, error codes,
 * register constants, MMIO helpers, and command submission API for
 * the E810 control queue interface.
 *
 * The descriptor format and opcodes live in fdice_aq_cmd.h.
 * This header adds the ring-level machinery and control queue instance.
 *
 * Architecture reference: docs/architecture/01-hal-design.md
 */

#include <linux/types.h>
#include <linux/idr.h>
#include <linux/completion.h>
#include <linux/workqueue.h>
#include <linux/xarray.h>
#include <linux/atomic.h>
#include <linux/spinlock.h>
#include <linux/io.h>
#include <linux/bitfield.h>

#include "fdice_aq_cmd.h"

/* Forward declarations */
struct fdice_hw;

/* -----------------------------------------------------------------------
 * AQ Register Offsets -- hardware-fixed per E810 datasheet
 * ----------------------------------------------------------------------- */

/* Admin Queue - Transmit (ATQ = Admin Transmit Queue) */
#define FDICE_REG_ATQ_BAL		0x00080000
#define FDICE_REG_ATQ_BAH		0x00080100
#define FDICE_REG_ATQ_LEN		0x00080200
#define FDICE_REG_ATQ_HEAD		0x00080300
#define FDICE_REG_ATQ_TAIL		0x00080400

/* Admin Queue - Receive (ARQ = Admin Receive Queue) */
#define FDICE_REG_ARQ_BAL		0x00080080
#define FDICE_REG_ARQ_BAH		0x00080180
#define FDICE_REG_ARQ_LEN		0x00080280
#define FDICE_REG_ARQ_HEAD		0x00080380
#define FDICE_REG_ARQ_TAIL		0x00080480

/* Mailbox Queue - Transmit */
#define FDICE_REG_MBX_ATQ_BAL		0x0022E100
#define FDICE_REG_MBX_ATQ_BAH		0x0022E180
#define FDICE_REG_MBX_ATQ_LEN		0x0022E200
#define FDICE_REG_MBX_ATQ_HEAD		0x0022E280
#define FDICE_REG_MBX_ATQ_TAIL		0x0022E300

/* Mailbox Queue - Receive */
#define FDICE_REG_MBX_ARQ_BAL		0x0022E380
#define FDICE_REG_MBX_ARQ_BAH		0x0022E400
#define FDICE_REG_MBX_ARQ_LEN		0x0022E480
#define FDICE_REG_MBX_ARQ_HEAD		0x0022E500
#define FDICE_REG_MBX_ARQ_TAIL		0x0022E580

/* Sideband Queue - Transmit */
#define FDICE_REG_SB_ATQ_BAL		0x0022FC00
#define FDICE_REG_SB_ATQ_BAH		0x0022FC80
#define FDICE_REG_SB_ATQ_LEN		0x0022FD00
#define FDICE_REG_SB_ATQ_HEAD		0x0022FD80
#define FDICE_REG_SB_ATQ_TAIL		0x0022FE00

/* Sideband Queue - Receive */
#define FDICE_REG_SB_ARQ_BAL		0x0022FE80
#define FDICE_REG_SB_ARQ_BAH		0x0022FF00
#define FDICE_REG_SB_ARQ_LEN		0x0022FF80
#define FDICE_REG_SB_ARQ_HEAD		0x00230000
#define FDICE_REG_SB_ARQ_TAIL		0x00230080

/* Queue Length register bit fields (common across all queue types) */
#define FDICE_REG_CTLQ_LEN_M		GENMASK(9, 0)
#define FDICE_REG_CTLQ_LEN_VFE		BIT(28)
#define FDICE_REG_CTLQ_LEN_OVFL	BIT(29)
#define FDICE_REG_CTLQ_LEN_CRIT	BIT(30)
#define FDICE_REG_CTLQ_LEN_ENABLE	BIT(31)

/* Queue Head register mask (common across all queue types) */
#define FDICE_REG_CTLQ_HEAD_M		GENMASK(9, 0)

/* -----------------------------------------------------------------------
 * Error Code System
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_err - Unified error codes for the fdice_ driver
 *
 * Values 0-99 map to E810 firmware AQ return codes (retval field).
 * Values 100+ are software-defined errors for driver-internal conditions.
 */
enum fdice_err {
	FDICE_ERR_OK		= 0,
	FDICE_ERR_PERM		= 1,
	FDICE_ERR_NOENT		= 2,
	FDICE_ERR_BAD_OPCODE	= 3,
	FDICE_ERR_IO		= 5,
	FDICE_ERR_AGAIN		= 8,
	FDICE_ERR_NOMEM		= 9,
	FDICE_ERR_ACCESS	= 10,
	FDICE_ERR_BUSY		= 12,
	FDICE_ERR_EXIST		= 13,
	FDICE_ERR_INVAL		= 14,
	FDICE_ERR_NOSPC		= 16,
	FDICE_ERR_NOSYS		= 17,
	FDICE_ERR_MODE		= 21,
	FDICE_ERR_NOSEC		= 24,
	FDICE_ERR_BADSIG	= 25,
	FDICE_ERR_SVN		= 26,
	FDICE_ERR_BADMAN	= 27,
	FDICE_ERR_BADBUF	= 28,

	/* Software-defined errors */
	FDICE_ERR_TIMEOUT	= 100,
	FDICE_ERR_RESET		= 101,
	FDICE_ERR_QUEUE_FULL	= 102,
	FDICE_ERR_QUEUE_SHUT	= 103,
};

int fdice_aq_retval_to_err(u16 retval);
int fdice_err_to_errno(int err);

/* -----------------------------------------------------------------------
 * Hardware State Tracking
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_hw_state - Hardware initialization state bitmask
 * @FDICE_HW_AQ_SW_INIT: AQ software state allocated
 * @FDICE_HW_AQ_ENABLED: Admin queue operational
 * @FDICE_HW_MBX_ENABLED: Mailbox queue operational
 * @FDICE_HW_SB_ENABLED: Sideband queue operational
 * @FDICE_HW_CAPS_LOADED: Device/function capabilities discovered
 * @FDICE_HW_REMOVING: Device removal in progress
 */
enum fdice_hw_state {
	FDICE_HW_AQ_SW_INIT	= BIT(0),
	FDICE_HW_AQ_ENABLED	= BIT(1),
	FDICE_HW_MBX_ENABLED	= BIT(2),
	FDICE_HW_SB_ENABLED	= BIT(3),
	FDICE_HW_CAPS_LOADED	= BIT(4),
	FDICE_HW_REMOVING	= BIT(5),
};

/* -----------------------------------------------------------------------
 * AQ Ring and Buffer Structures
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_aq_buf - Per-descriptor DMA buffer for indirect commands
 * @va: Virtual address of DMA buffer
 * @dma: DMA address of buffer
 * @len: Allocated length of buffer in bytes
 */
struct fdice_aq_buf {
	void			*va;
	dma_addr_t		dma;
	u16			len;
};

/**
 * struct fdice_aq_ring - Single control queue ring (send or receive)
 * @desc: DMA-coherent descriptor ring
 * @desc_dma: DMA bus address of descriptor ring base
 * @bufs: Array of per-descriptor DMA buffer metadata
 * @count: Number of descriptors in ring
 * @next_to_use: Producer index
 * @next_to_clean: Consumer index
 * @reg_head: MMIO offset of head register
 * @reg_tail: MMIO offset of tail register
 * @reg_len: MMIO offset of length/enable register
 * @reg_bal: MMIO offset of base address low register
 * @reg_bah: MMIO offset of base address high register
 */
struct fdice_aq_ring {
	struct fdice_aq_desc	*desc;
	dma_addr_t		desc_dma;
	struct fdice_aq_buf	*bufs;
	u16			count;
	u16			next_to_use;
	u16			next_to_clean;
	u32			reg_head;
	u32			reg_tail;
	u32			reg_len;
	u32			reg_bal;
	u32			reg_bah;
};

/* -----------------------------------------------------------------------
 * Token-Based Command Tracking
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_aq_callback - Async command completion context
 * @fn: Callback invoked from completion workqueue
 * @ctx: Opaque caller context
 */
struct fdice_aq_callback {
	void (*fn)(struct fdice_hw *hw,
		   struct fdice_aq_desc *resp,
		   void *buf, int status, void *ctx);
	void *ctx;
};

/**
 * struct fdice_aq_pending - Pending command slot
 * @desc: Copy of submitted descriptor
 * @buf: DMA buffer reference
 * @buf_len: Buffer length in bytes
 * @cb: Async callback pointer (NULL for synchronous)
 * @completion: Kernel completion for synchronous waiters
 * @status: Completion status
 * @submit_jiffies: Timestamp at submission
 * @token: Token value assigned to this command
 */
struct fdice_aq_pending {
	struct fdice_aq_desc	desc;
	void			*buf;
	u16			buf_len;
	struct fdice_aq_callback *cb;
	struct completion	completion;
	int			status;
	unsigned long		submit_jiffies;
	u16			token;
};

/**
 * struct fdice_aq_cmd_stats - Per-opcode command statistics
 * @count: Total commands submitted
 * @success: Successful completions
 * @error: Error completions
 * @timeout: Commands that timed out
 * @total_usec: Cumulative execution time
 * @max_usec: Maximum single-command execution time
 * @last_err: Last AQ error code seen
 */
struct fdice_aq_cmd_stats {
	atomic64_t	count;
	atomic64_t	success;
	atomic64_t	error;
	atomic64_t	timeout;
	atomic64_t	total_usec;
	atomic64_t	max_usec;
	u16		last_err;
};

/* -----------------------------------------------------------------------
 * Control Queue Instance
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_ctlq_type - Control queue type identifier
 * @FDICE_CTL_Q_ADMIN: Primary firmware command interface
 * @FDICE_CTL_Q_MAILBOX: PF-VF communication channel
 * @FDICE_CTL_Q_SIDEBAND: Sideband control channel
 */
enum fdice_ctlq_type {
	FDICE_CTL_Q_ADMIN	= 0,
	FDICE_CTL_Q_MAILBOX	= 1,
	FDICE_CTL_Q_SIDEBAND	= 2,
};

/**
 * struct fdice_aq - Admin Queue instance (one per control queue type)
 * @sq: Send queue ring
 * @rq: Receive queue ring
 * @sq_lock: Protects send queue ring submission
 * @rq_lock: Protects receive queue event processing
 * @sq_buf_size: Maximum buffer size for SQ indirect commands
 * @rq_buf_size: Maximum buffer size for RQ events
 * @num_sq_entries: Number of send queue descriptors
 * @num_rq_entries: Number of receive queue descriptors
 * @tokens: IDA allocator for concurrent command tracking tokens
 * @pending: Array of pending command slots
 * @max_pending: Maximum concurrent commands
 * @pending_count: Current number of in-flight commands
 * @completion_wq: Workqueue for processing completions
 * @stats: Per-opcode statistics xarray
 * @timeout_jiffies: Default command timeout
 * @initialized: Set true when queue is operational
 */
struct fdice_aq {
	struct fdice_aq_ring	sq;
	struct fdice_aq_ring	rq;

	spinlock_t		sq_lock;
	spinlock_t		rq_lock;

	u16			sq_buf_size;
	u16			rq_buf_size;
	u16			num_sq_entries;
	u16			num_rq_entries;

	struct ida		tokens;
	struct fdice_aq_pending	*pending;
	u16			max_pending;
	atomic_t		pending_count;

	struct workqueue_struct	*completion_wq;

	struct xarray		stats;
	unsigned long		timeout_jiffies;

	bool			initialized;
};

/**
 * struct fdice_aq_work - Work item for deferred completion processing
 * @work: kernel work_struct
 * @hw: hardware instance backpointer
 * @aq: admin queue instance to process
 */
struct fdice_aq_work {
	struct work_struct	work;
	struct fdice_hw		*hw;
	struct fdice_aq		*aq;
};

/**
 * fdice_aq_event_handler_t - Callback type for async event processing
 * @hw: hardware instance
 * @desc: event descriptor from firmware
 * @buf: indirect data buffer (may be NULL)
 * @len: length of indirect data
 * @ctx: opaque context
 */
typedef void (*fdice_aq_event_handler_t)(struct fdice_hw *hw,
					 struct fdice_aq_desc *desc,
					 void *buf, u16 len, void *ctx);

/* -----------------------------------------------------------------------
 * Control Queue Lifecycle API
 * ----------------------------------------------------------------------- */

int fdice_ctlq_init(struct fdice_hw *hw, struct fdice_aq *aq,
		     enum fdice_ctlq_type type);
void fdice_ctlq_shutdown(struct fdice_hw *hw, struct fdice_aq *aq);

/* -----------------------------------------------------------------------
 * Command Submission API
 * ----------------------------------------------------------------------- */

void fdice_aq_prep_desc(struct fdice_aq_desc *desc, u16 opcode);

int fdice_aq_send_cmd(struct fdice_hw *hw,
		      struct fdice_aq_desc *desc,
		      void *buf, u16 buf_len);

int fdice_aq_send_cmd_async(struct fdice_hw *hw,
			    struct fdice_aq_desc *desc,
			    void *buf, u16 buf_len,
			    struct fdice_aq_callback *cb);

int fdice_aq_process_completions(struct fdice_hw *hw, struct fdice_aq *aq);

int fdice_aq_process_events(struct fdice_hw *hw, struct fdice_aq *aq,
			    fdice_aq_event_handler_t handler, void *ctx);

void fdice_aq_schedule_completion(struct fdice_hw *hw, struct fdice_aq *aq);

#endif /* _FDICE_AQ_H_ */
