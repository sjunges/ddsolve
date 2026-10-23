```
██████╗ ██████╗ ███████╗ ██████╗ ██╗    ██╗   ██╗███████╗
██╔══██╗██╔══██╗██╔════╝██╔═══██╗██║    ██║   ██║██╔════╝
██║  ██║██║  ██║███████╗██║   ██║██║    ██║   ██║█████╗  
██║  ██║██║  ██║╚════██║██║   ██║██║    ╚██╗ ██╔╝██╔══╝  
██████╔╝██████╔╝███████║╚██████╔╝███████╗╚████╔╝ ███████╗
╚═════╝ ╚═════╝ ╚══════╝ ╚═════╝ ╚══════╝ ╚═══╝  ╚══════╝
═════════════════════════════════════════════════════════
```


# DDSOLVE

Documentation under construction...

## Replaying the scaling data points

`bench/scaling.py` runs the BRP and Rabin benchmarks on a single thread and prints, per instance,
the running time, the number of k-induction iterations, the number of nodes of the largest TEDD
before pruning (`peak`) and of the final TEDD (`final`), and the number of cases of the case
expression corresponding to the peak TEDD (`cases`, the leaves of the TEDD with all sharing unfolded).

```
python bench/scaling.py                        # quick preset, a few seconds
python bench/scaling.py --preset paper         # all sizes quoted in the paper
python bench/scaling.py --brp 20 --rabin 3     # custom sizes
```

For BRP, the size is the bound `n` on `toSend`; for Rabin it is the precision `p` of the bound `5/7 + 10^-p`.
Node counts are deterministic; running times depend on the machine.
