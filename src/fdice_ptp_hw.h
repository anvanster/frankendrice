// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_PTP_HW_H_
#define _FDICE_PTP_HW_H_

/**
 * DOC: FrankenDrICE PTP Hardware Abstraction Header
 *
 * Defines E810 PTP register offsets, timer command encodings, and hardware-
 * level function declarations.  The register addresses come from the E810
 * datasheet; the abstraction layer hides per-MAC differences so that the
 * core PTP logic in fdice_ptp.c remains hardware-agnostic.
 *
 * Architecture reference: docs/exploration/13-ptp-clock-synchronization.md
 */

#include <linux/bits.h>
#include <linux/types.h>

/* Forward declarations */
struct fdice_hw;
struct ptp_system_timestamp;

/* -----------------------------------------------------------------------
 * E810 Global Timestamp Synchronization (GLTSYN) Register Offsets
 *
 * These are hardware-fixed addresses from the E810 datasheet.
 * The timer index (_i) selects among the per-PF timer instances.
 * ----------------------------------------------------------------------- */

/* Timer command and synchronization */
#define FDICE_GLTSYN_CMD		0x00088810
#define FDICE_GLTSYN_CMD_SYNC		0x00088814

/* Timer command bit encodings */
#define FDICE_GLTSYN_CMD_INIT_TIME	BIT(0)
#define FDICE_GLTSYN_CMD_INIT_INCVAL	BIT(1)
#define FDICE_GLTSYN_CMD_INIT_TIME_INCVAL (BIT(0) | BIT(1))
#define FDICE_GLTSYN_CMD_ADJ_TIME	BIT(2)
#define FDICE_GLTSYN_CMD_ADJ_INIT_TIME	(BIT(2) | BIT(3))
#define FDICE_GLTSYN_CMD_READ_TIME	BIT(7)

/* Synchronous execution trigger value for GLTSYN_CMD_SYNC */
#define FDICE_PTP_SYNC_EXEC		0x3

/* Timer enable */
#define FDICE_GLTSYN_ENA(_i)		(0x00088808 + ((_i) * 4))
#define FDICE_GLTSYN_ENA_TSYN_ENA_M	BIT(0)

/* Increment value registers (determines timer tick rate) */
#define FDICE_GLTSYN_INCVAL_L(_i)	(0x00088918 + ((_i) * 4))
#define FDICE_GLTSYN_INCVAL_H(_i)	(0x00088920 + ((_i) * 4))

/* Shadow time registers (staging area for atomic read/write) */
#define FDICE_GLTSYN_SHTIME_0(_i)	(0x000888E0 + ((_i) * 4))
#define FDICE_GLTSYN_SHTIME_L(_i)	(0x000888E8 + ((_i) * 4))
#define FDICE_GLTSYN_SHTIME_H(_i)	(0x000888F0 + ((_i) * 4))

/* Current timer value registers (read-only snapshot after READ_TIME cmd) */
#define FDICE_GLTSYN_TIME_0(_i)		(0x000888C8 + ((_i) * 4))
#define FDICE_GLTSYN_TIME_L(_i)		(0x000888D0 + ((_i) * 4))
#define FDICE_GLTSYN_TIME_H(_i)		(0x000888D8 + ((_i) * 4))

/* Timer status */
#define FDICE_GLTSYN_STAT(_i)		(0x000888C0 + ((_i) * 4))

/* Synchronization delay */
#define FDICE_GLTSYN_SYNC_DLAY		0x00088818

/* -----------------------------------------------------------------------
 * Per-Function PTP Semaphore (PFTSYN)
 *
 * Hardware semaphore for atomicity across PFs sharing the source timer.
 * Each PF has its own 4-byte semaphore register at a fixed stride.
 * ----------------------------------------------------------------------- */
#define FDICE_PFTSYN_SEM		0x00088880
#define FDICE_PFTSYN_SEM_STRIDE		4
#define FDICE_PFTSYN_SEM_BUSY_M		BIT(0)

/* Maximum retries when acquiring the PTP hardware semaphore */
#define FDICE_PTP_SEM_MAX_RETRIES	100

/* -----------------------------------------------------------------------
 * E810 PHY Timestamp Registers
 *
 * E810 uses sideband access (via PHY register read/write AQ commands) for
 * per-port TX timestamps.  Each port has a dedicated block of 64 timestamp
 * registers.
 * ----------------------------------------------------------------------- */
#define FDICE_E810_ETH_GLTSYN_CMD	0x03000344
#define FDICE_E810_LOW_TX_MEM_START	0x03090000
#define FDICE_E810_TS_BLOCK_SIZE	64

/**
 * FDICE_E810_TS_EXT - Calculate PHY timestamp register address
 * @base: base address of the TX timestamp memory bank
 * @port: physical port number
 * @idx: timestamp index within the port block
 *
 * Each port has its own 0x1000-byte region, with 8-byte entries per index.
 */
#define FDICE_E810_TS_EXT(base, port, idx) \
	((base) + (0x1000 * (port)) + ((idx) * 8))

/* -----------------------------------------------------------------------
 * GPIO / Periodic Output / External Timestamp Registers
 * ----------------------------------------------------------------------- */
#define FDICE_GLTSYN_AUX_IN_0(_i)	(0x000889D8 + ((_i) * 4))
#define FDICE_GLTSYN_AUX_OUT_0(_i)	(0x00088998 + ((_i) * 4))
#define FDICE_GLTSYN_CLKO_0(_i)		(0x000889B8 + ((_i) * 4))
#define FDICE_GLTSYN_TGT_L_0(_i)	(0x00088928 + ((_i) * 4))
#define FDICE_GLTSYN_TGT_H_0(_i)	(0x00088930 + ((_i) * 4))
#define FDICE_GLTSYN_EVNT_L_0(_i)	(0x00088968 + ((_i) * 4))
#define FDICE_GLTSYN_EVNT_H_0(_i)	(0x00088970 + ((_i) * 4))

