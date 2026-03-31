// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

#ifndef _FDICE_TXRX_H_
#define _FDICE_TXRX_H_

/**
 * DOC: FrankenDrICE Data Path Declarations and Inline Helpers
 *
 * Hardware-fixed descriptor formats for E810 flex RX and TX descriptors,
 * NAPI poll entry points, and inline hot-path helpers for work queue
 * index/fill-level calculations.
 *
 * Descriptor layouts must match E810 firmware expectations exactly --
 * these are the hardware boundary below the AQ interface.
 *
 * Architecture reference: docs/architecture/02-queue-architecture.md sec 4-5
 */

#include <linux/types.h>
#include <linux/bitfield.h>
#include <linux/byteorder/generic.h>

#include "fdice_queue.h"

/* Forward declarations */
struct fdice_channel;
struct fdice_rxq;
struct fdice_txq;

/* -----------------------------------------------------------------------
 * Data Path Constants
 * ----------------------------------------------------------------------- */

/**
 * FDICE_RX_REFILL_THRESHOLD - Minimum missing buffers before refill
 *
 * Prevents excessive small page_pool allocations while ensuring the
 * hardware never runs out of posted buffers under normal load.
 * Sized to match the page pool cache refill unit.
 */
#define FDICE_RX_REFILL_THRESHOLD	64

/**
 * FDICE_TX_CLEAN_THRESHOLD - Minimum completed WQEs before TX cleanup
 *
 * Batches TX completion processing for amortized DMA unmap cost.
 */
#define FDICE_TX_CLEAN_THRESHOLD	32

/**
 * FDICE_MAX_READ_REQ_SIZE - PCIe Max Read Request Size for TX alignment
 *
 * TX descriptor data chunks are aligned to this boundary to maximize
 * PCIe bus efficiency. 512 bytes covers the common PCIe MPS/MRRS.
 */
#define FDICE_MAX_READ_REQ_SIZE		512

/* -----------------------------------------------------------------------
 * E810 RX Flex Descriptor (32 bytes) -- Hardware Fixed Format
 *
 * This union matches the E810 flex descriptor writeback layout.
 * The 'read' arm is what software writes when posting buffers;
 * the 'wb' arm is what firmware writes back after reception.
 * ----------------------------------------------------------------------- */

/**
 * union fdice_rx_flex_desc - E810 32-byte flexible RX descriptor
 * @read: Software-written format for posting RX buffers to hardware
 * @read.pkt_addr: DMA address of packet data buffer
 * @read.hdr_addr: DMA address of header buffer (bit 0 is DD bit)
 * @read.rsvd1: Reserved qword
 * @read.rsvd2: Reserved qword
 * @wb: Firmware-written format after packet reception
 * @wb.rxdid: Descriptor builder profile ID
 * @wb.mir_id_umb_cast: Mirror ID [5:0], unicast/multicast/broadcast [7:6]
 * @wb.ptype_flex_flags0: Packet type [9:0], flex flags 0 [15:10]
 * @wb.pkt_len: Packet length ([15:14] reserved)
 * @wb.hdr_len_sph_flex_flags1: Header length [10:0], split [11], ff1 [15:12]
 * @wb.status_error0: Primary status/error bits (DD, EOP, etc.)
 * @wb.l2tag1: First L2 VLAN tag (if stripped)
 * @wb.flex_meta0: Flex metadata word 0 (profile-dependent)
 * @wb.flex_meta1: Flex metadata word 1 (profile-dependent)
 * @wb.status_error1: Extended status/error bits
 * @wb.flex_flags2: Additional flex flags
 * @wb.time_stamp_low: Low byte of timestamp
 * @wb.l2tag2_1st: Second L2 tag, first instance
 * @wb.l2tag2_2nd: Second L2 tag, second instance
 * @wb.flex_meta2: Flex metadata word 2
 * @wb.flex_meta3: Flex metadata word 3
 * @wb.flex_ts: Union of flex metadata 4/5 or 32-bit timestamp high
 *
 * Size: 32 bytes (4 qwords). Must not be modified -- layout is dictated
 * by E810 hardware and firmware.
 */
union fdice_rx_flex_desc {
	struct {
		__le64 pkt_addr;
		__le64 hdr_addr;
		__le64 rsvd1;
		__le64 rsvd2;
	} read;
	struct {
		/* Qword 0 */
		u8	rxdid;
		u8	mir_id_umb_cast;
		__le16	ptype_flex_flags0;
		__le16	pkt_len;
		__le16	hdr_len_sph_flex_flags1;

		/* Qword 1 */
		__le16	status_error0;
		__le16	l2tag1;
		__le16	flex_meta0;
		__le16	flex_meta1;

		/* Qword 2 */
		__le16	status_error1;
		u8	flex_flags2;
		u8	time_stamp_low;
		__le16	l2tag2_1st;
		__le16	l2tag2_2nd;

		/* Qword 3 */
		__le16	flex_meta2;
		__le16	flex_meta3;
		union {
			struct {
				__le16 flex_meta4;
				__le16 flex_meta5;
			} flex;
			__le32 ts_high;
		} flex_ts;
	} wb;
};

