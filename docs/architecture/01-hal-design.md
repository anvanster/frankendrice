<!-- SPDX-License-Identifier: GPL-2.0 -->
<!-- Copyright (c) 2026, FrankenDrICE Contributors. -->

# Architecture 01: Hardware Abstraction Layer Design

> **The HAL is the most critical architectural boundary in FrankenDrICE.**
> It separates what the E810 firmware dictates from what we are free to redesign.

## 1. HAL Boundary Definition

The E810 Admin Queue command interface is the hardware boundary. Everything
below it must conform to the E810 firmware protocol exactly. Everything above
it is software architecture -- our design space.

### 1.1 Below the Boundary (Hardware-Fixed)

These elements are defined by E810 firmware and must be reproduced exactly:

| Element | Constraint | Reference |
|---------|-----------|-----------|
| `struct fdice_aq_desc` | 32-byte descriptor, exact field layout matching `libie_aq_desc` | Exploration 11 §2 |
| Opcode values | All ~100 opcodes are firmware protocol constants (0x0001-0xFF33) | Exploration 11 §3 |
| Descriptor flags | DD, CMP, ERR, BUF, LB, RD, VFE, VFC, SI, EI, FE -- bit positions and semantics | Exploration 11 §2.1 |
| Error codes | `libie_aq_err` values (RC_OK through RC_EBADBUF) | Exploration 11 §2.3 |
| Parameter structs | Per-opcode 16-byte parameter layouts (ice_aqc_* equivalents) | Exploration 11 §8 |
| Ring protocol | Head/tail register semantics, descriptor writeback, DMA buffer addressing | Exploration 11 §4.2 |
| Buffer conventions | Direct (16B in descriptor) vs indirect (BUF flag + DMA), 4096B max, 512B large threshold | Exploration 11 §2.2, §4.3 |
| Command types | Direct commands, indirect commands, async events on ARQ | Exploration 11 §2.2, §5.5 |

### 1.2 Above the Boundary (Software Design Space)

These are software choices where we apply mlx5-inspired patterns:

| Element | ice Approach | fdice_ Redesign |
|---------|-------------|-----------------|
| Completion model | Synchronous busy-wait polling | Workqueue-based with callbacks |
| Concurrency | Single mutex, one command at a time | Token-based tracking, multiple in-flight |
| Command abstraction | Flat function namespace (`ice_aq_*`) | Subsystem vtable (`struct fdice_hw_ops`) |
| Error handling | Raw `retval` propagation | Translated `enum fdice_err` with context |
| Statistics | None | Per-opcode execution time and count |
| Retry policy | Hardcoded EBUSY retry for select opcodes | Configurable per-opcode retry policy |
| Global locking | Single `cfg_lock_sw` mutex for package ops | Per-subsystem locking with priority |
| Event dispatch | Switch statement in `__ice_clean_ctrlq` | Registered handler table with opcode matching |
| Command grouping | Flat `ice_common.c` / `ice_sched.c` etc. | `struct fdice_hw_ops` vtable by subsystem |

## 2. Admin Queue Architecture

### 2.1 Ring Structure: `struct fdice_aq`

Each control queue (Admin, Mailbox, Sideband) is represented by a
`struct fdice_aq` containing paired send and receive rings.

```c
/**
 * struct fdice_aq_ring - Single control queue ring (send or receive)
 * @desc: DMA-coherent descriptor ring (array of fdice_aq_desc)
 * @desc_dma: DMA address of descriptor ring
 * @bufs: Array of per-descriptor DMA buffer metadata
 * @count: Number of descriptors in ring (power of 2)
 * @next_to_use: Producer index -- next slot driver will write
 * @next_to_clean: Consumer index -- next slot driver will read back
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

/**
 * struct fdice_aq_buf - Per-descriptor DMA buffer for indirect commands
 * @va: Virtual address of DMA buffer
 * @dma: DMA address of buffer
 * @len: Allocated length of buffer
 */
struct fdice_aq_buf {
	void			*va;
	dma_addr_t		dma;
	u16			len;
};
```

The top-level AQ structure owns both rings plus the concurrency and
completion tracking machinery:

