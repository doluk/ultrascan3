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

Writes: convergence.png, mass_drift.png, grids_vs_time.png (number of
mesh vertices vs. time, one line per series), error_vs_time.png (Linf
error vs. the reference run at each of the run's own recorded times),
mesh_tracking.png, elem_h_profile.png, concentration_profiles.png. Pass
--compare-series to additionally render grids_vs_time_compare.png /
error_vs_time_compare.png comparing specific dt (or N) values *within*
one series, matching the classic 3-dt convergence figure.
"""

from __future__ import annotations

import argparse
import sys
from pathlib import Path

from .convergence import (
    convergence_sweep, plot_convergence_by_series, plot_mass_drift_by_series, plot_error_vs_time,
    plot_error_vs_time_multi,
)
from .mesh_tracking import (
    plot_concentration_profiles, plot_elem_h_profile, plot_grids_vs_time,
    plot_grids_vs_time_multi, plot_mesh_tracking,
)
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
    p.add_argument("--time", type=float, default=None,
                   help="Observation time (s) at which to evaluate convergence.png's "
                        "single-snapshot error. Default: 70%% of the reference run's "
                        "duration (past the initial transient; error_vs_time.png shows "
                        "the full trajectory so this single value only drives the "
                        "log-log slope summary in convergence.png).")
    p.add_argument("--reference-series", default=None,
                   help="Series (e.g. 'R1_U0') to draw the ground-truth run from; "
                        "default prefers 'R1_U0' if present, else the series with the largest N")
    p.add_argument("--mesh-tag", default=None,
                   help="Tag of the run to use for the mesh-tracking figure "
                        "(default: the reference run)")
    p.add_argument("--error-stride", type=int, default=1,
                   help="Subsample a run's own recorded times by this stride when "
                        "computing error_vs_time.png (default: every step)")
    p.add_argument("--compare-series", default=None,
                   help="Series (e.g. 'R1_U0') whose individual dt (or N) values to "
                        "compare directly, one line per value, matching the classic "
                        "3-dt convergence figure. Off by default.")
    p.add_argument("--compare-kind", choices=["dt", "N"], default="dt",
                   help="Which swept variable to compare within --compare-series (default: dt)")
    p.add_argument("--profile-tag", default=None,
                   help="Tag of the run to use for concentration_profiles.png "
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

    time = args.time
    if time is None:
        t_end = float(reference.steps["time"].iloc[-1])
        time = 0.7 * t_end
        print(f"--time not given; defaulting to 70% of the reference run's "
              f"duration = {time:.4g}s (of {t_end:.4g}s total)")
    ref_fn = self_convergence_reference(reference, time)

    series_sweeps_N = {}
    for series, pairs in n_groups.items():
        runs = [run for _, run in pairs if run is not reference]
        if runs:
            series_sweeps_N[series] = convergence_sweep(runs, ref_fn, time, sweep_key="N")

    series_sweeps_dt = {}
    for series, pairs in dt_groups.items():
        runs = [run for _, run in pairs if run is not reference]
        if runs:
            series_sweeps_dt[series] = convergence_sweep(runs, ref_fn, time, sweep_key="dt")

    plot_convergence_by_series(series_sweeps_N, series_sweeps_dt,
                               str(outdir / "convergence.png"))
    print(f"wrote {outdir / 'convergence.png'}")

    # One representative (finest) run per series, across whichever of the
    # N-/dt-sweeps that series appears in -- shared by the mass-drift,
    # grids-vs-time, and error-vs-time companion figures.
    series_runs = {}
    for series, pairs in n_groups.items():
        series_runs[series] = pairs[-1][1]  # largest N
    for series, pairs in dt_groups.items():
        series_runs.setdefault(series, pairs[0][1])  # smallest dt

    if series_runs:
        plot_mass_drift_by_series(series_runs, str(outdir / "mass_drift.png"))
        print(f"wrote {outdir / 'mass_drift.png'}")

        plot_grids_vs_time(series_runs, str(outdir / "grids_vs_time.png"))
        print(f"wrote {outdir / 'grids_vs_time.png'}")

        plot_error_vs_time(series_runs, reference, str(outdir / "error_vs_time.png"),
                          stride=args.error_stride)
        print(f"wrote {outdir / 'error_vs_time.png'}")

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

    profile_run = None
    if args.profile_tag:
        profile_run = next((r for r in all_runs if r.tag == args.profile_tag), None)
    profile_run = profile_run or reference
    profile_t_end = float(profile_run.steps["time"].iloc[-1])
    profile_times = [profile_t_end * f for f in (0.02, 0.05, 0.1, 0.2, 0.35, 0.5, 0.7, 1.0)]
    plot_concentration_profiles(profile_run, profile_times, str(outdir / "concentration_profiles.png"))
    print(f"wrote {outdir / 'concentration_profiles.png'}")

    # Optional: compare specific dt (or N) values within one series directly,
    # matching the classic 3-dt convergence figure (same color, marker per value).
    if args.compare_series:
        groups = dt_groups if args.compare_kind == "dt" else n_groups
        value_runs = groups.get(args.compare_series)
        if value_runs is None:
            print(f"--compare-series {args.compare_series!r} has no {args.compare_kind}_* "
                  f"runs; skipping compare figures", file=sys.stderr)
        else:
            plot_grids_vs_time_multi(value_runs, str(outdir / "grids_vs_time_compare.png"),
                                     value_label=args.compare_kind)
            print(f"wrote {outdir / 'grids_vs_time_compare.png'}")

            plot_error_vs_time_multi(value_runs, reference, str(outdir / "error_vs_time_compare.png"),
                                     value_label=args.compare_kind, stride=args.error_stride)
            print(f"wrote {outdir / 'error_vs_time_compare.png'}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