/* Flex descriptor status_error0 bit positions */
#define FDICE_RX_FLEX_DESC_STATUS0_DD_S		0
#define FDICE_RX_FLEX_DESC_STATUS0_EOF_S	1
#define FDICE_RX_FLEX_DESC_STATUS0_HBO_S	2
#define FDICE_RX_FLEX_DESC_STATUS0_L3L4P_S	3
#define FDICE_RX_FLEX_DESC_STATUS0_XSUM_IPE_S	4
#define FDICE_RX_FLEX_DESC_STATUS0_XSUM_L4E_S	5
#define FDICE_RX_FLEX_DESC_STATUS0_XSUM_EIPE_S	6
#define FDICE_RX_FLEX_DESC_STATUS0_XSUM_EUDPE_S 7
#define FDICE_RX_FLEX_DESC_STATUS0_LPBK_S	8
#define FDICE_RX_FLEX_DESC_STATUS0_IPV6EXADD_S	9
#define FDICE_RX_FLEX_DESC_STATUS0_RXE_S	10
#define FDICE_RX_FLEX_DESC_STATUS0_CRCP_S	11
#define FDICE_RX_FLEX_DESC_STATUS0_RSS_VALID_S	12
#define FDICE_RX_FLEX_DESC_STATUS0_L2TAG1P_S	13
#define FDICE_RX_FLEX_DESC_STATUS0_XTRMD0_VALID_S 14
#define FDICE_RX_FLEX_DESC_STATUS0_XTRMD1_VALID_S 15

/* Packet type field extraction from ptype_flex_flags0 */
#define FDICE_RX_FLEX_DESC_PTYPE_M	GENMASK(9, 0)

/* -----------------------------------------------------------------------
 * E810 TX Descriptor (16 bytes) -- Hardware Fixed Format
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_tx_desc - E810 16-byte transmit descriptor
 * @buf_addr: DMA address of the data buffer
 * @cmd_type_offset_bsz: Combined command, type, offset, and buffer size
 *
 * The cmd_type_offset_bsz qword packs multiple fields:
 *   [3:0]   dtype   - Descriptor type (data=0x0, context=0x1, done=0xF)
 *   [15:4]  cmd     - Command bits (EOP, RS, TSO, etc.)
 *   [33:16] offset  - Header offsets (MACLEN, IPLEN, L4LEN)
 *   [47:34] l2tag1  - L2 VLAN tag insertion value
 *   [63:48] tx_buf_sz - Data buffer size in bytes
 *
 * Size: 16 bytes (2 qwords). Must not be modified -- layout is dictated
 * by E810 hardware and firmware.
 */
struct fdice_tx_desc {
	__le64			buf_addr;
	__le64			cmd_type_offset_bsz;
};

/* TX descriptor dtype field values */
#define FDICE_TXD_DTYPE_DATA		0x0
#define FDICE_TXD_DTYPE_CTX		0x1
#define FDICE_TXD_DTYPE_FLTR_PROG	0x8
#define FDICE_TXD_DTYPE_DESC_DONE	0xF

/* TX descriptor cmd_type_offset_bsz field layout */
#define FDICE_TXD_QW1_DTYPE_S		0
#define FDICE_TXD_QW1_DTYPE_M		GENMASK_ULL(3, 0)
#define FDICE_TXD_QW1_CMD_S		4
#define FDICE_TXD_QW1_CMD_M		GENMASK_ULL(15, 4)
#define FDICE_TXD_QW1_OFFSET_S		16
#define FDICE_TXD_QW1_OFFSET_M		GENMASK_ULL(33, 16)
#define FDICE_TXD_QW1_TX_BUF_SZ_S	34
#define FDICE_TXD_QW1_TX_BUF_SZ_M	GENMASK_ULL(47, 34)
#define FDICE_TXD_QW1_L2TAG1_S		48
#define FDICE_TXD_QW1_L2TAG1_M		GENMASK_ULL(63, 48)

/* TX descriptor command bits */
#define FDICE_TX_DESC_CMD_EOP		BIT(0)
#define FDICE_TX_DESC_CMD_RS		BIT(1)
#define FDICE_TX_DESC_CMD_IL2TAG1	BIT(3)
#define FDICE_TX_DESC_CMD_DUMMY		BIT(4)
#define FDICE_TX_DESC_CMD_IIPT_IPV6	BIT(5)
#define FDICE_TX_DESC_CMD_IIPT_IPV4	BIT(6)
#define FDICE_TX_DESC_CMD_IIPT_IPV4_CSUM (BIT(5) | BIT(6))
#define FDICE_TX_DESC_CMD_L4T_EOFT_TCP	BIT(8)
#define FDICE_TX_DESC_CMD_L4T_EOFT_SCTP	BIT(9)
#define FDICE_TX_DESC_CMD_L4T_EOFT_UDP	(BIT(8) | BIT(9))
#define FDICE_TX_DESC_CMD_RE		BIT(10)

/* TX descriptor offset field sub-fields (within the 18-bit offset) */
#define FDICE_TX_DESC_LEN_MACLEN_S	0
#define FDICE_TX_DESC_LEN_IPLEN_S	7
#define FDICE_TX_DESC_LEN_L4_LEN_S	14