```c
/**
 * struct fdice_aq - Admin Queue instance (one per control queue type)
 * @sq: Send queue ring
 * @rq: Receive queue ring
 * @sq_lock: Protects send queue ring submission
 * @rq_lock: Protects receive queue event processing
 * @sq_buf_size: Maximum buffer size for send queue indirect commands
 * @rq_buf_size: Maximum buffer size for receive queue events
 * @num_sq_entries: Number of send queue descriptors
 * @num_rq_entries: Number of receive queue descriptors
 * @tokens: Token allocator for concurrent command tracking
 * @pending: Array of pending command slots (indexed by token)
 * @max_pending: Maximum concurrent commands (sized per queue type)
 * @pending_count: Current number of in-flight commands
 * @completion_wq: Workqueue for processing completions
 * @stats: Per-opcode statistics (xarray indexed by opcode)
 * @timeout_jiffies: Default command timeout
 * @initialized: Set when queue is fully operational
 */
struct fdice_aq {
	struct fdice_aq_ring	sq;
	struct fdice_aq_ring	rq;

	/* Concurrency control */
	spinlock_t		sq_lock;
	spinlock_t		rq_lock;

	/* Queue parameters */
	u16			sq_buf_size;
	u16			rq_buf_size;
	u16			num_sq_entries;
	u16			num_rq_entries;

	/* Token-based command tracking */
	struct ida		tokens;
	struct fdice_aq_pending	*pending;
	u16			max_pending;
	atomic_t		pending_count;

	/* Completion processing */
	struct workqueue_struct	*completion_wq;

	/* Diagnostics */
	struct xarray		stats;
	unsigned long		timeout_jiffies;

	bool			initialized;
};
```

### 2.2 Descriptor Format: `struct fdice_aq_desc`

This struct must match the E810 hardware layout byte-for-byte. It is
functionally identical to `libie_aq_desc` but lives in fdice_ namespace
for self-containment.

```c
/**
 * struct fdice_aq_desc - Admin Queue descriptor (hardware-fixed, 32 bytes)
 * @flags: Control flags (DD, CMP, ERR, BUF, LB, RD, etc.)
 * @opcode: AQ command opcode (firmware-defined)
 * @datalen: Length of indirect data buffer
 * @retval: Firmware return value (fdice_aq_err)
 * @cookie_high: Opaque driver data (used for completion token)
 * @cookie_low: Opaque driver data (used for completion token)
 * @params: Command-specific parameters (16 bytes, union)
 *
 * This layout is dictated by E810 firmware and must not be modified.
 * The cookie fields carry the fdice_ completion token for matching
 * responses to outstanding commands.
 */
struct fdice_aq_desc {
	__le16		flags;
	__le16		opcode;
	__le16		datalen;
	__le16		retval;
	__le32		cookie_high;
	__le32		cookie_low;
	union {
		u8			raw[16];
		struct fdice_aqp_generic	generic;
		/* Per-opcode parameter structs declared elsewhere */
	} params;
};

static_assert(sizeof(struct fdice_aq_desc) == 32,
	      "AQ descriptor must be exactly 32 bytes");

/* Descriptor flags -- bit positions are hardware-fixed */
#define FDICE_AQ_FLAG_DD	BIT(0)   /* Descriptor Done (FW->Driver) */
#define FDICE_AQ_FLAG_CMP	BIT(1)   /* Completion (FW->Driver) */
#define FDICE_AQ_FLAG_ERR	BIT(2)   /* Error (FW->Driver) */
#define FDICE_AQ_FLAG_VFE	BIT(3)   /* VF Error (FW->Driver) */
#define FDICE_AQ_FLAG_LB	BIT(9)   /* Large Buffer (Driver->FW) */
#define FDICE_AQ_FLAG_RD	BIT(10)  /* Read direction (Driver->FW) */
#define FDICE_AQ_FLAG_VFC	BIT(11)  /* VF Command (Driver->FW) */
#define FDICE_AQ_FLAG_BUF	BIT(12)  /* Buffer present (Driver->FW) */
#define FDICE_AQ_FLAG_SI	BIT(13)  /* Security (Driver->FW) */
#define FDICE_AQ_FLAG_EI	BIT(14)  /* Error Interrupt (Driver->FW) */
#define FDICE_AQ_FLAG_FE	BIT(15)  /* Fatal Error (Driver->FW) */

#define FDICE_AQ_MAX_BUF_LEN	4096
#define FDICE_AQ_LG_BUF_THRESH	512
```

### 2.3 Command Submission API

The primary submission interface supports both synchronous and asynchronous
modes. This is the key divergence from ice's single-mutex busy-wait model.

**Design rationale**: ice serializes all AQ commands behind a single mutex
and busy-waits for each completion. This limits throughput to one command at
a time and wastes CPU cycles polling. mlx5 supports 32 concurrent command
slots with workqueue-based completion. fdice_ adopts the mlx5 concurrency
model while preserving the E810 ring submission protocol.

#### Synchronous API (blocking)

