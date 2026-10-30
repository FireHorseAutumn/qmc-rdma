# 04_nccl — NCCL topology machinery, a code read

Source read: `src/graph/paths.cc` at NVIDIA/nccl master (fetched
2026-10-30). Scope discipline: this note covers path computation and the
P2P/GDR gates — the code actually read. Ring/tree *construction*
(`rings.cc`, `trees.cc`) was not read; nothing below claims how channels
are laid out, only how paths are scored and gated.

## The model

NCCL builds a node graph (GPU, CPU, PCI, NVS, NET, DEV…) with typed,
bandwidth-annotated links, then runs `ncclTopoSetPaths` — a per-node BFS —
to compute the best path from every base node to every other node. "Best"
is lexicographic:

1. **path type** (lower wins — the type ladder below),
2. **bandwidth** (higher wins; a path's bw is the min over its links — the
   bottleneck rule),
3. **fewer hops** (strictly fewer, only to break exact ties).

The path-type ladder as seen in the code: `PATH_LOC` (self) < `PATH_NVL`
(single NVLink hop) < `PATH_NVB` (NVLink through an NVSwitch-bonded DEV,
bounded by `pathMaxLength` = 2 from a GPU) < `PATH_PIX` < `PATH_PXB`
(multiple PCI switches) < `PATH_PHB` (crosses a CPU host bridge) <
`PATH_SYS`. Composite types layered on top: `PATH_PXN` (GPU→NIC relayed
through an NVLink-connected GPU), `PATH_P2C` (NIC local to the CPU on a
C2C platform; `mergePathType(PHB, C2C) = P2C`), `PATH_NET` (unreachable
except by network — used to trim peers that can neither P2P nor SHM).

## The gates that decide what traffic actually flows

- **P2P (`ncclTopoCheckP2p`):** default cutoff `PATH_PXB` — no direct P2P
  across host bridges by default. AMD x86 systems with ≤ 2 PCI-device nodes
  get `PATH_SYS` (AMD host bridges are trusted further). User override via
  `NCCL_P2P_LEVEL`/`NCCL_P2P_DISABLE`. Then NVML gets a veto:
  `p2pStatusRead/Write` per pair, and a disagreement between topology and
  NVML on an NVLink pair is treated as a hardware fault ("P2P is disabled
  between NVLINK connected GPUs … probably due to a hardware issue") —
  a loud signal class worth copying: when the topology map and the device
  disagree, say so loudly.
- **GDR (`ncclTopoCheckGdr`):** both the NIC and the GPU must report
  `gdrSupport`; the GPU↔NIC distance must be ≤ `PATH_PXB` (≤ `PATH_P2C`
  on C2C parts). Reads (send side) are gated harder: pre-Ampere with other
  PCI flows gets no GDR reads unless an NVLink path exists. A PXN path's
  distance is re-resolved to the intermediate GPU's distance — the relay
  doesn't get to borrow proximity.
- **PXN:** only the GPU→NIC (send) direction gets the relay; the comment
  states the reason: favor receiving locally and sending remotely
  (consistent with net.cc). Asymmetry on purpose.
- **Flush (`ncclTopoNeedFlush`):** Hopper+ needs no explicit GDR flush —
  except DataDirect-style C2C systems where the control path is C2C and
  the data path is PCIe, which forces a flush. Architecture detail
  overrides the generation rule.
- **Net-vs-P2P (`ncclTopoCheckNet`):** same-node GPUs use the *network*
  only when both GPUs' best NIC path (≤ PXB) is faster than their direct
  GPU-GPU path — the rail-locality check, in one comparison.

## Channels

`ncclTopoGetNchannels`: NVLink-connected peers get `2 * max(1, bw / nvlBw)`
channels (bandwidth-proportional), PCIe peers get 2, and network peers get
channels from the local NIC count and `NCCL_P2P_PER_CHANNEL_NET_BW`
(14 GB/s per channel) — channels are a bandwidth-allocation device, not
just a concurrency one.

## What this says to the QMC lab

This repo's fabric has no topology: two namespaces, one path, one "NIC".
On a real multi-NIC cluster the physics layer's choices land exactly on
this machinery:

- Walker migration (WRITE+IMM) and population READs should target the
  rail-local NIC — the same preference PXN encodes in reverse.
- The WRITE+IMM migration path will sit behind the GDR flush rules if
  walker buffers ever move to GPU memory; the C2C/DataDirect exception is
  the case to remember.
- The NVML-veto pattern is the monitorable: topology-vs-device disagreement
  is how "fabric is up but lying" first shows up.
