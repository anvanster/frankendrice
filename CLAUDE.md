# FrankenDrICE — Claude Code Instructions

## Project Context
FrankenDrICE is a clean-sheet E810 driver informed by architectural patterns
from mlx5 and ice/irdma. This is a CodeGraph showcase project. License: GPL-2.0.
All code is original — reference drivers are study material, not source to copy.

## Critical Rules
- ALL files: `// SPDX-License-Identifier: GPL-2.0` on line 1
- ALL files: `/* Copyright (c) 2026, FrankenDrICE Contributors. */` on line 2
- NEVER copy from `ice-oot-reference/` — read only for understanding
- NEVER copy code one-to-one from ANY reference driver (kernel tree included)
- Reference drivers are study material — learn the pattern, then rewrite as fdice_
- MODULE_LICENSE must be "GPL v2"
- No Intel/NVIDIA/Mellanox trademarks in module descriptions

## Architecture Principle
The E810 Admin Queue command interface is the hardware boundary.
Everything BELOW the AQ interface is E810-specific — study ice's implementation,
then rewrite as fdice_ with improvements.
Everything ABOVE the AQ interface is where mlx5-inspired patterns add the most value.

## CodeGraph Usage
Use CodeGraph MCP tools to explore cross-codebase relationships before
making architectural decisions. Document all exploration findings in
docs/exploration/ before implementing.

## Code Originality Principle
DO NOT transplant code. For every function:
1. Study the reference implementations (ice, mlx5, irdma) using CodeGraph
2. Understand WHY it works that way
3. Rewrite from scratch as fdice_ with improvements, better error handling,
   cleaner abstractions, or optimized logic
4. If the result would be identical to the original — restructure it.
   Identical output means you copied, not learned.

## Naming Conventions
- ALL functions: `fdice_` prefix — no exceptions
- ALL structs: `fdice_` or `struct fdice_` — no exceptions
- ALL enums: `FDICE_` prefix
- ALL macros: `FDICE_` prefix
- The codebase must contain ZERO `ice_`, `mlx5_`, or `irdma_` prefixed symbols
- Use descriptive names that reflect FrankenDrICE's architecture, not the
  original driver's naming

## Quality Gates
- No new compiler warnings with W=1
- All error paths must free resources (check with sparse/smatch)
- Document every function with kerneldoc format
- Every architectural decision must reference the exploration finding that motivated it

## Reference Source Locations
- mlx5: `kernel/drivers/net/ethernet/mellanox/mlx5/`
- ice: `kernel/drivers/net/ethernet/intel/ice/`
- irdma: `kernel/drivers/infiniband/hw/irdma/`
- Kernel headers: `kernel/include/`
- Intel out-of-tree (READ ONLY): `ice-oot-reference/`

## Project Phases
1. **Phase 0** — Repo setup (sparse kernel checkout, project scaffold)
2. **Phase 1** — Deep exploration of both driver architectures using CodeGraph
3. **Phase 2** — Architecture design documents in `docs/architecture/`
4. **Phase 3** — Implementation: module scaffold → HAL/AQ → queues → health → flow steering → RDMA → devlink → netdev ops

## MCP Tools
- **CodeGraph** — Cross-codebase exploration, symbol search, call graphs, dependency analysis
- **Tempera** — Persistent memory for storing exploration findings and architectural decisions
