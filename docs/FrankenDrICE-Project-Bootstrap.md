# FrankenDrICE — Project Bootstrap & Claude Code Instructions

## Project Overview

**FrankenDrICE** is an experimental Linux kernel network driver for the Intel E810 adapter family, written from scratch after deeply studying NVIDIA/Mellanox's `mlx5` driver and Intel's `ice` + `irdma` driver stack. The project serves as a showcase for [CodeGraph](https://github.com/codegraph-ai/codegraph-vscode) — demonstrating AI-assisted comprehension, cross-codebase reasoning, and intelligent code synthesis across two large, complex kernel driver codebases.

**Goal:** Produce a clean-sheet E810 driver written entirely under the `fdice_` namespace, informed by the superior design patterns from mlx5 (queue management, RDMA integration, flow steering, resource lifecycle) and the E810-specific domain knowledge from ice/irdma — while remaining fully functional against E810 hardware.

**This is NOT a code merge or fork.** The mlx5 and ice drivers talk to fundamentally different hardware via different firmware command sets. FrankenDrICE *studies* both codebases, learns the architectural patterns, and produces an entirely new `fdice_`-prefixed codebase written from scratch — informed by the best ideas from both drivers but never copying code verbatim. The original driver sources serve as **reference material only**. The final codebase should contain zero `ice_`, `mlx5_`, or `irdma_` prefixed symbols — only `fdice_`.

---

## Licensing

All source code is *informed by* drivers in the Linux kernel mainline tree (`torvalds/linux`). Although FrankenDrICE is written from scratch, the architectural patterns and domain knowledge are derived from GPL-licensed kernel code, making this a derivative work under copyright law.

| Reference Driver | Location in kernel tree | License |
|-----------------|------------------------|---------|
| mlx5 | `drivers/net/ethernet/mellanox/mlx5/` | Dual BSD/GPL (`MODULE_LICENSE("Dual BSD/GPL")`) |
| ice | `drivers/net/ethernet/intel/ice/` | GPL-2.0 (`MODULE_LICENSE("GPL v2")`) |
| irdma | `drivers/infiniband/hw/irdma/` | GPL-2.0 OR Linux-OpenIB |

**Resulting license: GPL-2.0.**

The ice driver's strict GPL-2.0 is the most restrictive compatible license in the mix. The BSD side of mlx5's dual license and the OpenIB side of irdma's dual license are both compatible when combined under GPL-2.0. As a derivative work informed by these GPL sources, FrankenDrICE **must** be published under GPL-2.0.

### License Rules for Claude Code

- Every file MUST include: `// SPDX-License-Identifier: GPL-2.0`
- Every file MUST include a copyright line: `/* Copyright (c) 2026, FrankenDrICE Contributors. */`
- The repository root MUST contain a `COPYING` file with the full GPL-2.0 text
- **DO NOT** use Intel's out-of-tree driver from downloadcenter.intel.com — it has a separate EULA
- **ONLY** use source from the mainline kernel tree (`torvalds/linux` on GitHub)
- **DO NOT** use NVIDIA/Mellanox/Intel trademarks in any module descriptions or branding
- `MODULE_LICENSE` for the FrankenDrICE driver MUST be `"GPL v2"`
- **DO NOT** copy code one-to-one from any reference driver — study, learn, rewrite
- All code must be original `fdice_`-prefixed implementations informed by reference patterns
- The final codebase must contain NO `ice_`, `mlx5_`, or `irdma_` prefixed symbols
- Reference drivers are study material, not a source to transplant from

---

## Phase 0: Repository Setup

### Clone Required Repositories

```bash
# Create workspace
mkdir -p ~/frankendrice && cd ~/frankendrice

# Clone the Linux kernel (sparse checkout — we only need the driver subtrees)
git clone --depth 1 --filter=blob:none --sparse \
  https://github.com/torvalds/linux.git kernel
cd kernel
git sparse-checkout set \
  drivers/net/ethernet/mellanox/mlx5 \
  drivers/net/ethernet/intel/ice \
  drivers/infiniband/hw/irdma \
  include/linux/mlx5 \
  include/linux/net/intel \
  include/net \
  include/linux/auxiliary_bus.h \
  include/linux/etherdevice.h \
  include/linux/netdevice.h \
  include/linux/pci.h \
  include/rdma \
  include/uapi/rdma
cd ..

# Clone Intel's out-of-tree ice driver (reference only — for understanding
# features not yet upstreamed. DO NOT copy code from here, only study patterns)
git clone https://github.com/intel/ethernet-linux-ice.git ice-oot-reference

# Clone CodeGraph VSCode extension
git clone https://github.com/codegraph-ai/codegraph-vscode.git codegraph

# Create the FrankenDrICE output project
mkdir -p frankendrice-driver/src
cd frankendrice-driver
git init
```

### Initial Project Structure

```
frankendrice/
├── kernel/                          # Sparse checkout of torvalds/linux
│   ├── drivers/net/ethernet/
│   │   ├── mellanox/mlx5/           # mlx5 source (reference/study only)
│   │   └── intel/ice/               # ice source (reference/study only)
│   ├── drivers/infiniband/hw/irdma/ # irdma source (reference/study only)
│   └── include/                     # Kernel headers (reference)
├── ice-oot-reference/               # Intel out-of-tree (READ ONLY reference)
├── codegraph/                       # CodeGraph tooling
└── frankendrice-driver/             # Clean-sheet output project
    ├── COPYING                      # GPL-2.0 full text
    ├── README.md
    ├── CLAUDE.md                    # Claude Code persistent instructions
    ├── Kconfig
    ├── Makefile
    └── src/                         # All-original fdice_ source
```

---

## Phase 1: Exploration & Indexing with CodeGraph

This is the critical phase. Before writing any code, Claude Code must deeply understand both driver architectures using CodeGraph's context provider capabilities.

### 1.1 Index All Source Trees

Use CodeGraph to index the following source directories. All three must be in the same index so cross-references and architectural comparisons can be made:

- `kernel/drivers/net/ethernet/mellanox/mlx5/` (~200+ files, ~80k LOC)
- `kernel/drivers/net/ethernet/intel/ice/` (~80+ files, ~50k LOC)
- `kernel/drivers/infiniband/hw/irdma/` (~30+ files, ~38k LOC)
- Relevant kernel headers from `kernel/include/`

### 1.2 Exploration Questions

Claude Code should use CodeGraph to answer these questions systematically before proceeding to implementation. Document findings in `frankendrice-driver/docs/exploration/`.

#### Architecture Comparison

1. **Driver initialization lifecycle**: How does mlx5 probe/init vs ice probe/init? What does mlx5 do that ice doesn't? Focus on `mlx5_init_one()` vs `ice_probe()`.

2. **Queue management**: Compare mlx5's WQ/CQ/EQ abstraction layers with ice's queue model. mlx5 has a more sophisticated multi-level queue hierarchy — map the differences.

3. **Resource lifecycle & cleanup**: mlx5 uses a structured teardown sequence with dependency tracking. How does ice handle resource cleanup? Where does ice leak or have less robust error recovery?

4. **Flow steering / traffic classification**: mlx5 has a sophisticated flow steering tree (flow tables → flow groups → flow table entries). Compare with ice's Flow Director and switchdev implementation. What patterns can be reimplemented for E810?

5. **RDMA integration model**: Both use auxiliary bus for RDMA. Compare how mlx5 exposes RDMA resources vs how ice/irdma does it. mlx5 has tighter coupling — is that beneficial?

6. **SR-IOV and VF management**: Compare mlx5's SRIOV model (especially E-Switch offloads) with ice's VF management. mlx5's eswitch is significantly more capable.

7. **Devlink integration**: mlx5 has extensive devlink support (health reporters, flash update, params). Compare depth with ice's devlink implementation.

8. **Error handling & health monitoring**: mlx5's health/recovery system with syndrome-based diagnostics. What does ice do? This is likely a major improvement vector.

9. **DMA and memory management**: Compare buffer management, page pool usage, and DMA mapping strategies.

10. **DCB/QoS implementation**: Compare PFC, ETS, and DCBX implementations between the two. Given the RDMA use case, this is critical.

#### E810-Specific Deep Dives

11. **Admin Queue command interface**: Map the full AQ command set used by ice. This is the hardware abstraction boundary — everything above it can be redesigned freely, everything below must match E810 firmware expectations.

12. **DDP (Dynamic Device Personalization)**: Understand ice's DDP package loading mechanism deeply. This is unique to E810 and must be reimplemented correctly — firmware compatibility depends on getting this right.

13. **PTP/clock synchronization**: ice has sophisticated PTP support for E810. Catalog the implementation.

14. **ADQ (Application Device Queues)**: ice's ADQ feature set — how it maps to traffic classes.

15. **Switchdev mode**: ice's switchdev implementation and offload capabilities.

---

## Phase 2: Architecture Design

Based on exploration findings, produce an architecture document before writing code.

### Key Design Decisions to Make

- **Which mlx5 patterns are worth reimplementing?** Some are deeply tied to ConnectX firmware (e.g., Flex Parser, programmable flow steering tables). These cannot translate to E810. Focus on software-layer patterns that improve driver robustness regardless of hardware.

- **Abstraction boundary**: Define a clean HAL (Hardware Abstraction Layer) that isolates E810 AQ commands from the upper driver layers. Study how both ice and mlx5 handle their respective hardware interfaces, then design an `fdice_hw_` API that's cleaner than either.

- **RDMA integration model**: Should FrankenDrICE adopt mlx5's tightly coupled RDMA model or ice/irdma's auxiliary bus separation? Study both, decide based on E810 hardware constraints.

- **Hardware-specific domain knowledge**: DDP loading, firmware update paths, NVM management, PCI config — the *what* must match E810 hardware requirements, but the *how* (code structure, error handling, abstractions) should be reimagined from scratch.

### Output Artifacts

Document in `frankendrice-driver/docs/architecture/`:

- `00-design-rationale.md` — Why each pattern was chosen and how fdice_ improves on it
- `01-hal-design.md` — Hardware abstraction layer specification  
- `02-queue-architecture.md` — fdice_ queue management design
- `03-rdma-integration.md` — RDMA subsystem design
- `04-flow-steering.md` — Traffic classification design
- `05-health-monitoring.md` — Error recovery design
- `06-implementation-plan.md` — Ordered list of what to implement and when

---

## Phase 3: Implementation

### Build Strategy

Write from scratch, informed by exploration findings. **Do not copy-paste code from reference drivers.** Study the patterns, understand the reasoning, then reimplement under the `fdice_` namespace with optimizations and improvements.

1. **Scaffold the module** — `fdice_main.c` with module init/exit, PCI probe/remove for E810 device IDs, `MODULE_LICENSE("GPL v2")`
2. **Implement HAL** — `fdice_hw.c` / `fdice_hw.h` — clean Admin Queue abstraction layer, informed by studying both ice's AQ and mlx5's command interface patterns. Optimize for clarity and error handling
3. **Implement queue management** — `fdice_queue.c` — learn from mlx5's WQ/CQ/EQ layered model AND ice's ring-based model, produce a unified `fdice_` queue architecture that leverages the best of both
4. **Implement health/recovery** — `fdice_health.c` — inspired by mlx5's health reporters and syndrome-based diagnostics, adapted for E810 error conditions
5. **Implement flow steering** — `fdice_flow.c` — learn from mlx5's flow table tree abstraction, implement over E810's Flow Director and switch filter capabilities
6. **Implement RDMA integration** — `fdice_rdma.c` — study both mlx5's tight RDMA coupling and ice/irdma's auxiliary bus model, choose and implement the better approach
7. **Implement devlink** — `fdice_devlink.c` — health reporters, flash update, parameter exposure
8. **Implement resource lifecycle** — structured init/teardown with dependency tracking across all subsystems
9. **Implement netdev operations** — `fdice_ethtool.c`, `fdice_txrx.c` — data path informed by both drivers' approaches

### Code Quality Principle

For every function written, Claude Code should be able to answer: "What did I learn from the reference code, and how is `fdice_` doing it better or differently?" If the answer is "it's identical" — that's a red flag. Rewrite it with improvements, better error messages, cleaner abstractions, or at minimum restructured logic that reflects genuine understanding rather than transcription.

### Build & Test

```bash
# Build against running kernel headers
cd frankendrice-driver
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules

# Load (requires E810 hardware or can be validated structurally)
sudo insmod frankendrice.ko

# Basic validation
dmesg | grep frankendrice
ethtool -i <interface>
```

---

## CLAUDE.md Template

Place this in `frankendrice-driver/CLAUDE.md` for persistent Claude Code instructions:

```markdown
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
```

---

## Repository README.md Template

```markdown
# FrankenDrICE 🧟‍♂️❄️

An experimental, clean-sheet Linux kernel network driver for the Intel E810
adapter family, informed by architectural best practices studied from
NVIDIA/Mellanox mlx5 and Intel's ice/irdma stacks.

**This project is a showcase for [CodeGraph](https://github.com/codegraph-ai/codegraph-vscode)**
— demonstrating AI-assisted cross-codebase comprehension and intelligent code
synthesis across complex kernel driver architectures.

## What Is This?

FrankenDrICE (Franken-Dr-ICE) studies two of the most complex driver
architectures in the Linux kernel — mlx5 (ConnectX) and ice (E810) — and
produces an entirely new, clean-sheet driver for the E810 adapter family.

No code was copied one-to-one. The reference drivers served as study material:
CodeGraph was used to deeply understand queue management, health monitoring,
flow steering, RDMA integration, and resource lifecycle patterns across both
codebases. FrankenDrICE then reimplements these patterns from scratch under
a unified `fdice_` namespace, with improvements and optimizations informed
by the architectural strengths of both originals.

This is NOT a fork or a code merge. It's what happens when an AI reads
170k lines of kernel driver code and writes something new.

## Status

🚧 **Experimental / Work In Progress** — Not for production use.

## License

This project is an original work informed by GPL-2.0 licensed code in the Linux kernel.

Licensed under **GPL-2.0**. See [COPYING](COPYING) for the full license text.

### Reference Sources

No code was copied from these drivers. They served as architectural study material.

| Reference | Origin | License |
|-----------|--------|---------|
| ice driver | `drivers/net/ethernet/intel/ice/` | GPL-2.0 |
| irdma driver | `drivers/infiniband/hw/irdma/` | GPL-2.0 OR Linux-OpenIB |
| mlx5 driver | `drivers/net/ethernet/mellanox/mlx5/` | Dual BSD/GPL |

All reference sources from [torvalds/linux](https://github.com/torvalds/linux).

## Building

Requires Linux kernel headers for your running kernel.

\`\`\`bash
make -C /lib/modules/$(uname -r)/build M=$(pwd) modules
sudo insmod frankendrice.ko
\`\`\`

## Powered By

Built with [CodeGraph](https://github.com/codegraph-ai/codegraph-vscode) —
AI-native codebase understanding for developers.
```

---

## Notes for Review

- The sparse checkout approach keeps the workspace manageable (~170k LOC of driver code vs the full 30M+ LOC kernel)
- Intel's out-of-tree driver is included as read-only reference because it contains features (HQoS, 5-layer scheduler topology) not yet upstreamed — useful for understanding E810 capabilities but code must NOT be copied due to separate EULA
- The `fdice_` prefix is the ONLY prefix in the output codebase — no residual `ice_`, `mlx5_`, or `irdma_` symbols. This enforces that every line was genuinely rewritten, not transplanted
- The "study, learn, rewrite" principle also strengthens the legal position — original work informed by GPL sources (still published as GPL-2.0 to respect derivative work obligations) is cleaner than a patchwork of copied files with renamed prefixes
- Exploration phase is deliberately front-loaded — this is where CodeGraph adds the most value and where the project narrative for the showcase is strongest
- The code quality principle ("if it's identical, you copied not learned") is both a legal safeguard and a genuine quality bar — forcing reimplementation surfaces opportunities for improvement