```c
/**
 * fdice_aq_send_cmd - Submit an AQ command and wait for completion
 * @hw: Hardware instance
 * @desc: Descriptor with opcode and parameters filled in
 * @buf: DMA buffer for indirect commands (NULL for direct)
 * @buf_len: Length of indirect buffer
 *
 * Allocates a completion token, submits the descriptor to the send
 * queue, and blocks on a completion until the firmware responds or
 * the timeout expires. Returns translated fdice error code.
 *
 * Context: May sleep. Must not be called from atomic context.
 * Return: 0 on success, negative fdice_err on failure.
 */
int fdice_aq_send_cmd(struct fdice_hw *hw,
		      struct fdice_aq_desc *desc,
		      void *buf, u16 buf_len);
```

#### Asynchronous API (callback-based)

```c
/**
 * struct fdice_aq_callback - Async command completion context
 * @fn: Callback function invoked from workqueue on completion
 * @ctx: Opaque caller context passed to callback
 */
struct fdice_aq_callback {
	void (*fn)(struct fdice_hw *hw,
		   struct fdice_aq_desc *resp,
		   void *buf, int status, void *ctx);
	void *ctx;
};

/**
 * fdice_aq_send_cmd_async - Submit an AQ command with async completion
 * @hw: Hardware instance
 * @desc: Descriptor with opcode and parameters
 * @buf: DMA buffer for indirect commands (NULL for direct)
 * @buf_len: Length of indirect buffer
 * @cb: Callback to invoke on completion (from workqueue context)
 *
 * Submits the command and returns immediately. The callback is invoked
 * from the completion workqueue when the firmware responds. The caller
 * must not free @buf until the callback fires or the command is cancelled.
 *
 * Context: May be called from any context. Callback runs in workqueue.
 * Return: 0 if submitted successfully, negative on submission failure.
 */
int fdice_aq_send_cmd_async(struct fdice_hw *hw,
			    struct fdice_aq_desc *desc,
			    void *buf, u16 buf_len,
			    struct fdice_aq_callback *cb);
```

#### Token-Based Command Tracking

Each submitted command receives a token from an IDA allocator. The token
is stored in the descriptor's `cookie_high`/`cookie_low` fields, which
the firmware echoes back in the completion descriptor. This enables
matching completions to outstanding commands without serialization.

```c
/**
 * struct fdice_aq_pending - Pending command slot
 * @desc: Copy of submitted descriptor (for timeout reporting)
 * @buf: DMA buffer reference
 * @buf_len: Buffer length
 * @cb: Async callback (NULL for synchronous commands)
 * @completion: Kernel completion for synchronous waiters
 * @status: Completion status (written by completion handler)
 * @submit_jiffies: Timestamp for timeout detection
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
```

**Concurrency limits by queue type** (see Section 4):

| Queue Type | Max Concurrent | Rationale |
|-----------|---------------|-----------|
| Admin | 16 | Primary control path, moderate concurrency |
| Mailbox | 8 | PF-VF messaging, lower volume |
| Sideband | 4 | Sideband control, lowest volume |

These limits are conservative starting points. The E810 firmware does not
advertise a concurrency limit for the AQ ring, but the ring depth (typically
32-64 entries) provides a natural ceiling. The token allocator enforces
the software concurrency limit via `ida_alloc_max()`.

### 2.4 Completion Model

**Design rationale**: ice busy-waits on the head register after every
command submission, burning CPU cycles and preventing concurrency. fdice_
uses a workqueue-based completion model inspired by mlx5's
`mlx5_cmd_comp_handler()`.

#### Completion Flow

```
1. Command submitted: token allocated, descriptor written to SQ ring,
   tail register bumped, pending slot populated.

2. Firmware processes command: writes back descriptor with DD+CMP flags,
   updates head register.

3. Completion detection (two paths):
   a) Interrupt-driven: MSI-X vector for admin queue triggers
      fdice_aq_irq_handler(), which schedules completion work.
   b) Polling fallback: fdice_aq_poll_completions() called from
      periodic health check or timeout path.

4. Completion work (workqueue context):
   - Scan SQ from next_to_clean to head for DD-flagged descriptors
   - For each completed descriptor:
     - Extract token from cookie_high/cookie_low
     - Look up pending slot
     - Copy writeback descriptor and buffer to pending slot
     - Translate retval to fdice_err
     - Update per-opcode statistics
     - If async: invoke callback, free pending slot
     - If sync: set status, call complete()

5. Synchronous waiter wakes from wait_for_completion_timeout(),
   reads status and response data from pending slot, frees token.
```

#### Timeout Handling

A periodic timer (driven by the health subsystem) scans pending slots
for commands that have exceeded their timeout. Timed-out commands:

1. Are marked with `FDICE_ERR_TIMEOUT` status
2. Have their completion signaled (sync) or callback invoked (async)
3. Increment the per-opcode timeout counter in statistics
4. Trigger a warning log with opcode, token, and elapsed time
5. If timeout count exceeds threshold: escalate to health subsystem

