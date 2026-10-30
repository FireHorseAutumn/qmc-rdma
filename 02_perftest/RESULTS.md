# 02_perftest — RESULTS

SoftRoCE (RXE) two-namespace fabric, single RC QP per test, MTU 1024 B.
Measured 2026-10-30 on the lab laptop (Fedora, performance CPU governor
pinned and confirmed; no conflicting-frequency warnings this run — unlike
the superseded 2026-10-23 run, which ran under ondemand with those
warnings active). Raw client and server output plus exit statuses: `raw/`.
All 14 pairs exited client_rc=0 server_rc=0. The 2026-10-23 raw output is
superseded and preserved in-tree at `raw_2026-10-23/` (and in history at
`cb56a7b`).

These are software-RDMA numbers over the kernel network stack: they measure
the SoftRoCE datapath on this machine, not NIC behavior. See
`../BOUNDARY.md` scope notes in the README.

## Headline (default 64 KiB messages)

| Test | Result |
|---|---|
| ib_write_bw | 180.7 MiB/s avg (252.4 peak), 5000 iters |
| ib_read_bw | 279.9 MiB/s avg = peak, 1000 iters |
| ib_send_lat | 7.29 µs avg, 7.09 typical, 10.76 p99 (2 B, 1000 iters) |

## Message-size sweep — write bandwidth (TX depth 128)

| Size | avg MiB/s | peak MiB/s | MsgRate |
|---|---|---|---|
| 1 KiB | 148.8 | 195.2 | 0.152 Mpps |
| 4 KiB | 301.7 | 309.2 | 0.077 Mpps |
| 64 KiB | 267.2 | 316.5 | 0.0043 Mpps |
| 1 MiB | 240.4 | 301.8 | 0.00024 Mpps |

## Message-size sweep — send latency

| Size | typical µs | avg µs | p99 µs | max µs |
|---|---|---|---|---|
| 16 B | 7.78 | 7.77 | 10.08 | 16.61 |
| 256 B | 7.03 | 7.04 | 9.09 | 12.35 |
| 4 KiB | 18.98 | 19.29 | 24.14 | 31.14 |
| 64 KiB | 226.53 | 231.01 | 286.25 | 306.98 |

Latency is flat (~7 µs) up to 256 B, then grows with size — per-packet
software processing dominates small messages, copy/transmission cost takes
over past a few KiB.

## TX-depth sweep — 64 KiB write

| TX depth | avg MiB/s | peak MiB/s |
|---|---|---|
| 1 | 278.9 | 337.7 |
| 32 | 288.7 | 295.7 |
| 128 | 298.7 | 301.4 |

**This sweep has now given three distinct orderings on three runs**
(2026-10-23: 254/196/184 decreasing; an earlier same-day run reversed at
173/163/275; today, pinned governor: 279/289/299 increasing). Two different
governor states, three orderings — the non-replication is not (only) CPU
frequency scaling. No depth-tuning claim is made; the sweep is preserved as
evidence of the noise floor, not of a trend.

## Day-to-day variance note

Same fabric, same code, different day: the 64 KiB write average moved
227.9 → 180.7 MiB/s and send latency ~8.2 → ~7.3 µs between the superseded
run and this one, in opposite directions, both well outside within-run
spreads. Between-day machine state exceeds any effect these sweeps can
resolve; treat every table here as a per-machine, per-day baseline.

## Notes

- RXE emulation runs over veth between network namespaces on one host;
  round trips never leave the kernel. Absolute numbers would differ on any
  other machine; the tables are a per-machine baseline, not a spec.
- `ib_read_bw` printed "Device not recognized to implement inline feature"
  — expected, RXE has no inline-data offload.