/* Per-channel register helpers (channel 0..3, timer index 0..n) */
#define FDICE_GLTSYN_AUX_OUT(chan, idx) \
	(FDICE_GLTSYN_AUX_OUT_0(idx) + ((chan) * 8))
#define FDICE_GLTSYN_AUX_IN(chan, idx) \
	(FDICE_GLTSYN_AUX_IN_0(idx) + ((chan) * 8))
#define FDICE_GLTSYN_CLKO(chan, idx) \
	(FDICE_GLTSYN_CLKO_0(idx) + ((chan) * 8))
#define FDICE_GLTSYN_TGT_L(chan, idx) \
	(FDICE_GLTSYN_TGT_L_0(idx) + ((chan) * 16))
#define FDICE_GLTSYN_TGT_H(chan, idx) \
	(FDICE_GLTSYN_TGT_H_0(idx) + ((chan) * 16))
#define FDICE_GLTSYN_EVNT_L(chan, idx) \
	(FDICE_GLTSYN_EVNT_L_0(idx) + ((chan) * 16))
#define FDICE_GLTSYN_EVNT_H(chan, idx) \
	(FDICE_GLTSYN_EVNT_H_0(idx) + ((chan) * 16))

/* Auxiliary output control bits */
#define FDICE_GLTSYN_AUX_OUT_ENA_M	BIT(0)
#define FDICE_GLTSYN_AUX_OUT_OUTMOD_M	GENMASK(2, 1)
/* Auxiliary input control bits */
#define FDICE_GLTSYN_AUX_IN_INT_ENA_M	BIT(4)

/* Maximum channels for periodic output and external timestamps */
#define FDICE_PTP_TGT_MAX_CHANNELS	4
#define FDICE_PTP_EVNT_MAX_CHANNELS	3

/* -----------------------------------------------------------------------
 * Timer Command Encodings
 * ----------------------------------------------------------------------- */

/**
 * enum fdice_ptp_tmr_cmd - Timer command types for GLTSYN_CMD
 * @FDICE_PTP_TMR_INIT_TIME: Load shadow time registers into timer
 * @FDICE_PTP_TMR_INIT_INCVAL: Load shadow increment value into timer
 * @FDICE_PTP_TMR_ADJ_TIME: Adjust timer by signed offset
 * @FDICE_PTP_TMR_READ_TIME: Snapshot current time into read registers
 * @FDICE_PTP_TMR_NOP: No operation
 */
enum fdice_ptp_tmr_cmd {
	FDICE_PTP_TMR_INIT_TIME,
	FDICE_PTP_TMR_INIT_INCVAL,
	FDICE_PTP_TMR_ADJ_TIME,
	FDICE_PTP_TMR_READ_TIME,
	FDICE_PTP_TMR_NOP,
};

/* -----------------------------------------------------------------------
 * E810 PTP Hardware Capabilities
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_ptp_hw_caps - Per-MAC PTP hardware capability descriptor
 * @ts_block_per_port: Number of TX timestamp entries per port
 * @max_adj_ppb: Maximum frequency adjustment in parts-per-billion
 * @nominal_incval: Default timer increment for 1ns ticks
 * @ptp_pins_max: Maximum configurable GPIO pins
 *
 * Populated during fdice_ptp_hw_init_e810() to describe what the
 * hardware supports.
 */
struct fdice_ptp_hw_caps {
	u8	ts_block_per_port;
	u32	max_adj_ppb;
	u64	nominal_incval;
	u8	ptp_pins_max;
};

/* -----------------------------------------------------------------------
 * E810 PTP Nominal Increment Value
 *
 * For E810 the TSPLL runs at 812.5 MHz.  The nominal increment value
 * produces exactly 1 ns per tick: incval = 2^40 / 812500000 truncated.
 * This constant is derived from the E810 datasheet PLL specification
 * and validated against the ice driver's ice_base_incval() result for
 * the E810 PLL frequency.
 * ----------------------------------------------------------------------- */
#define FDICE_E810_NOMINAL_INCVAL	0x13b13b13bULL

/* -----------------------------------------------------------------------
 * Hardware-Level Function Declarations
 * ----------------------------------------------------------------------- */

int fdice_ptp_hw_init_e810(struct fdice_hw *hw,
			   struct fdice_ptp_hw_caps *caps);

bool fdice_ptp_hw_sem_acquire(struct fdice_hw *hw);
void fdice_ptp_hw_sem_release(struct fdice_hw *hw);

int fdice_ptp_hw_read_time(struct fdice_hw *hw, u64 *time_ns,
			   struct ptp_system_timestamp *sts);
int fdice_ptp_hw_write_time(struct fdice_hw *hw, u64 time_ns);
int fdice_ptp_hw_adj_time(struct fdice_hw *hw, s32 adj_ns);

int fdice_ptp_hw_write_incval(struct fdice_hw *hw, u64 incval);

int fdice_ptp_hw_read_tx_tstamp(struct fdice_hw *hw, u8 port, u8 idx,
				u64 *tstamp);

int fdice_ptp_hw_exec_cmd(struct fdice_hw *hw, enum fdice_ptp_tmr_cmd cmd);

#endif /* _FDICE_PTP_HW_H_ */