### 2.5 Per-Opcode Statistics

Inspired by mlx5's per-opcode stats tracking via xarray:

```c
/**
 * struct fdice_aq_cmd_stats - Per-opcode command statistics
 * @count: Total commands submitted with this opcode
 * @success: Successful completions
 * @error: Error completions (any non-zero retval)
 * @timeout: Commands that timed out
 * @total_usec: Cumulative execution time in microseconds
 * @max_usec: Maximum single execution time
 * @last_err: Last error code seen for this opcode
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
```

Statistics are indexed by opcode in an xarray on `struct fdice_aq` and
exposed via debugfs at `fdice/<pci_slot>/aq_stats/`. This enables runtime
diagnosis of firmware responsiveness issues without requiring specialized
tools.

## 3. Command Abstraction Layer

### 3.1 Subsystem vtable: `struct fdice_hw_ops`

Instead of ice's flat namespace of `ice_aq_*` functions scattered across
multiple files, fdice_ groups AQ commands into subsystem objects accessible
through a vtable. This provides:

- Clear ownership of opcodes by subsystem
- Mockability for unit testing (swap vtable for test stubs)
- Future extensibility for E830 or other hardware variants

```c
/**
 * struct fdice_hw_ops - Hardware operation vtable grouped by subsystem
 *
 * Each subsystem provides type-safe wrappers around raw AQ commands.
 * The wrapper functions handle descriptor construction, buffer setup,
 * response parsing, and error translation. Callers never interact
 * with raw descriptors above this layer.
 */
struct fdice_hw_ops {
	/* Core / Resource Management */
	struct {
		int (*get_fw_ver)(struct fdice_hw *hw,
				  struct fdice_fw_ver *ver);
		int (*send_driver_ver)(struct fdice_hw *hw);
		int (*request_resource)(struct fdice_hw *hw,
					enum fdice_res_id res,
					enum fdice_res_access access,
					u32 timeout_ms);
		int (*release_resource)(struct fdice_hw *hw,
					enum fdice_res_id res);
		int (*get_func_caps)(struct fdice_hw *hw,
				     struct fdice_caps *caps);
		int (*get_dev_caps)(struct fdice_hw *hw,
				    struct fdice_caps *caps);
	} core;

	/* Internal Switch */
	struct {
		int (*get_config)(struct fdice_hw *hw,
				  struct fdice_sw_cfg *cfg);
		int (*set_port_params)(struct fdice_hw *hw,
				       struct fdice_port_params *params);
		int (*alloc_resources)(struct fdice_hw *hw,
				       struct fdice_sw_res_req *req,
				       struct fdice_sw_res *res);
		int (*free_resources)(struct fdice_hw *hw,
				      struct fdice_sw_res *res);
		int (*add_rules)(struct fdice_hw *hw,
				 struct fdice_sw_rule *rules,
				 u16 num_rules);
		int (*remove_rules)(struct fdice_hw *hw,
				    struct fdice_sw_rule *rules,
				    u16 num_rules);
	} sw;

	/* VSI Management */
	struct {
		int (*alloc)(struct fdice_hw *hw,
			     struct fdice_vsi_params *params,
			     u16 *vsi_id);
		int (*update)(struct fdice_hw *hw, u16 vsi_id,
			      struct fdice_vsi_params *params);
		int (*free)(struct fdice_hw *hw, u16 vsi_id);
	} vsi;

	/* PHY / Link */
	struct {
		int (*get_caps)(struct fdice_hw *hw,
				struct fdice_phy_caps *caps);
		int (*set_config)(struct fdice_hw *hw,
				  struct fdice_phy_cfg *cfg);
		int (*get_link_status)(struct fdice_hw *hw,
				       struct fdice_link_status *status);
		int (*restart_autoneg)(struct fdice_hw *hw);
		int (*set_event_mask)(struct fdice_hw *hw, u16 mask);
	} phy;

	/* NVM Management */
	struct {
		int (*read)(struct fdice_hw *hw, u16 module,
			    u32 offset, u16 len, void *buf);
		int (*write)(struct fdice_hw *hw, u16 module,
			     u32 offset, u16 len, const void *buf);
		int (*checksum)(struct fdice_hw *hw);
		int (*activate)(struct fdice_hw *hw, u8 flags);
	} nvm;

	/* Transmit Scheduler */
	struct {
		int (*get_default_topo)(struct fdice_hw *hw,
					struct fdice_sched_topo *topo);
		int (*add_elements)(struct fdice_hw *hw,
				    struct fdice_sched_elem *elems,
				    u16 num_elems);
		int (*delete_elements)(struct fdice_hw *hw,
				       u32 *teid_list, u16 num);
		int (*add_rl_profile)(struct fdice_hw *hw,
				      struct fdice_rl_profile *prof);
	} sched;

	/* LLDP / DCB */
	struct {
		int (*get_mib)(struct fdice_hw *hw,
			       struct fdice_lldp_mib *mib);
		int (*stop_agent)(struct fdice_hw *hw);
		int (*start_agent)(struct fdice_hw *hw);
		int (*set_mib_change_event)(struct fdice_hw *hw,
					    bool enable);
	} lldp;

	/* RSS */
	struct {
		int (*set_key)(struct fdice_hw *hw, u16 vsi_id,
			       const u8 *key, u16 key_len);
		int (*get_key)(struct fdice_hw *hw, u16 vsi_id,
			       u8 *key, u16 key_len);
		int (*set_lut)(struct fdice_hw *hw, u16 vsi_id,
			       const u8 *lut, u16 lut_len);
		int (*get_lut)(struct fdice_hw *hw, u16 vsi_id,
			       u8 *lut, u16 lut_len);
	} rss;
};
```