/* -----------------------------------------------------------------------
 * TX Context Descriptor (16 bytes) -- Hardware Fixed Format
 * ----------------------------------------------------------------------- */

/**
 * struct fdice_tx_ctx_desc - E810 TX context descriptor for TSO/tunneling
 * @tunneling_params: Tunnel header offsets and type
 * @l2tag2: Second VLAN tag for QinQ insertion
 * @gcs: Generic checksum parameters
 * @qw1: Command/TSO fields (MSS, TSO enable, etc.)
 */
struct fdice_tx_ctx_desc {
	__le32			tunneling_params;
	__le16			l2tag2;
	__le16			gcs;
	__le64			qw1;
};

/* -----------------------------------------------------------------------
 * NAPI Poll Entry Points
 * ----------------------------------------------------------------------- */

int fdice_napi_poll(struct napi_struct *napi, int budget);
netdev_tx_t fdice_xmit_frame(struct sk_buff *skb, struct fdice_txq *txq);

/* Per-queue poll helpers (called from fdice_napi_poll) */
int fdice_poll_tx_cq(struct fdice_txq *txq, int budget);
int fdice_poll_rx_cq(struct fdice_rxq *rxq, int budget);

/* -----------------------------------------------------------------------
 * Inline Hot-Path Helpers
 * ----------------------------------------------------------------------- */

/**
 * fdice_wq_cyc_get - Get virtual address of WQE at given index
 * @wq: Cyclic work queue
 * @ix: Raw ring index (will be masked)
 *
 * Translates a ring index to the virtual address of the corresponding
 * WQE within the fragmented DMA buffer.
 *
 * Return: Pointer to the WQE at the masked index position
 */
static inline void *fdice_wq_cyc_get(struct fdice_wq_cyc *wq, u16 ix)
{
	u32 masked = ix & wq->mask;
	u32 frag_ix = masked >> (wq->fbc.frag_sz_shift - wq->fbc.log_stride);
	u32 frag_off = (masked & (wq->fbc.strides_per_frag - 1))
			<< wq->fbc.log_stride;

	return wq->fbc.frag_buf.buf[frag_ix] + frag_off;
}

/**
 * fdice_wq_cyc_missing - Number of free entries available for posting
 * @wq: Cyclic work queue
 *
 * Computes the number of WQE slots between the consumer counter and
 * the producer counter. This is the space available for new posts.
 *
 * Return: Number of free WQE slots
 */
static inline u16 fdice_wq_cyc_missing(struct fdice_wq_cyc *wq)
{
	return wq->sz - (wq->pc - wq->cc);
}

/**
 * fdice_wq_cyc_get_dma - Get DMA address of WQE at given index
 * @wq: Cyclic work queue
 * @ix: Raw ring index (will be masked)
 *
 * Translates a ring index to the DMA bus address of the corresponding
 * WQE for hardware doorbell writes and descriptor posting.
 *
 * Return: DMA address of the WQE at the masked index position
 */
static inline dma_addr_t fdice_wq_cyc_get_dma(struct fdice_wq_cyc *wq, u16 ix)
{
	u32 masked = ix & wq->mask;
	u32 frag_ix = masked >> (wq->fbc.frag_sz_shift - wq->fbc.log_stride);
	u32 frag_off = (masked & (wq->fbc.strides_per_frag - 1))
			<< wq->fbc.log_stride;

	return wq->fbc.frag_buf.dma[frag_ix] + frag_off;
}

/**
 * fdice_wq_cyc_is_full - Check if the work queue is full
 * @wq: Cyclic work queue
 *
 * Return: true if no free WQE slots remain
 */
static inline bool fdice_wq_cyc_is_full(struct fdice_wq_cyc *wq)
{
	return fdice_wq_cyc_missing(wq) == 0;
}

/**
 * fdice_wq_cyc_is_empty - Check if the work queue is empty
 * @wq: Cyclic work queue
 *
 * Return: true if producer and consumer counters are equal
 */
static inline bool fdice_wq_cyc_is_empty(struct fdice_wq_cyc *wq)
{
	return wq->pc == wq->cc;
}

/**
 * fdice_tx_desc_encode - Build cmd_type_offset_bsz for a TX data descriptor
 * @dtype: Descriptor type (FDICE_TXD_DTYPE_*)
 * @cmd: Command bits (ORed FDICE_TX_DESC_CMD_* flags)
 * @offset: Packed header offset field
 * @size: Data buffer size in bytes
 *
 * Return: Fully encoded __le64 value for fdice_tx_desc.cmd_type_offset_bsz
 */
static inline __le64 fdice_tx_desc_encode(u16 dtype, u16 cmd,
					   u32 offset, u16 size)
{
	return cpu_to_le64(((u64)dtype) |
			   ((u64)cmd << FDICE_TXD_QW1_CMD_S) |
			   ((u64)offset << FDICE_TXD_QW1_OFFSET_S) |
			   ((u64)size << FDICE_TXD_QW1_TX_BUF_SZ_S));
}

#endif /* _FDICE_TXRX_H_ */
