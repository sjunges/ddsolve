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
the running time, the number of k-induction iterations, the nodes of the largest TEDD before
pruning (`peak`) and of the final TEDD, and the number of cases of the largest TEDD (`cases`, the
leaves of the tree obtained by unfolding all sharing).

```
python bench/scaling.py
```

The sizes are set at the top of the script: for BRP the bound `n` on `toSend`, for Rabin the
precision `p` of the bound `5/7 + 10^-p`. Node counts are deterministic; running times depend on
the machine.
