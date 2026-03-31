// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE PTP Hardware Layer
 *
 * E810-specific PTP timer operations: register reads/writes, hardware
 * semaphore management, and TX timestamp extraction.  All register access
 * goes through MMIO helpers and is serialized by the PFTSYN hardware
 * semaphore where required.
 *
 * The design separates hardware register mechanics (this file) from PTP
 * subsystem logic (fdice_ptp.c), allowing future E830 support by adding
 * an alternative hw init function without touching the core PTP code.
 *
 * Architecture reference: docs/exploration/13-ptp-clock-synchronization.md
 */

#include <linux/types.h>
#include <linux/io.h>
#include <linux/delay.h>
#include <linux/bitfield.h>
#include <linux/ptp_clock_kernel.h>

#include "fdice_dev.h"
#include "fdice_ptp_hw.h"

/* -----------------------------------------------------------------------
 * MMIO Helpers (local to this translation unit)
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_rd32 - Read a 32-bit PTP register
 * @hw: hardware instance
 * @reg: register offset from BAR0
 *
 * Return: The 32-bit register value.
 */
static inline u32 fdice_ptp_rd32(struct fdice_hw *hw, u32 reg)
{
	return readl(hw->hw_addr + reg);
}

/**
 * fdice_ptp_wr32 - Write a 32-bit PTP register
 * @hw: hardware instance
 * @reg: register offset from BAR0
 * @val: value to write
 */
static inline void fdice_ptp_wr32(struct fdice_hw *hw, u32 reg, u32 val)
{
	writel(val, hw->hw_addr + reg);
}

/* -----------------------------------------------------------------------
 * Hardware Semaphore
 *
 * The E810 uses a per-PF semaphore register (PFTSYN_SEM) to arbitrate
 * access to the shared source timer across multiple physical functions.
 * Each PF's semaphore lives at PFTSYN_SEM + (pf_id * 4).
 *
 * The acquire path retries with a 5-10 ms sleep between attempts, which
 * mirrors the ice approach but consolidates the retry logic into a single
 * bounded loop with clear timeout semantics.
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_hw_sem_acquire - Acquire the PTP hardware semaphore
 * @hw: hardware instance
 *
 * Attempts to acquire the per-PF PTP hardware semaphore.  The semaphore
 * register is at a PF-specific offset and uses bit 0 as the busy flag.
 * Retries up to FDICE_PTP_SEM_MAX_RETRIES with a 5 ms sleep between
 * each attempt.
 *
 * Return: true if the semaphore was acquired, false on timeout.
 */
bool fdice_ptp_hw_sem_acquire(struct fdice_hw *hw)
{
	u32 sem_reg = FDICE_PFTSYN_SEM +
		      (FDICE_PFTSYN_SEM_STRIDE * hw->pf_id);
	unsigned int retries;
	u32 val;

	for (retries = 0; retries < FDICE_PTP_SEM_MAX_RETRIES; retries++) {
		val = fdice_ptp_rd32(hw, sem_reg);
		if (!(val & FDICE_PFTSYN_SEM_BUSY_M))
			return true;
		usleep_range(5000, 6000);
	}

	return false;
}

/**
 * fdice_ptp_hw_sem_release - Release the PTP hardware semaphore
 * @hw: hardware instance
 *
 * Clears the per-PF PTP hardware semaphore by writing zero to the
 * register.  Must only be called by the PF that currently holds it.
 */
void fdice_ptp_hw_sem_release(struct fdice_hw *hw)
{
	u32 sem_reg = FDICE_PFTSYN_SEM +
		      (FDICE_PFTSYN_SEM_STRIDE * hw->pf_id);

	fdice_ptp_wr32(hw, sem_reg, 0);
}

/* -----------------------------------------------------------------------
 * Timer Command Execution
 *
 * The E810 timer is controlled by writing a command encoding to
 * GLTSYN_CMD, then triggering synchronous execution by writing to
 * GLTSYN_CMD_SYNC.  The caller must hold the hardware semaphore.
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_hw_exec_cmd - Execute a timer command
 * @hw: hardware instance
 * @cmd: timer command to execute
 *
 * Translates the abstract timer command into the E810 GLTSYN_CMD bit
 * encoding, writes it, and triggers execution via GLTSYN_CMD_SYNC.
 * The caller must hold the PTP hardware semaphore.
 *
 * Return: 0 on success, -EINVAL for unknown commands.
 */
