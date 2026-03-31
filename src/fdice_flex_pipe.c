// SPDX-License-Identifier: GPL-2.0
/* Copyright (c) 2026, FrankenDrICE Contributors. */

/**
 * DOC: FrankenDrICE Flex Pipeline Table Population
 *
 * Populates the hardware pipeline tables (XLT, TCAM, field vectors)
 * from DDP package segment data. These tables configure how the E810
 * parser pipeline identifies protocols, extracts header fields, and
 * classifies packets for steering.
 *
 * This is a stub implementation for Phase 3.6 -- full table parsing
 * and population will be completed in Phase 3.5 (Flow Steering)
 * integration, when the pipeline tables are actively consumed by
 * the flow steering subsystem.
 *
 * Architecture reference: docs/architecture/06-implementation-plan.md (Phase 3.6)
 * Exploration reference: docs/exploration/12-ddp-dynamic-device-personalization.md
 */

#include <linux/types.h>
#include <linux/kernel.h>
#include <linux/device.h>

#include "fdice_dev.h"
#include "fdice_ddp.h"

/* -----------------------------------------------------------------------
 * Pipeline Table Population (Stub)
 * ----------------------------------------------------------------------- */

/**
 * fdice_flex_pipe_parse_section - Parse a single section from a buffer
 * @hw: hardware interface state
 * @sect_type: section type identifier
 * @data: pointer to section data
 * @size: size of section data in bytes
 *
 * Dispatches section data to the appropriate block table handler
 * based on the section type ID. Each pipeline block (SW, ACL, FD,
 * RSS, PE) has section types for XLT0, XLT1, XLT2, PROF_TCAM,
 * PROF_REDIR, and field vector tables.
 *
 * Stub: logs the section type for development visibility.
 *
 * Return: 0 on success, negative errno on parse error
 */
static int
fdice_flex_pipe_parse_section(struct fdice_hw *hw,
			      u32 sect_type, const void *data, u16 size)
{
	/*
	 * TODO (Phase 3.5 integration): Populate per-block tables:
	 *
	 * Switch block (SID 10-18):
	 *   XLT0: Protocol-to-group mapping
	 *   XLT1: Group-to-profile mapping
	 *   XLT2: Protocol-pair to profile mapping
	 *   PROF_TCAM: Profile TCAM entries for pattern matching
	 *   FLD_VEC: Field vector extraction sequences
	 *
	 * ACL block (SID 20-28): Access control list tables
	 * FD block (SID 30-38): Flow Director tables
	 * RSS block (SID 40-48): RSS hash configuration tables
	 * PE block (SID 80-88): Protocol Engine tables
	 *
	 * Parser sections (SID 50-78): RX/TX parser CAM, TCAM,
	 *   protocol groups, metadata initialization
	 */

	return 0;
}

/**
 * fdice_flex_pipe_fill_blk_tbls - Walk segment buffers and populate tables
 * @hw: hardware interface state
 * @seg: pointer to the E810 pipeline segment
 *
 * Iterates over all buffers in the segment, parsing each section
 * within each buffer and dispatching to the appropriate table
 * population handler.
 *
 * Stub: walks the structure for validation but does not store data.
 *
 * Return: 0 on success, negative errno on error
 */
static int
fdice_flex_pipe_fill_blk_tbls(struct fdice_hw *hw,
			      struct fdice_ice_seg *seg)
{
	const u8 *seg_data;
	u32 dev_tbl_size;
	const struct fdice_buf_table *buf_tbl;
	u32 buf_count;
	u32 i;

	/*
	 * Locate the buffer table within the segment. It follows the
	 * fixed header, device_table_count field, and device table entries.
	 */
	dev_tbl_size = le32_to_cpu(seg->device_table_count) *
		       sizeof(struct fdice_device_id_entry);
	seg_data = (const u8 *)seg + sizeof(struct fdice_seg_hdr) +
		   sizeof(__le32) + dev_tbl_size;
	buf_tbl = (const struct fdice_buf_table *)seg_data;

	buf_count = le32_to_cpu(buf_tbl->buf_count);

	for (i = 0; i < buf_count; i++) {
		const struct fdice_buf_hdr *bh;
		u16 sect_count;
		u16 j;

		bh = (const struct fdice_buf_hdr *)buf_tbl->buf_array[i].buf;
		sect_count = le16_to_cpu(bh->section_count);

		/* Validate section count bounds */
		if (sect_count > FDICE_MAX_S_COUNT)
			continue;

		for (j = 0; j < sect_count; j++) {
			u32 sect_type;
			u16 sect_off;
			u16 sect_size;
			const void *sect_data;
			int ret;

			sect_type = le32_to_cpu(
				bh->section_entry[j].type);
			sect_off = le16_to_cpu(
				bh->section_entry[j].offset);
			sect_size = le16_to_cpu(
				bh->section_entry[j].size);

			/* Skip metadata sections */
			if (sect_type & FDICE_METADATA_BUF)
				continue;

			/* Bounds validation */
			if (sect_off < FDICE_MIN_S_OFF ||
			    sect_off > FDICE_MAX_S_OFF)
				continue;
			if (sect_size < FDICE_MIN_S_SZ ||
			    sect_size > FDICE_MAX_S_SZ)
				continue;

			sect_data = (const u8 *)bh + sect_off;

			ret = fdice_flex_pipe_parse_section(
				hw, sect_type, sect_data, sect_size);
			if (ret)
				return ret;
		}
	}

	return 0;
}

/**
 * fdice_flex_pipe_init - Initialize pipeline tables from DDP package
 * @hw: hardware interface state
 * @seg: pointer to the E810 pipeline segment
 *
 * Top-level entry point for pipeline table population after a
 * successful DDP download. Walks the segment's buffer table and
 * populates hardware pipeline tables for all blocks.
 *
 * Return: 0 on success, negative errno on error
 */
int fdice_flex_pipe_init(struct fdice_hw *hw, struct fdice_ice_seg *seg)
{
	int ret;

	if (!seg)
		return -EINVAL;

	ret = fdice_flex_pipe_fill_blk_tbls(hw, seg);
	if (ret) {
		dev_warn(&hw->pdev->dev,
			 "flex pipeline table population failed: %d\n", ret);
		return ret;
	}

	dev_dbg(&hw->pdev->dev,
		"flex pipeline tables populated from DDP segment\n");

	return 0;
}

/**
 * fdice_flex_pipe_cleanup - Clear pipeline tables
 * @hw: hardware interface state
 *
 * Releases any resources allocated during pipeline table population.
 * Called during device removal or DDP cleanup.
 *
 * Stub: no resources to free in the current implementation.
 */
void fdice_flex_pipe_cleanup(struct fdice_hw *hw)
{
	/*
	 * TODO (Phase 3.5 integration): Free per-block table allocations:
	 *   - XLT lookup tables
	 *   - TCAM entries
	 *   - Field vector profile storage
	 *   - Label hint tables
	 */
}
