#!/usr/bin/env python3
"""Serial DMC correctness tests (QMC-1): population stability, energy
accuracy, multi-seed statistics, timestep behavior.

Builds dmc_serial.c into a temp dir and checks parsed RESULT lines.
Bounds carry roughly 4x margin over the observed 2026-10-18 values
(spread +-0.001 across seeds, population within +-5% of target).

Usage: python3 test_dmc_serial.py
"""

import math
import re
import subprocess
import sys
import tempfile
from pathlib import Path

SRC = Path(__file__).resolve().parent / "dmc_serial.c"
EXACT = 0.5

failures = 0


def check(ok, name, detail):
    print(f"[{'pass' if ok else 'FAIL'}] {name}: {detail}")
    if not ok:
        global failures
        failures += 1


def run(binary, *args):
    out = subprocess.run(
        [binary, "--quiet", *args], capture_output=True, text=True, check=True
    ).stdout
    m = re.search(r"RESULT (.*)", out)
    kv = dict(kv.split("=") for kv in m.group(1).split())
    return {
        "E0": float(kv["E0"]),
        "pop_final": int(kv["pop_final"]),
        "pop_mean": float(kv["pop_mean"]),
        "target": int(kv["target"]),
    }


def rc_of(binary, *args):
    return subprocess.run(
        [binary, "--quiet", *args], capture_output=True
    ).returncode


def main():
    with tempfile.TemporaryDirectory(prefix="dmc-test-") as td:
        binary = str(Path(td) / "dmc_serial")
        subprocess.run(
            ["gcc", "-std=c11", "-D_DEFAULT_SOURCE", "-O3", "-Wall", "-Wextra",
             "-pedantic", str(SRC), "-o", binary, "-lm"],
            check=True,
        )

        # --- population stability + energy accuracy, default parameters ---
        r = run(binary)
        check(
            0.75 * r["target"] <= r["pop_final"] <= 1.25 * r["target"]
            and 0.75 * r["target"] <= r["pop_mean"] <= 1.25 * r["target"],
            "population stable at target",
            f"pop_final={r['pop_final']} pop_mean={r['pop_mean']:.1f} "
            f"target={r['target']}",
        )
        check(
            abs(r["E0"] - EXACT) < 0.005,
            "E0 near exact (default seed)",
            f"E0={r['E0']:.6f} exact={EXACT}",
        )

        # --- multi-seed statistics ---
        runs = [run(binary, f"--seed={s}") for s in range(1, 9)]
        e0s = [x["E0"] for x in runs]
        mean = sum(e0s) / len(e0s)
        std = math.sqrt(sum((e - mean) ** 2 for e in e0s) / (len(e0s) - 1))
        check(
            all(abs(e - EXACT) < 0.005 for e in e0s),
            "every seed within 0.005 of exact",
            "E0s: " + " ".join(f"{e:.6f}" for e in e0s),
        )
        check(
            abs(mean - EXACT) < 0.002 and 1e-5 < std < 0.005,
            "cross-seed mean/spread statistical",
            f"mean={mean:.6f} std={std:.6f} over {len(e0s)} seeds",
        )
        check(
            all(0.75 * x["target"] <= x["pop_mean"] <= 1.25 * x["target"]
                for x in runs),
            "population stable across seeds",
            "pop_means: " + " ".join(f"{x['pop_mean']:.0f}" for x in runs),
        )

        # --- timestep: constant total imaginary time tau*gens=30 ---
        errs = []
        for tau, gens, equil in [(0.04, 750, 125), (0.01, 3000, 500),
                                 (0.0025, 12000, 2000)]:
            r = run(binary, f"--tau={tau}", f"--gens={gens}",
                    f"--equil={equil}")
            errs.append(abs(r["E0"] - EXACT))
        check(
            all(e < 0.005 for e in errs),
            "all timesteps within 0.005 of exact",
            "errors at tau=0.04/0.01/0.0025: "
            + " ".join(f"{e:.6f}" for e in errs),
        )
        check(
            errs[2] <= errs[0] + 0.002,
            "no error growth at smaller timestep",
            f"tau=0.0025 err {errs[2]:.6f} vs tau=0.04 err {errs[0]:.6f}",
        )

        # --- entry-point validation ---
        bad = [
            ["--target=8001"],              # MAXW is 8000: initial fill overflows
            ["--tau=nan"], ["--tau=inf"],   # NaN/inf slip past tau <= 0.0
            ["--tau=0"], ["--target=0"], ["--gens=0"],
            ["--gens=10", "--equil=10"],
        ]
        rcs = [rc_of(binary, *a) for a in bad]
        check(
            all(rc != 0 for rc in rcs),
            "invalid parameters exit nonzero",
            "; ".join(f"{' '.join(a)} rc={rc}" for a, rc in zip(bad, rcs)),
        )
        r = run(binary, "--target=8000", "--gens=20", "--equil=10")
        check(
            0.75 * r["target"] <= r["pop_final"] <= 1.25 * r["target"],
            "boundary target == MAXW runs clean",
            f"pop_final={r['pop_final']} target={r['target']}",
        )

    print(f"{'ALL CHECKS PASSED' if failures == 0 else 'FAILURES'} "
          f"({failures} failure{'s' if failures != 1 else ''})")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
