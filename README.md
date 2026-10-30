# qmc-rdma — Quantum Monte Carlo over RDMA

A two-node distributed diffusion Monte Carlo (DMC) lab over hand-written
RDMA verbs, running on a SoftRoCE (RXE) fabric between two network
namespaces on a single Linux host — no RDMA hardware required.

**Why QMC is the vehicle:** DMC/VMC is walker-parallel with minimal
communication — exactly the traffic RDMA exists for. Population statistics
collection and walker rebalancing are the real workload, not a synthetic
benchmark:

- **Population control → one-sided RDMA READ.** Each node registers a buffer
  of walker `{position, weight}` structs; a master reads them all without
  touching remote CPUs.
- **Load balancing → RDMA WRITE.** Surplus walkers get pushed into an
  underpopulated node's registered buffer.
- **Memory registration** is the same physics as CUDA pinned memory, on a
  different bus: the NIC DMAs straight out of the buffer, so pages must be
  pinned (`ibv_reg_mr`).

**Reference:** Wei et al., [arXiv:2105.00027](https://arxiv.org/abs/2105.00027),
"Memory Reduction using a Ring Abstraction over GPU RDMA for Distributed
Quantum Monte Carlo Solver" — the grown-up version of this lab's premise.

## Components

| Component | What it is | Status |
|-----------|------------|--------|
| `01_bringup/` | SoftRoCE (RXE) setup: two network namespaces as two "nodes," RXE link over veth, `ibv_devices` / `ibstat` verification. Script: `bringup.sh {up\|down}` | validated 2026-10-23, re-validated 2026-10-30 (pinned governor, fresh boot): fabric up, both links ACTIVE/LinkUp, ping and ibstat clean; the ENFILE name collision (fixed by distinct rxe0/rxe1 names) did not recur |
| `02_perftest/` | `perftest` suite between namespaces: `ib_write_bw`, `ib_read_bw`, `ib_send_lat`; message-size and TX-depth sweeps; results table in `RESULTS.md`. Script: `run_sweeps.sh`, raw output in `raw/` | validated 2026-10-23, re-run pinned 2026-10-30 (raw/ and RESULTS.md replaced by the pinned run): 14/14 pairs rc=0, server+client raw output and exit statuses preserved; the TX-depth sweep has now given three distinct orderings on three runs across two governor states — recorded as noise-floor evidence, not a trend |
| `03_qmc_verbs/` | The physics payload. `dmc_serial.c`: serial reference DMC (1D harmonic oscillator, importance-sampled, α=0.8). `qmc_dmc_rdma.c`: the same physics split across two nodes over hand-written ibverbs — manual QP bring-up, TCP only for the QPN/GID/rkey handshake, RDMA READ/WRITE for statistics/control, WRITE+IMM for migration | serial validated 2026-10-18 (test suite 9/9, re-verified 2026-10-30); distributed validated 2026-10-23 and re-validated pinned 2026-10-30: QP up both sides, master E0 = 0.4995, worker E0 = 0.5020 vs exact 0.5000 over 2500 post-equilibration generations, populations stable near 500/node, both rc=0 (2026-10-23 run: 0.5011 / 0.4997 — within Monte Carlo spread of the new run) |
| `04_nccl/` | NCCL topology code-read notes (PCI/NVLink path scoring, ring vs tree) | not started |
| `BOUNDARY.md` | What SoftRoCE does NOT teach: DCQCN congestion control, PFC/ECN lossless config, firmware skew, link-flap pathology | not started |

## Correctness gates (closed)

- ~~The serial population feedback has a destabilizing sign, and its energy sum
  and denominator use different walker populations.~~ Fixed 2026-10-18:
  the feedback now lowers E_T on overpopulation, the generation estimator
  divides the pre-branch sum by the pre-branch count, and
  `03_qmc_verbs/test_dmc_serial.py` passes 9/9 (population stability, energy
  accuracy, multi-seed statistics, timestep behavior, entry-point
  validation).
- ~~Distributed completion polling must match `wr_id`/opcode rather than
  consuming any completion on the shared CQ. The single walker inbox also
  needs a peer consumption/ownership handshake, and nodes need independent
  RNG seeds.~~ Fixed 2026-10-18, fabric-validated 2026-10-23: `rdma_op`
  matches its own completion by `wr_id`/opcode and defers peer WRITE+IMM
  arrivals to `absorb_inbox`; the inbox is single-slot with an ownership READ
  (`n == 0` = free) and `absorb_inbox` runs at the top of the round so
  inbox-wait can never deadlock against the E_T exchange; master and worker
  default to distinct RNG seeds with a `--seed=` override.
- ~~Distributed-versus-serial agreement needs a fabric run.~~ Resolved
  2026-10-23: the distributed run agrees with the serial reference within
  the serial suite's own statistical spread — master E0 = 0.5011, worker
  E0 = 0.4997, serial 0.5010, exact 0.5000.
- Server and client perftest output and exit statuses are preserved in
  `02_perftest/raw/`. The toy's small host-registered payloads are not
  GPUDirect, GPU-memory scaling, or an RDMA bandwidth stress test.

## Environment notes (Fedora 44)

- `rdma_rxe` is in-kernel (`modprobe rdma_rxe`); userspace needs
  `rdma-core libibverbs-utils infiniband-diags perftest` (+ `rdma-core-devel`
  for the verbs code).
- RXE netns mode must be `shared` (default); `bringup.sh` checks and sets it.
  Corollary, learned the hard way: in shared mode RDMA devices are visible in
  every namespace, so the two nodes' devices need distinct names — ns1 uses
  `rxe0`, ns2 uses `rxe1` (same-name creation collides globally and surfaces
  as a misleading ENFILE, "Too many open files in system").
- Two namespaces + a veth pair = a two-node fabric on one host; no RDMA NIC
  required. Same verbs API the real NICs use. `bringup.sh up` is destructive
  to existing resources named ns1/ns2/rxe0/rxe1, not a status probe.

## Reproducible builds

From the repository root. `_DEFAULT_SOURCE` is currently needed for `M_PI`;
both programs compile warning-free with `-Wall -Wextra -pedantic`.

```bash
build_dir=$(mktemp -d /tmp/qmc-rdma-build.XXXXXX)
gcc -std=c11 -D_DEFAULT_SOURCE -O3 -Wall -Wextra -pedantic \
  03_qmc_verbs/dmc_serial.c -o "$build_dir/dmc_serial" -lm
gcc -std=c11 -D_DEFAULT_SOURCE -O3 -Wall -Wextra -pedantic \
  03_qmc_verbs/qmc_dmc_rdma.c -o "$build_dir/qmc_dmc_rdma" -libverbs -lm
```

The serial correctness suite builds itself into a temp dir:

```bash
python3 03_qmc_verbs/test_dmc_serial.py
```

Compilation is not physics or fabric validation. Generated executables belong
in a temporary/build directory, not beside the C sources.

## Running the lab

```bash
sudo 01_bringup/bringup.sh up        # fabric up, self-verifying
sudo 02_perftest/run_sweeps.sh       # ~2 min; raw output in 02_perftest/raw/

sudo ip netns exec ns1 "$build_dir/qmc_dmc_rdma" --listen &
sleep 1
sudo ip netns exec ns2 "$build_dir/qmc_dmc_rdma" --connect 10.0.0.1
# both nodes should print E0 ≈ 0.50

sudo 01_bringup/bringup.sh down      # teardown
```

## Scope

SoftRoCE emulates the verbs API over the kernel network stack; the numbers
here measure that software datapath on one machine, not NIC behavior.
Nothing here demonstrates GPUDirect, congestion control, or lossless-fabric
configuration.