### 3.2 Type-Safe Wrappers

Each subsystem entry in `fdice_hw_ops` is backed by a concrete
implementation function. The wrapper handles:

1. **Descriptor construction** -- fills opcode, flags, parameters
2. **Buffer setup** -- allocates DMA buffer for indirect commands
3. **Submission** -- calls `fdice_aq_send_cmd()` or `_async()`
4. **Response parsing** -- extracts typed data from writeback
5. **Error translation** -- converts AQ retval to `enum fdice_err`

Example for link status:

```c
/**
 * fdice_hw_get_link_status - Query current link state
 * @hw: Hardware instance
 * @status: Output structure for link status data
 *
 * Issues opcode 0x0607 (get_link_status) and parses the indirect
 * response buffer into the caller's typed structure.
 *
 * Return: 0 on success, negative fdice_err on failure.
 */
int fdice_hw_get_link_status(struct fdice_hw *hw,
			     struct fdice_link_status *status)
{
	struct fdice_aq_desc desc = {};
	struct fdice_aqp_get_link_status *cmd;
	struct fdice_link_status_data *resp;
	int err;

	fdice_aq_prep_desc(&desc, FDICE_AQ_OPC_GET_LINK_STATUS);
	cmd = &desc.params.get_link_status;
	cmd->lport_num = hw->port_info->lport;

	resp = fdice_aq_alloc_buf(sizeof(*resp));
	if (!resp)
		return -FDICE_ERR_NOMEM;

	err = fdice_aq_send_cmd(hw, &desc, resp, sizeof(*resp));
	if (err)
		goto out_free;

	fdice_parse_link_status(resp, status);

out_free:
	fdice_aq_free_buf(resp);
	return err;
}
```

### 3.3 Error Translation

AQ firmware return codes (`retval` in the descriptor) are translated to
a fdice-specific error enum at the HAL boundary. Upper layers never see
raw firmware error codes.

```c
/**
 * enum fdice_err - Unified error codes for the fdice_ driver
 *
 * Negative values following kernel convention. The AQ retval is
 * preserved in the per-opcode stats for diagnostics.
 */
enum fdice_err {
	FDICE_ERR_OK		= 0,
	FDICE_ERR_PERM		= 1,    /* RC_EPERM */
	FDICE_ERR_NOENT		= 2,    /* RC_ENOENT */
	FDICE_ERR_BAD_OPCODE	= 3,    /* RC_ESRCH */
	FDICE_ERR_IO		= 5,    /* RC_EIO */
	FDICE_ERR_AGAIN		= 8,    /* RC_EAGAIN */
	FDICE_ERR_NOMEM		= 9,    /* RC_ENOMEM */
	FDICE_ERR_ACCESS	= 10,   /* RC_EACCES */
	FDICE_ERR_BUSY		= 12,   /* RC_EBUSY */
	FDICE_ERR_EXIST		= 13,   /* RC_EEXIST */
	FDICE_ERR_INVAL		= 14,   /* RC_EINVAL */
	FDICE_ERR_NOSPC		= 16,   /* RC_ENOSPC */
	FDICE_ERR_NOSYS		= 17,   /* RC_ENOSYS */
	FDICE_ERR_MODE		= 21,   /* RC_EMODE */
	FDICE_ERR_NOSEC		= 24,   /* RC_ENOSEC */
	FDICE_ERR_BADSIG	= 25,   /* RC_EBADSIG */
	FDICE_ERR_SVN		= 26,   /* RC_ESVN */
	FDICE_ERR_BADMAN	= 27,   /* RC_EBADMAN */
	FDICE_ERR_BADBUF	= 28,   /* RC_EBADBUF */

	/* Software-defined errors (above firmware range) */
	FDICE_ERR_TIMEOUT	= 100,  /* Command timed out */
	FDICE_ERR_RESET		= 101,  /* Device is resetting */
	FDICE_ERR_QUEUE_FULL	= 102,  /* No free command slots */
	FDICE_ERR_QUEUE_SHUT	= 103,  /* Queue is shut down */
};

/**
 * fdice_aq_retval_to_err - Translate AQ retval to fdice_err
 * @retval: Raw firmware return value from descriptor
 *
 * Maps firmware error codes to the fdice error enum. Unknown
 * firmware codes map to FDICE_ERR_IO as a catch-all.
 */
int fdice_aq_retval_to_err(u16 retval);

/**
 * fdice_err_to_errno - Translate fdice_err to standard errno
 * @err: fdice error code
 *
 * For kernel interfaces that require standard errno values.
 * Used at the boundary between HAL consumers and kernel subsystems.
 */
int fdice_err_to_errno(int err);
```

