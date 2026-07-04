"""Command-line entry point: render the two Chapter 4 ASTFVM figures from a
directory of trace CSVs written by US_LammAstfvm::setSolutionTrace().

Expects the driver's sweep-by-tag convention: ``N_<value>[_<series>]`` for
the spatial (N) sweep and ``dt_<value>[_<series>]`` for the temporal (dt)
sweep (see ../astfvm_convergence/README.md). The optional ``_<series>``
suffix (e.g. ``R0_U1`` for refine=0/uniform=1) groups runs that vary a
mesh configuration alongside N or dt; each series is plotted as its own
line so refine/uniform variations are never averaged or overplotted
together. Tags without a recognized ``N_``/``dt_`` prefix (e.g. a one-off
``band_mesh`` run) are not part of a sweep and are used only for the
mesh-tracking figure (pass --mesh-tag to pick one explicitly).
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .convergence import convergence_sweep, plot_convergence_by_series, plot_mass_drift_by_series
from .mesh_tracking import plot_elem_h_profile, plot_mesh_tracking
from .reference import self_convergence_reference
from .sweep import group_sweep
from .trace_io import load_sweep


def _pick_reference(n_groups: dict, dt_groups: dict, reference_series: str | None):
    """Pick the run to treat as ground truth: the finest N in the chosen
    (or auto-selected) series, falling back to the smallest dt if there's
    no N-sweep at all.
    """
    if n_groups:
        series = reference_series if reference_series in n_groups else (
            "R1_U0" if "R1_U0" in n_groups else max(n_groups, key=lambda s: n_groups[s][-1][0]))
        return series, n_groups[series][-1][1]  # largest N (list is sorted ascending)
    if dt_groups:
        series = reference_series if reference_series in dt_groups else (
            "R1_U0" if "R1_U0" in dt_groups else next(iter(dt_groups)))
        return series, dt_groups[series][0][1]  # smallest dt
    return None, None


def main(argv=None) -> int:
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("tracedir", help="Directory containing *_trace_steps.csv / *_trace_nodes.csv")
    p.add_argument("--outdir", default="figures", help="Directory to write PNGs to")
    p.add_argument("--time", type=float, required=True,
                   help="Observation time (s) at which to evaluate convergence error")
    p.add_argument("--reference-series", default=None,
                   help="Series (e.g. 'R1_U0') to draw the ground-truth run from; "
                        "default prefers 'R1_U0' if present, else the series with the largest N")
    p.add_argument("--mesh-tag", default=None,
                   help="Tag of the run to use for the mesh-tracking figure "
                        "(default: the reference run)")
    args = p.parse_args(argv)

    tracedir = Path(args.tracedir)
    outdir = Path(args.outdir)
    outdir.mkdir(parents=True, exist_ok=True)

    all_runs = load_sweep(tracedir)
    if not all_runs:
        print(f"No trace files found under {tracedir}", file=sys.stderr)
        return 1

    n_groups = group_sweep(all_runs, "N")
    dt_groups = group_sweep(all_runs, "dt")

    ref_series, reference = _pick_reference(n_groups, dt_groups, args.reference_series)
    if reference is None:
        print("No N_* or dt_* sweep tags found; nothing to compute convergence against.",
              file=sys.stderr)
        return 1
    print(f"reference run: tag={reference.tag} (series={ref_series})")
    ref_fn = self_convergence_reference(reference, args.time)

    series_sweeps_N = {}
    for series, pairs in n_groups.items():
        runs = [run for _, run in pairs if run is not reference]
        if runs:
            series_sweeps_N[series] = convergence_sweep(runs, ref_fn, args.time, sweep_key="N")

    series_sweeps_dt = {}
    for series, pairs in dt_groups.items():
        runs = [run for _, run in pairs if run is not reference]
        if runs:
            series_sweeps_dt[series] = convergence_sweep(runs, ref_fn, args.time, sweep_key="dt")

    plot_convergence_by_series(series_sweeps_N, series_sweeps_dt,
                               str(outdir / "convergence.png"))
    print(f"wrote {outdir / 'convergence.png'}")

    # Mass drift: one representative (finest) run per series, across
    # whichever of the N-/dt-sweeps that series appears in.
    mass_series_runs = {}
    for series, pairs in n_groups.items():
        mass_series_runs[series] = pairs[-1][1]  # largest N
    for series, pairs in dt_groups.items():
        mass_series_runs.setdefault(series, pairs[0][1])  # smallest dt
    if mass_series_runs:
        plot_mass_drift_by_series(mass_series_runs, str(outdir / "mass_drift.png"))
        print(f"wrote {outdir / 'mass_drift.png'}")

    mesh_run = None
    if args.mesh_tag:
        mesh_run = next((r for r in all_runs if r.tag == args.mesh_tag), None)
    mesh_run = mesh_run or reference
    plot_mesh_tracking(mesh_run, str(outdir / "mesh_tracking.png"))
    print(f"wrote {outdir / 'mesh_tracking.png'}")

    t_end = float(mesh_run.steps["time"].iloc[-1])
    sample_times = [t_end * f for f in (0.1, 0.4, 0.7, 1.0)]
    plot_elem_h_profile(mesh_run, sample_times, str(outdir / "elem_h_profile.png"))
    print(f"wrote {outdir / 'elem_h_profile.png'}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
