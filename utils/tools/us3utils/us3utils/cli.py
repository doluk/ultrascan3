"""Command-line entry point: render the two Chapter 4 ASTFVM figures from a
directory of trace CSVs written by US_LammAstfvm::setSolutionTrace().

Expects the driver's sweep-by-tag convention: tags starting with "N_" for
the spatial (N) sweep and "dt_" for the temporal (dt) sweep (see the
example in ../astfvm_convergence/README.md). Any other tag is used only
for the mesh-tracking figure (pass --mesh-tag to pick one explicitly).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .convergence import convergence_sweep, plot_convergence, plot_mass_drift
from .mesh_tracking import plot_elem_h_profile, plot_mesh_tracking
from .reference import self_convergence_reference
from .trace_io import load_sweep


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("tracedir", help="Directory containing *_trace_steps.csv / *_trace_nodes.csv")
    p.add_argument("--outdir", default="figures", help="Directory to write PNGs to")
    p.add_argument("--time", type=float, required=True,
                   help="Observation time (s) at which to evaluate convergence error")
    p.add_argument("--mesh-tag", default=None,
                   help="Tag of the run to use for the mesh-tracking figure "
                        "(default: the finest N-sweep run)")
    args = p.parse_args(argv)

    tracedir = Path(args.tracedir)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    all_runs = load_sweep(tracedir)
    if not all_runs:
        print(f"No trace files found under {tracedir}", file=sys.stderr)
        return 1

    n_runs = [r for r in all_runs if r.tag.startswith("N_")]
    dt_runs = [r for r in all_runs if r.tag.startswith("dt_")]

    reference_pool = n_runs or all_runs
    finest = max(reference_pool, key=lambda r: r.N_init)
    ref_fn = self_convergence_reference(finest, args.time)

    sweep_n = convergence_sweep([r for r in n_runs if r is not finest], ref_fn,
                                args.time, sweep_key="N")
    sweep_dt = convergence_sweep(dt_runs, ref_fn, args.time, sweep_key="dt")

    plot_convergence(sweep_n, sweep_dt, str(outdir / "convergence.png"))
    print(f"wrote {outdir / 'convergence.png'}")

    if n_runs:
        plot_mass_drift(n_runs, str(outdir / "mass_drift.png"))
        print(f"wrote {outdir / 'mass_drift.png'}")

    mesh_run = None
    if args.mesh_tag:
        mesh_run = next((r for r in all_runs if r.tag == args.mesh_tag), None)
    mesh_run = mesh_run or finest
    plot_mesh_tracking(mesh_run, str(outdir / "mesh_tracking.png"))
    print(f"wrote {outdir / 'mesh_tracking.png'}")

    t_end = float(mesh_run.steps["time"].iloc[-1])
    sample_times = [t_end * f for f in (0.1, 0.4, 0.7, 1.0)]
    plot_elem_h_profile(mesh_run, sample_times, str(outdir / "elem_h_profile.png"))
    print(f"wrote {outdir / 'elem_h_profile.png'}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