int fdice_ptp_hw_exec_cmd(struct fdice_hw *hw, enum fdice_ptp_tmr_cmd cmd)
{
	u32 cmd_val;

	switch (cmd) {
	case FDICE_PTP_TMR_INIT_TIME:
		cmd_val = FDICE_GLTSYN_CMD_INIT_TIME;
		break;
	case FDICE_PTP_TMR_INIT_INCVAL:
		cmd_val = FDICE_GLTSYN_CMD_INIT_INCVAL;
		break;
	case FDICE_PTP_TMR_ADJ_TIME:
		cmd_val = FDICE_GLTSYN_CMD_ADJ_TIME;
		break;
	case FDICE_PTP_TMR_READ_TIME:
		cmd_val = FDICE_GLTSYN_CMD_READ_TIME;
		break;
	case FDICE_PTP_TMR_NOP:
		return 0;
	default:
		return -EINVAL;
	}

	fdice_ptp_wr32(hw, FDICE_GLTSYN_CMD, cmd_val);
	fdice_ptp_wr32(hw, FDICE_GLTSYN_CMD_SYNC, FDICE_PTP_SYNC_EXEC);

	return 0;
}

/* -----------------------------------------------------------------------
 * Timer Read / Write / Adjust Operations
 *
 * All three operations follow the same pattern:
 *   1. Caller holds the semaphore.
 *   2. Stage data into shadow registers (for write/adjust).
 *   3. Issue the appropriate timer command.
 *   4. Read result from time registers (for read).
 *
 * The timer index is always 0 for the source timer owner, which is the
 * only path FrankenDrICE currently supports for E810.
 * ----------------------------------------------------------------------- */

/** Timer index for the source timer owner */
#define FDICE_PTP_TMR_IDX	0

/**
 * fdice_ptp_hw_read_time - Read the current PHC time
 * @hw: hardware instance
 * @time_ns: output pointer for the 64-bit nanosecond timestamp
 * @sts: optional system timestamp correlation structure for
 *       PTP_SYS_OFFSET_EXTENDED support; may be NULL
 *
 * Issues a READ_TIME command to snapshot the current timer value into
 * the GLTSYN_TIME registers, then assembles the 64-bit result.  If
 * @sts is provided, captures system timestamps before and after the
 * hardware read for cross-timestamp correlation.
 *
 * The caller must hold the PTP hardware semaphore.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_hw_read_time(struct fdice_hw *hw, u64 *time_ns,
			   struct ptp_system_timestamp *sts)
{
	u32 lo, hi;
	int err;

	if (sts)
		ptp_read_system_prets(sts);

	err = fdice_ptp_hw_exec_cmd(hw, FDICE_PTP_TMR_READ_TIME);
	if (err)
		return err;

	if (sts)
		ptp_read_system_postts(sts);

	lo = fdice_ptp_rd32(hw, FDICE_GLTSYN_TIME_L(FDICE_PTP_TMR_IDX));
	hi = fdice_ptp_rd32(hw, FDICE_GLTSYN_TIME_H(FDICE_PTP_TMR_IDX));

	*time_ns = ((u64)hi << 32) | lo;

	return 0;
}

/**
 * fdice_ptp_hw_write_time - Set the PHC to an absolute time
 * @hw: hardware instance
 * @time_ns: 64-bit nanosecond value to set
 *
 * Stages the time value into the shadow registers (GLTSYN_SHTIME_L/H)
 * and issues an INIT_TIME command to load it into the running timer.
 *
 * The caller must hold the PTP hardware semaphore.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_hw_write_time(struct fdice_hw *hw, u64 time_ns)
{
	fdice_ptp_wr32(hw, FDICE_GLTSYN_SHTIME_L(FDICE_PTP_TMR_IDX),
		       lower_32_bits(time_ns));
	fdice_ptp_wr32(hw, FDICE_GLTSYN_SHTIME_H(FDICE_PTP_TMR_IDX),
		       upper_32_bits(time_ns));
	/* SHTIME_0 must be written but its value is unused for INIT_TIME */
	fdice_ptp_wr32(hw, FDICE_GLTSYN_SHTIME_0(FDICE_PTP_TMR_IDX), 0);

	return fdice_ptp_hw_exec_cmd(hw, FDICE_PTP_TMR_INIT_TIME);
}

