"""Replay the TEDD scaling data points for the BRP and Rabin benchmarks.

For every instance we run the verifier on a single thread and report

  * wall-clock time and number of k-induction iterations,
  * nodes of the largest TEDD before pruning (``peak``) and of the final TEDD (``final``),
  * the number of cases of the case expression that corresponds to the peak TEDD, i.e.,
    the number of leaves of the tree obtained by unfolding all sharing (``cases``).

Usage (from the repository root):

    python bench/scaling.py                       # quick preset, seconds
    python bench/scaling.py --preset paper        # all data points quoted in the paper
    python bench/scaling.py --brp 10 20 --rabin 3 # custom sizes
    python bench/scaling.py --csv out.csv
"""

import argparse
import csv
import os
import sys
import time

# Must be set before the extension module is imported.
os.environ.setdefault("LACE_WORKERS", "1")
sys.setrecursionlimit(10_000_000)

from ddsolve._ddsolve import VariableOrder  # noqa: E402
from ddsolve.parser import parse  # noqa: E402
import ddsolve.wp as wp  # noqa: E402

BRP = """\
// wp <= ite(toSend <= {n}, totalFailed + 10, infty)

@kinduction(ite(toSend <= {n}, totalFailed + 10, infty))
while(failed < maxFailed & sent < toSend){{
    {{ failed := 0; sent := sent + 1 }}
    [9/10]
    {{ failed := failed + 1; totalFailed := totalFailed + 1 }}
}}

// totalFailed
"""

RABIN = """\
// wp <= ite(1<i & i<4 & phase==0, (5/7) + (1/{p}), 1)

@kinduction(ite(1<i & i<4 & phase==0, (5/7) + (1/{p}), 1))
while(1<i | phase==1){{
    if(phase==0){{
        n:=i;
        phase:=1;
    }} else {{
        if(0<n){{
            {{d:=0}}[1/2]{{d:=1}};
            i:=i-d;
            n:=n-1;
        }} else {{
            phase:=0;
        }}
    }}
}}

// ite(i == 1, 1, 0)
"""

PRESETS = {
    "quick": {"brp": [5, 10, 15, 20], "rabin": [1, 2, 3, 5]},
    "paper": {"brp": [10, 20, 30, 40, 50], "rabin": [1, 2, 3, 5, 7, 10]},
}


def program(bench, size):
    """Source text of the benchmark ``bench`` at parameter ``size``."""
    if bench == "brp":
        return BRP.format(n=size)
    return RABIN.format(p=10**size)


def run(bench, size, cases):
    """Verify one instance and return its measurements."""
    order = VariableOrder()
    ast, _ = parse(program(bench, size), order)
    verifier = wp.Verifier(order)

    start = time.perf_counter()
    result = verifier.verify(ast)
    seconds = time.perf_counter() - start

    return {
        "bench": bench,
        "size": size,
        "result": result,
        "seconds": round(seconds, 2),
        "iterations": verifier.iterations,
        "peak": verifier.peak_nodecount,
        "final": verifier.post_expectation.get_nodecount(),
        "cases": verifier.peak_case_count if cases else None,
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__.split("\n\n")[0])
    parser.add_argument("--preset", choices=PRESETS, default="quick")
    parser.add_argument("--brp", type=int, nargs="*", metavar="N", help="BRP bounds n")
    parser.add_argument(
        "--rabin", type=int, nargs="*", metavar="P", help="Rabin precisions"
    )
    parser.add_argument(
        "--no-cases", action="store_true", help="skip the case count of the peak TEDD"
    )
    parser.add_argument("--csv", metavar="FILE", help="also write the table as CSV")
    args = parser.parse_args()

    sizes = {
        "brp": PRESETS[args.preset]["brp"] if args.brp is None else args.brp,
        "rabin": PRESETS[args.preset]["rabin"] if args.rabin is None else args.rabin,
    }

    header = f"{'bench':6} {'size':>4} {'result':>6} {'time[s]':>8} {'iters':>5} {'peak':>9} {'final':>7} {'cases':>16}"
    print(f"threads: LACE_WORKERS={os.environ['LACE_WORKERS']}")
    print(header)
    rows = []
    for bench, values in sizes.items():
        for size in values:
            row = run(bench, size, not args.no_cases)
            rows.append(row)
            print(
                f"{bench:6} {size:4d} {str(row['result']):>6} {row['seconds']:8.2f} "
                f"{row['iterations']:5d} {row['peak']:9d} {row['final']:7d} "
                f"{row['cases'] if row['cases'] is not None else '-':>16}",
                flush=True,
            )

    if args.csv:
        with open(args.csv, "w", newline="") as f:
            writer = csv.DictWriter(
                f,
                fieldnames=[
                    "bench", "size", "result", "seconds", "iterations",
                    "peak", "final", "cases",
                ],
            )
            writer.writeheader()
            writer.writerows(rows)


if __name__ == "__main__":
    main()
