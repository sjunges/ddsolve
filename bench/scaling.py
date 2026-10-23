"""Replay the TEDD scaling data points for the BRP and Rabin benchmarks.

For every instance we run the verifier on a single thread and report the running time,
the number of k-induction iterations, the nodes of the largest TEDD before pruning (peak)
and of the final TEDD, and the number of cases of the largest TEDD (the leaves of the tree
obtained by unfolding all sharing).

Usage (from the repository root): python bench/scaling.py
"""

import os
import sys
import time

# Must be set before the extension module is imported.
os.environ.setdefault("LACE_WORKERS", "1")
sys.setrecursionlimit(10_000_000)

from ddsolve._ddsolve import VariableOrder  # noqa: E402
from ddsolve.parser import parse  # noqa: E402
from ddsolve.wp import Verifier  # noqa: E402

# BRP: the bound n on toSend. Rabin: the precision p of the bound 5/7 + 10^-p.
BRP_SIZES = [10, 20, 30, 40, 50]
RABIN_SIZES = [1, 2, 3, 5, 7, 10]

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


def run(name, source):
    order = VariableOrder()
    ast, _ = parse(source, order)
    verifier = Verifier(order)

    start = time.perf_counter()
    verifier.verify(ast)
    seconds = time.perf_counter() - start

    print(
        f"{name:10} {seconds:8.2f} {verifier.iterations:6d} "
        f"{verifier.peak_nodecount:9d} {verifier.post_expectation.get_nodecount():7d} "
        f"{verifier.peak_case_count:>20}",
        flush=True,
    )


print(f"{'instance':10} {'time[s]':>8} {'iters':>6} {'peak':>9} {'final':>7} {'cases':>20}")
for n in BRP_SIZES:
    run(f"brp {n}", BRP.format(n=n))
for p in RABIN_SIZES:
    run(f"rabin {p}", RABIN.format(p=10**p))