The two-stage translation (AQ retval -> fdice_err -> errno) keeps firmware
details out of upper layers while preserving diagnostic specificity within
the driver.

## 4. Control Queue Types

The E810 has three control queue types. All use identical ring mechanics
and the same `struct fdice_aq` infrastructure, differing only in purpose,
register offsets, and buffer sizes.

| Property | Admin Queue | Mailbox Queue | Sideband Queue |
|----------|-------------|---------------|----------------|
| Purpose | Primary FW command interface | PF-VF communication | Sideband control channel |
| SQ entries | 32 | 32 | 16 |
| RQ entries | 32 | 32 | 16 |
| Max buf size | 4096 | 4096 | 512 |
| Max concurrent | 16 | 8 | 4 |
| MSI-X vector | Dedicated admin vector | Shared or dedicated | Shared |
| Register set | PF_FW_ATQBAL/H/LEN/T, PF_FW_ARQBAL/H/LEN/T | PF_MBX_ATQBAL/H/LEN/T, PF_MBX_ARQBAL/H/LEN/T | PF_SB_ATQBAL/H/LEN/T, PF_SB_ARQBAL/H/LEN/T |

### 4.1 Queue Initialization

Each control queue is initialized through the same sequence:

```
fdice_ctlq_init(hw, type)
  1. Allocate fdice_aq structure
  2. Set register offsets based on queue type
  3. Set buffer sizes and entry counts based on queue type
  4. Allocate descriptor ring: dmam_alloc_coherent (SQ + RQ)
  5. Allocate per-descriptor DMA buffers for RQ (pre-populated)
  6. Initialize token IDA and pending slots array
  7. Create completion workqueue
  8. Initialize statistics xarray
  9. Write base address registers (BAL/BAH) for SQ and RQ
  10. Write length registers with enable bit to activate queues
  11. Set initialized = true
```

### 4.2 Queue Shutdown

```
fdice_ctlq_shutdown(hw, type)
  1. Set initialized = false (reject new submissions)
  2. Cancel all pending commands (invoke callbacks with FDICE_ERR_QUEUE_SHUT)
  3. Drain completion workqueue
  4. Write length register with enable bit cleared
  5. Poll for head == tail (queue drained) with timeout
  6. Destroy statistics xarray
  7. Destroy token IDA
  8. Free pending slots array
  9. Free RQ per-descriptor buffers
  10. Free descriptor rings (devres-managed, freed automatically)
```

