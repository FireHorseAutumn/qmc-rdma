# 02_perftest — RESULTS

SoftRoCE (RXE) two-namespace fabric, single RC QP per test, MTU 1024 B.
Measured 2026-10-23 on the lab laptop (Fedora, ondemand CPU governor active
— perftest reported conflicting CPU frequencies during most pairs).
Raw client and server output plus exit statuses: `raw/`. All 14 pairs
exited client_rc=0 server_rc=0.

These are software-RDMA numbers over the kernel network stack: they measure
the SoftRoCE datapath on this machine, not NIC behavior. See
`../BOUNDARY.md` scope notes in the README.

## Headline (default 64 KiB messages)

| Test | Result |
|---|---|
| ib_write_bw | 227.9 MiB/s avg (267.1 peak), 5000 iters |
| ib_read_bw | 271.4 MiB/s avg (290.9 peak), 1000 iters |
| ib_send_lat | 8.24 µs avg, 8.08 typical, 11.51 p99 (2 B, 1000 iters) |

## Message-size sweep — write bandwidth (TX depth 128)

| Size | avg MiB/s | peak MiB/s | MsgRate |
|---|---|---|---|
| 1 KiB | 266.2 | 271.8 | 0.273 Mpps |
| 4 KiB | 148.4 | 209.7 | 0.038 Mpps |
| 64 KiB | 153.9 | 257.8 | 0.0025 Mpps |
| 1 MiB | 160.4 | 258.0 | 0.00016 Mpps |

## Message-size sweep — send latency

| Size | typical µs | avg µs | p99 µs | max µs |
|---|---|---|---|---|
| 16 B | 7.84 | 7.96 | 10.56 | 30.88 |
| 256 B | 7.86 | 7.98 | 10.62 | 20.54 |
| 4 KiB | 19.21 | 19.58 | 24.32 | 42.44 |
| 64 KiB | 242.19 | 241.73 | 301.68 | 321.43 |

Latency is flat (~8 µs) up to 256 B, then grows with size — per-packet
software processing dominates small messages, copy/transmission cost takes
over past a few KiB.

## TX-depth sweep — 64 KiB write

| TX depth | avg MiB/s | peak MiB/s |
|---|---|---|
| 1 | 254.1 | 342.5 |
| 32 | 196.1 | 303.0 |
| 128 | 183.7 | 263.7 |

**This ordering did not replicate.** An earlier same-day run of the same
sweep (raw files since overwritten) gave 173.2 / 163.4 / 275.1 MiB/s for
depths 1/32/128 — the reverse ordering. Between-run machine state (CPU
frequency scaling; perftest printed conflicting-frequency warnings
throughout) exceeds any TX-depth effect at these sizes. No depth-tuning
claim is made; the sweep is preserved as evidence of the noise floor, not
of a trend.

## Notes

- RXE emulation runs over veth between network namespaces on one host;
  round trips never leave the kernel. Absolute numbers would differ on any
  other machine; the tables are a per-machine baseline, not a spec.
- `ib_read_bw` printed "Device not recognized to implement inline feature"
  — expected, RXE has no inline-data offload.