/**
 * fdice_ptp_hw_adj_time - Adjust the PHC by a signed offset
 * @hw: hardware instance
 * @adj_ns: signed 32-bit nanosecond adjustment
 *
 * Stages the adjustment value into GLTSYN_SHTIME_L (sign-extended into
 * the 32-bit register) and issues an ADJ_TIME command.
 *
 * The caller must hold the PTP hardware semaphore.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_hw_adj_time(struct fdice_hw *hw, s32 adj_ns)
{
	fdice_ptp_wr32(hw, FDICE_GLTSYN_SHTIME_L(FDICE_PTP_TMR_IDX),
		       (u32)adj_ns);

	return fdice_ptp_hw_exec_cmd(hw, FDICE_PTP_TMR_ADJ_TIME);
}

/* -----------------------------------------------------------------------
 * Increment Value Management
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_hw_write_incval - Write the timer increment value
 * @hw: hardware instance
 * @incval: 40-bit increment value (upper 8 bits in INCVAL_H)
 *
 * Stages the increment value into the GLTSYN_INCVAL shadow registers
 * and issues an INIT_INCVAL command.  The increment value controls the
 * timer tick rate: a larger value makes the clock run faster.
 *
 * The caller must hold the PTP hardware semaphore.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_hw_write_incval(struct fdice_hw *hw, u64 incval)
{
	fdice_ptp_wr32(hw, FDICE_GLTSYN_INCVAL_L(FDICE_PTP_TMR_IDX),
		       lower_32_bits(incval));
	fdice_ptp_wr32(hw, FDICE_GLTSYN_INCVAL_H(FDICE_PTP_TMR_IDX),
		       upper_32_bits(incval));

	return fdice_ptp_hw_exec_cmd(hw, FDICE_PTP_TMR_INIT_INCVAL);
}

/* -----------------------------------------------------------------------
 * TX Timestamp Register Access
 *
 * E810 stores per-port TX timestamps in dedicated PHY memory banks.
 * Each port has 64 timestamp entries, each occupying 8 bytes (lo + hi).
 * The registers are accessed via sideband MMIO in the PHY address space.
 *
 * NOTE: For a full implementation, E810 PHY register access requires
 * sideband queue (SBQ) commands.  This implementation uses direct MMIO
 * as a placeholder; production code would route through the SBQ path
 * (fdice_aq.c) for actual PHY register reads.
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_hw_read_tx_tstamp - Read a TX timestamp from the PHY
 * @hw: hardware instance
 * @port: physical port number
 * @idx: timestamp index within the port's block (0..63)
 * @tstamp: output pointer for the 64-bit raw timestamp
 *
 * Reads the low and high 32-bit halves of the TX timestamp register
 * for the given port and index, then assembles the 64-bit result.
 *
 * Return: 0 on success, -EINVAL for out-of-range parameters.
 */
int fdice_ptp_hw_read_tx_tstamp(struct fdice_hw *hw, u8 port, u8 idx,
				u64 *tstamp)
{
	u32 reg_lo, reg_hi;
	u32 lo, hi;

	if (idx >= FDICE_E810_TS_BLOCK_SIZE)
		return -EINVAL;

	reg_lo = FDICE_E810_TS_EXT(FDICE_E810_LOW_TX_MEM_START, port, idx);
	reg_hi = reg_lo + 4;

	lo = fdice_ptp_rd32(hw, reg_lo);
	hi = fdice_ptp_rd32(hw, reg_hi);

	*tstamp = ((u64)hi << 32) | lo;

	return 0;
}

/* -----------------------------------------------------------------------
 * E810 Hardware Initialization
 * ----------------------------------------------------------------------- */

/**
 * fdice_ptp_hw_init_e810 - Initialize E810 PTP hardware capabilities
 * @hw: hardware instance
 * @caps: output structure to populate with detected capabilities
 *
 * Detects E810-specific PTP hardware capabilities and populates the
 * capability descriptor.  Does NOT touch timer registers -- that is
 * deferred to fdice_ptp_init() which handles the full initialization
 * sequence including semaphore acquisition, increment value programming,
 * and initial time setting.
 *
 * Return: 0 on success, negative errno on failure.
 */
int fdice_ptp_hw_init_e810(struct fdice_hw *hw,
			   struct fdice_ptp_hw_caps *caps)
{
	if (hw->mac_type != FDICE_MAC_E810)
		return -ENODEV;

	caps->ts_block_per_port = FDICE_E810_TS_BLOCK_SIZE;
	caps->max_adj_ppb = 100000000; /* 100 MHz max frequency adjustment */
	caps->nominal_incval = FDICE_E810_NOMINAL_INCVAL;
	caps->ptp_pins_max = 6;

	return 0;
}