**Critical ordering**: Pending commands must be cancelled (step 2) before
the queue is disabled (step 4) and before DMA memory is freed. This avoids
the race condition identified in ice where `ice_aq_cancel_waiting_tasks`
is called after `ice_destroy_all_ctrlq` (Exploration 03 §ice Weaknesses #3).

## 5. DMA Strategy

### 5.1 Control Queue DMA (Managed)

Control queue descriptor rings and receive queue buffers use
`dmam_alloc_coherent()` (devres-managed). This ensures cleanup on driver
removal or probe failure without explicit free calls in error paths.

```c
/* Descriptor ring allocation -- devres-managed */
ring->desc = dmam_alloc_coherent(dev, ring_size,
				 &ring->desc_dma, GFP_KERNEL);

/* RQ pre-populated buffers -- devres-managed */
buf->va = dmam_alloc_coherent(dev, buf_size,
			      &buf->dma, GFP_KERNEL);
```

**Rationale**: Control queues are allocated once during probe and freed at
remove. Their lifecycle matches the device lifetime, making devres the
correct choice. This eliminates an entire class of DMA leak bugs in error
paths.

### 5.2 Send Queue Indirect Buffers (Explicit)

Send queue indirect command buffers are allocated per-command and freed
after completion. These use explicit `dma_alloc_coherent()` /
`dma_free_coherent()` because their lifecycle is per-command, not
per-device.

```c
/* Per-command buffer allocation */
buf = dma_alloc_coherent(dev, len, &dma_addr, GFP_KERNEL);

/* Freed in completion handler after response is copied out */
dma_free_coherent(dev, len, buf, dma_addr);
```

### 5.3 Data Path DMA (Explicit, Future)

Tx/Rx ring descriptors and data buffers for the data path will use
explicit DMA allocation with careful lifecycle management. This is
outside the HAL scope and will be specified in the queue architecture
document.

### 5.4 DMA Addressing

The E810 supports 64-bit DMA addresses. The driver sets a 64-bit DMA
mask during PCI initialization:

```c
err = dma_set_mask_and_coherent(&pdev->dev, DMA_BIT_MASK(64));
```

All DMA addresses in AQ descriptors are split into `addr_high` (upper 32
bits) and `addr_low` (lower 32 bits), matching the descriptor parameter
layout.

## 6. Init/Teardown Integration

The HAL fits into the 5-layer init model defined in Explorations 01 and
03. The AQ is the first hardware subsystem initialized and the last
destroyed, because all other hardware configuration depends on it.

### 6.1 Init Sequence (HAL Perspective)

```
Layer 0: fdice_mdev_init()
  └─ Allocates fdice_hw struct, initializes AQ software state
     (token IDA, pending array, statistics xarray, workqueue)
     No hardware access yet.

Layer 1: fdice_pci_init()
  └─ Maps BAR0, sets DMA mask. AQ register offsets become valid.

Layer 2: fdice_function_setup()
  ├─ fdice_ctlq_init(hw, FDICE_CTL_Q_ADMIN)
  │    Allocates DMA rings, writes registers, enables admin queue.
  │    AQ is now operational.
  ├─ fdice_hw_get_fw_ver()         [first AQ command]
  ├─ fdice_hw_send_driver_ver()
  ├─ fdice_hw_get_dev_caps()       [capability discovery]
  ├─ fdice_hw_get_func_caps()
  ├─ fdice_init_port_info()
  ├─ fdice_ctlq_init(hw, FDICE_CTL_Q_MAILBOX)  [if VFs supported]
  └─ fdice_ctlq_init(hw, FDICE_CTL_Q_SIDEBAND) [if sideband needed]

Layer 3: fdice_init_once()
  └─ Allocates VSI table, scheduler tree, filter structures.
     Uses AQ for hardware table initialization.

Layer 4: fdice_load()
  └─ Creates IRQs, event queues, flow steering.
     Uses AQ for runtime hardware configuration.
```

### 6.2 Teardown Sequence (HAL Perspective)

Teardown proceeds in exact reverse order, with the critical constraint
that AQ waiters are cancelled before queues are destroyed (R4 from
Exploration 03).

```
Layer 4: fdice_unload()
  └─ Tears down runtime subsystems. Final AQ commands issued here
     (e.g., disable scheduler elements, remove switch rules).

Layer 3: fdice_cleanup_once()
  └─ Frees VSI table, scheduler tree, filter structures.

Layer 2: fdice_hw_teardown()
  ├─ Cancel all pending AQ commands on all queues
  ├─ fdice_ctlq_shutdown(hw, FDICE_CTL_Q_SIDEBAND)
  ├─ fdice_ctlq_shutdown(hw, FDICE_CTL_Q_MAILBOX)
  └─ fdice_ctlq_shutdown(hw, FDICE_CTL_Q_ADMIN)  [last queue down]

Layer 1: fdice_pci_cleanup()
  └─ Unmaps BAR, releases PCI resources. Devres frees DMA.

Layer 0: fdice_mdev_uninit()
  └─ Destroys AQ software state (IDA, xarrays, workqueue).
```

### 6.3 State Tracking

The HAL uses a state bitmask to guard operations and ensure safe teardown
of partially-initialized devices (adopting mlx5's pattern from
Exploration 03 §R7):

```c
enum fdice_hw_state {
	FDICE_HW_AQ_SW_INIT	= BIT(0),  /* AQ software state allocated */
	FDICE_HW_AQ_ENABLED	= BIT(1),  /* Admin queue operational */
	FDICE_HW_MBX_ENABLED	= BIT(2),  /* Mailbox queue operational */
	FDICE_HW_SB_ENABLED	= BIT(3),  /* Sideband queue operational */
	FDICE_HW_CAPS_LOADED	= BIT(4),  /* Capabilities discovered */
	FDICE_HW_REMOVING	= BIT(5),  /* Device removal in progress */
};
```

Teardown functions check state bits before acting, making cleanup
idempotent and safe for partial init failures.

## 7. Key Structs Summary

### 7.1 `struct fdice_hw` -- Top-Level Hardware Abstraction

```c
/**
 * struct fdice_hw - Hardware abstraction for one E810 function
 * @hw_addr: MMIO base address (BAR0)
 * @pdev: PCI device backpointer
 * @adminq: Admin Queue instance
 * @mbxq: Mailbox Queue instance (PF-VF communication)
 * @sbq: Sideband Queue instance
 * @ops: Hardware operation vtable
 * @port_info: Port configuration and state
 * @caps: Discovered device/function capabilities
 * @fw_ver: Firmware version info
 * @state: Hardware state bitmask (enum fdice_hw_state)
 * @reset_ongoing: Set during device reset
 * @vendor_id: PCI vendor ID
 * @device_id: PCI device ID
 * @subsystem_vendor_id: PCI subsystem vendor ID
 * @subsystem_device_id: PCI subsystem device ID
 * @revision_id: PCI revision ID
 * @bus: PCI bus info string
 * @mac_type: MAC type (E810, E830, etc.)
 */
struct fdice_hw {
	/* MMIO and PCI */
	void __iomem		*hw_addr;
	struct pci_dev		*pdev;

	/* Control queues */
	struct fdice_aq		adminq;
	struct fdice_aq		mbxq;
	struct fdice_aq		sbq;

	/* Command abstraction */
	const struct fdice_hw_ops *ops;

	/* Device state */
	struct fdice_port_info	*port_info;
	struct fdice_caps	caps;
	struct fdice_fw_ver	fw_ver;
	unsigned long		state;
	bool			reset_ongoing;

	/* PCI identity */
	u16			vendor_id;
	u16			device_id;
	u16			subsystem_vendor_id;
	u16			subsystem_device_id;
	u8			revision_id;
	char			bus[16];
	enum fdice_mac_type	mac_type;
};
```

### 7.2 Struct Relationship Diagram

```
struct fdice_dev (core device, devlink-embedded)
  └── struct fdice_hw
        ├── struct fdice_aq adminq
        │     ├── struct fdice_aq_ring sq
        │     │     ├── struct fdice_aq_desc[] (DMA, hardware-fixed)
        │     │     └── struct fdice_aq_buf[]
        │     ├── struct fdice_aq_ring rq
        │     │     ├── struct fdice_aq_desc[] (DMA, hardware-fixed)
        │     │     └── struct fdice_aq_buf[]
        │     ├── struct fdice_aq_pending[] (token-indexed)
        │     ├── struct ida tokens
        │     ├── struct xarray stats -> struct fdice_aq_cmd_stats
        │     └── struct workqueue_struct *completion_wq
        ├── struct fdice_aq mbxq (same structure)
        ├── struct fdice_aq sbq (same structure)
        ├── const struct fdice_hw_ops *ops
        │     ├── .core  (get_fw_ver, get_caps, ...)
        │     ├── .sw    (add_rules, alloc_resources, ...)
        │     ├── .vsi   (alloc, update, free)
        │     ├── .phy   (get_caps, get_link_status, ...)
        │     ├── .nvm   (read, write, checksum, ...)
        │     ├── .sched (add_elements, delete_elements, ...)
        │     ├── .lldp  (get_mib, stop/start_agent, ...)
        │     └── .rss   (set/get_key, set/get_lut)
        ├── struct fdice_caps
        ├── struct fdice_port_info
        └── struct fdice_fw_ver
```

## 8. Design Decisions Summary

| Decision | Choice | Rationale |
|----------|--------|-----------|
| Completion model | Workqueue + callbacks | Avoids ice's busy-wait CPU waste; enables async ops |
| Concurrency | Token-based, 16 slots | mlx5 proves multi-slot works; E810 ring depth allows it |
| Command grouping | Subsystem vtable | Cleaner than flat namespace; enables mock testing |
| Error handling | Two-stage translation | Preserves firmware specificity; hides it from upper layers |
| Control queue DMA | dmam_alloc_coherent | Lifecycle matches device; eliminates leak class |
| Command buffer DMA | Explicit alloc/free | Per-command lifecycle; cannot use devres |
| Statistics | Per-opcode xarray | Debuggability; mlx5 proves operational value |
| Descriptor format | Byte-identical to libie | Hardware protocol; no choice |
| Queue shutdown order | Cancel waiters first | Fixes ice race condition (Exploration 03) |
| State tracking | Bitmask per fdice_hw | Safe partial teardown; idempotent cleanup |
| Retry policy | Per-opcode configurable | More flexible than ice's hardcoded EBUSY retry |
| Event dispatch | Registered handler table | Extensible; avoids monolithic switch statement |
