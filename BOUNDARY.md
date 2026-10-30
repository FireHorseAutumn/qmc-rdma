# BOUNDARY — what SoftRoCE does not teach

This lab's fabric is RXE (SoftRoCE) over a veth pair between two network
namespaces on one host. The verbs API is the real one — `ibv_reg_mr`,
QPs, CQs, RDMA READ/WRITE, WRITE+IMM — and the physics on top of it is
real. But the wire is the kernel's software datapath on loopback-class
plumbing, and a column of production RDMA reality never appears here.
This file is the explicit list, so nobody (including us, later) reads the
results as covering it.

## What transfers

- The verbs programming model itself: QP state machine, completion queues,
  memory registration and pinning semantics, scatter/gather, immediate
  data, the handshake problem (QPN/GID/rkey exchange over a side channel).
- The failure shapes we actually met and fixed: completion routing on a
  shared CQ (a receive completion consumed by the wrong waiter), inbox
  ownership between a peer's WRITE and the local consumer, per-role RNG
  seeding. These are API-level bugs; they exist identically on hardware.
- The measurement discipline: raw client+server output and exit statuses
  preserved, sweeps treated as noise-floor evidence when orderings don't
  replicate (three distinct TX-depth orderings on three runs — see
  `02_perftest/RESULTS.md`).

## What does not

- **Congestion control.** RoCE v2 on real fabrics runs DCQCN (ECN marks +
  CNP rate limiters) or newer schemes; RXE has none of it. Nothing here
  says anything about incast, PFC storms, or rate-limiter tuning — the
  whole "lossless Ethernet" problem class.
- **PFC/ECN configuration.** No switch exists in this lab. Priority Flow
  Control, ECN thresholds, and the head-of-line blocking they trade are
  out of scope by construction.
- **Firmware and NIC skew.** Real adapters carry firmware versions, offload
  differences (inline data — RXE reports none — atomics, TSO), and
  per-NIC errata. RXE is one kernel module; there is nothing to skew.
- **Link-flap and physical pathology.** veth links don't flap, don't
  degrade to half-duplex, don't get hot-optics retransmits. All the
  "the fabric is up but lying" failure modes are absent.
- **GPUDirect.** Walkers here live in host-registered memory; the NIC
  (emulated) DMAs from RAM. Nothing about GPU-memory registration,
  nvlink-peer copies, or `nvidia_peermem` behavior is exercised.
- **Topology.** Two namespaces on one host have one path, no hops, no
  routing. Multi-rail, rail-locality, and path-scoring questions (the
  NCCL problem — see `04_nccl/`) don't arise.
- **Scale.** Two nodes. No job scheduler, no partial failure, no straggler
  dynamics beyond what two populations show.

## Numbers context

The 2026-10-30 pinned run: ~7 µs send latency at small messages, ~180–280
MiB/s write bandwidth at 64 KiB — kernel-software numbers. A real
RoCE NIC pair does small-message latency in ~1–2 µs and bandwidth at line
rate (25/100/400 GbE). The gap is the point: this lab teaches verbs and
protocol logic, not wire performance.

## If this graduates to hardware

First questions to bring: DCQCN/PFC configuration state, per-NIC firmware
matrix, GPUDirect registration path, and the NCCL topology read in
`04_nccl/`. The physics layer (population control via one-sided READ,
migration via WRITE+IMM) is written against verbs and carries over
unchanged.
