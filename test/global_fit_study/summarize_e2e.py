#!/usr/bin/env python3
"""Aggregate run_e2e.py results:  mean and SD over seeds per scenario,
binary and output model.

Usage:  summarize_e2e.py results.json [more.json ...]
"""
import json
import sys
from collections import defaultdict

import statistics as st


def ms(vals):
    vals = [v for v in vals if v == v]
    if not vals:
        return "nan"
    sd = st.stdev(vals) if len(vals) > 1 else 0.0
    return "%+.4f ± %.4f" % (st.mean(vals), sd)


def main():
    rows = []
    for fn in sys.argv[1:]:
        rows += json.load(open(fn))
    groups = defaultdict(list)
    for r in rows:
        for model, sc in r["scores"].items():
            groups[(r["scenario"], model, r["program"])].append(sc)
    print("| scenario | model | binary | n | RMSD reported (true σ) | total rel. err | max |Δ fraction| | s̄ rel. err | aggregate signal | solutes |")
    print("|---|---|---|---|---|---|---|---|---|---|")
    for (sn, model, prog) in sorted(groups):
        g = groups[(sn, model, prog)]
        print("| %s | %s | %s | %d | %.5f (%.4f) | %s | %s | %s | %.4f | %.1f |" % (
            sn, model, prog, len(g),
            st.mean(x["rmsd_reported"] for x in g), g[0]["rmsd_true_noise"],
            ms([x["total_rel_err"] for x in g]),
            ms([x["frac_abs_err"] for x in g]),
            ms([x["s_mean_rel_err"] for x in g]),
            st.mean(x["agg_signal"] for x in g),
            st.mean(x["nsolutes"] for x in g)))


if __name__ == "__main__":
    main()
