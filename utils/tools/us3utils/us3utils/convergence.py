"""Error norms, convergence sweeps, and the convergence + mass-drift figures."""

from __future__ import annotations

from typing import Callable, Sequence

import numpy as np

from ._palette import CAT_BLUE, CAT_RED, GRID, INK_MUTED, INK_PRIMARY, INK_SECONDARY, sequential_blues
from .sweep import series_label, series_style
from .trace_io import TraceRun

# numpy >= 2.0 renamed trapz -> trapezoid; support both.
_trapz = getattr(np, "trapezoid", None) or np.trapz


def compute_norms(run: TraceRun, ref_fn: Callable[[np.ndarray], np.ndarray],
                  time: float) -> dict:
    """L2 and Linf error of a run's vertex solution vs. a reference C(r) at
    the closest recorded step to ``time``. L2 is trapezoid-weighted over r.
    """
    nodes = run.nodes_at_time(time, vertices_only=True)
    r = nodes["r"].to_numpy()
    c_num = nodes["C"].to_numpy()
    c_ref = ref_fn(r)
    err = c_num - c_ref

    linf = float(np.max(np.abs(err)))
    l2 = float(np.sqrt(_trapz(err ** 2, r) / (r[-1] - r[0])))
    return {"time": time, "N": run.N_init, "dt": run.dt, "L2": l2, "Linf": linf}


def convergence_sweep(runs: Sequence[TraceRun], ref_fn: Callable[[np.ndarray], np.ndarray],
                      time: float, sweep_key: str = "N") -> list:
    """Compute norms for a list of runs and sort by the sweep variable."""
    rows = [compute_norms(r, ref_fn, time) for r in runs]
    rows.sort(key=lambda row: row[sweep_key])
    return rows


def _fit_slope(xs: np.ndarray, ys: np.ndarray) -> float:
    """Log-log least-squares slope (the empirical convergence order)."""
    mask = (xs > 0) & (ys > 0)
    if mask.sum() < 2:
        return float("nan")
    lx, ly = np.log(xs[mask]), np.log(ys[mask])
    slope, _ = np.polyfit(lx, ly, 1)
    return float(slope)


def plot_convergence(sweep_N: list, sweep_dt: list, outpath: str,
                     norm: str = "L2", s_label: str = "") -> None:
    """Two-panel log-log convergence figure: error vs N (refine off, dt
    fixed) and error vs dt (N fixed). Panels are small multiples, not a
    dual-axis plot. Slopes are annotated directly on each curve.
    """
    import matplotlib.pyplot as plt

    fig, (ax_n, ax_dt) = plt.subplots(1, 2, figsize=(9, 4.2), facecolor="white")

    for ax in (ax_n, ax_dt):
        ax.set_facecolor("white")
        ax.grid(True, which="both", color=GRID, linewidth=0.8, zorder=0)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.spines["left"].set_color(INK_MUTED)
        ax.spines["bottom"].set_color(INK_MUTED)
        ax.tick_params(colors=INK_SECONDARY)
        ax.set_xscale("log")
        ax.set_yscale("log")

    if sweep_N:
        ns = np.array([row["N"] for row in sweep_N], dtype=float)
        errs = np.array([row[norm] for row in sweep_N], dtype=float)
        slope = _fit_slope(ns, errs)
        ax_n.plot(ns, errs, "-o", color=CAT_BLUE, linewidth=2, markersize=7,
                 markeredgecolor="white", markeredgewidth=0.8, zorder=3)
        ax_n.annotate(f"slope ≈ {slope:.2f}", xy=(ns[-1], errs[-1]),
                     xytext=(6, 0), textcoords="offset points",
                     color=CAT_BLUE, fontsize=9, va="center")
    ax_n.set_xlabel("N (elements, mesh refine off)", color=INK_SECONDARY)
    ax_n.set_ylabel(f"{norm} error{(' (' + s_label + ')') if s_label else ''}",
                    color=INK_PRIMARY)
    ax_n.set_title("Spatial convergence", color=INK_PRIMARY, fontsize=11)

    if sweep_dt:
        dts = np.array([row["dt"] for row in sweep_dt], dtype=float)
        errs = np.array([row[norm] for row in sweep_dt], dtype=float)
        slope = _fit_slope(dts, errs)
        ax_dt.plot(dts, errs, "-o", color=CAT_RED, linewidth=2, markersize=7,
                  markeredgecolor="white", markeredgewidth=0.8, zorder=3)
        ax_dt.annotate(f"slope ≈ {slope:.2f}", xy=(dts[-1], errs[-1]),
                      xytext=(6, 0), textcoords="offset points",
                      color=CAT_RED, fontsize=9, va="center")
    ax_dt.set_xlabel("Δt (s, N fixed)", color=INK_SECONDARY)
    ax_dt.set_title("Temporal convergence", color=INK_PRIMARY, fontsize=11)

    fig.tight_layout()
    fig.savefig(outpath, dpi=200, facecolor="white")
    plt.close(fig)


def plot_convergence_by_series(series_sweeps_N: dict, series_sweeps_dt: dict,
                               outpath: str, norm: str = "L2", s_label: str = "") -> None:
    """Two-panel log-log convergence figure with one line per mesh-config
    series (e.g. refine/uniform combinations), so those variations read as
    distinct, consistently colored/marked curves rather than one averaged
    or overplotted line. ``series_sweeps_N``/``series_sweeps_dt`` map
    series label -> list of ``compute_norms()`` rows (see
    ``convergence_sweep``), already sorted by the sweep variable.
    """
    import matplotlib.pyplot as plt

    fig, (ax_n, ax_dt) = plt.subplots(1, 2, figsize=(10.5, 4.5), facecolor="white")

    for ax in (ax_n, ax_dt):
        ax.set_facecolor("white")
        ax.grid(True, which="both", color=GRID, linewidth=0.8, zorder=0)
        ax.spines["top"].set_visible(False)
        ax.spines["right"].set_visible(False)
        ax.spines["left"].set_color(INK_MUTED)
        ax.spines["bottom"].set_color(INK_MUTED)
        ax.tick_params(colors=INK_SECONDARY)
        ax.set_xscale("log")
        ax.set_yscale("log")

    all_series = sorted(set(series_sweeps_N) | set(series_sweeps_dt))
    styles = series_style(all_series)

    for series, rows in sorted(series_sweeps_N.items()):
        if not rows:
            continue
        st = styles[series]
        ns = np.array([row["N"] for row in rows], dtype=float)
        errs = np.array([row[norm] for row in rows], dtype=float)
        slope = _fit_slope(ns, errs)
        label = f"{series_label(series)} (slope≈{slope:.2f})"
        ax_n.plot(ns, errs, "-", marker=st["marker"], color=st["color"],
                 linewidth=2, markersize=6, markeredgecolor="white",
                 markeredgewidth=0.6, label=label, zorder=3)
    ax_n.set_xlabel("N (elements)", color=INK_SECONDARY)
    ax_n.set_ylabel(f"{norm} error{(' (' + s_label + ')') if s_label else ''}",
                    color=INK_PRIMARY)
    ax_n.set_title("Spatial convergence", color=INK_PRIMARY, fontsize=11)
    if series_sweeps_N:
        ax_n.legend(frameon=False, fontsize=8, labelcolor=INK_SECONDARY, loc="best")

    for series, rows in sorted(series_sweeps_dt.items()):
        if not rows:
            continue
        st = styles[series]
        dts = np.array([row["dt"] for row in rows], dtype=float)
        errs = np.array([row[norm] for row in rows], dtype=float)
        slope = _fit_slope(dts, errs)
        label = f"{series_label(series)} (slope≈{slope:.2f})"
        ax_dt.plot(dts, errs, "-", marker=st["marker"], color=st["color"],
                  linewidth=2, markersize=6, markeredgecolor="white",
                  markeredgewidth=0.6, label=label, zorder=3)
    ax_dt.set_xlabel("Δt (s)", color=INK_SECONDARY)
    ax_dt.set_title("Temporal convergence", color=INK_PRIMARY, fontsize=11)
    if series_sweeps_dt:
        ax_dt.legend(frameon=False, fontsize=8, labelcolor=INK_SECONDARY, loc="best")

    fig.tight_layout()
    fig.savefig(outpath, dpi=200, facecolor="white")
    plt.close(fig)


def plot_mass_drift_by_series(series_runs: dict, outpath: str) -> None:
    """Companion figure: relative mass drift vs. time, one representative
    run per mesh-config series (e.g. the finest N in that series), colored
    to match ``plot_convergence_by_series``.
    """
    import matplotlib.pyplot as plt

    styles = series_style(series_runs.keys())

    fig, ax = plt.subplots(figsize=(6.5, 4.2), facecolor="white")
    ax.set_facecolor("white")
    ax.grid(True, color=GRID, linewidth=0.8, zorder=0)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(INK_MUTED)
    ax.spines["bottom"].set_color(INK_MUTED)
    ax.tick_params(colors=INK_SECONDARY)
    ax.set_yscale("log")

    for series, run in sorted(series_runs.items()):
        st = styles[series]
        ax.plot(run.steps["time"], run.steps["mass_rel_drift"].clip(lower=1e-18),
               color=st["color"], linewidth=1.8,
               label=f"{series_label(series)} (N={run.N_init})", zorder=3)

    ax.set_xlabel("time (s)", color=INK_SECONDARY)
    ax.set_ylabel("|mass drift| / mass$_0$", color=INK_PRIMARY)
    ax.set_title("Mass conservation", color=INK_PRIMARY, fontsize=11)
    ax.legend(frameon=False, fontsize=8, labelcolor=INK_SECONDARY)

    fig.tight_layout()
    fig.savefig(outpath, dpi=200, facecolor="white")
    plt.close(fig)


def plot_mass_drift(runs: Sequence[TraceRun], outpath: str) -> None:
    """Companion figure: relative mass drift vs. time for a set of runs,
    colored by N along the sequential blue ramp (an ordered quantity).
    """
    import matplotlib.pyplot as plt

    runs_sorted = sorted(runs, key=lambda r: r.N_init)
    colors = sequential_blues(len(runs_sorted))

    fig, ax = plt.subplots(figsize=(6, 4), facecolor="white")
    ax.set_facecolor("white")
    ax.grid(True, color=GRID, linewidth=0.8, zorder=0)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(INK_MUTED)
    ax.spines["bottom"].set_color(INK_MUTED)
    ax.tick_params(colors=INK_SECONDARY)
    ax.set_yscale("log")

    for run, color in zip(runs_sorted, colors):
        ax.plot(run.steps["time"], run.steps["mass_rel_drift"].clip(lower=1e-18),
               color=color, linewidth=1.5, label=f"N={run.N_init}", zorder=3)

    ax.set_xlabel("time (s)", color=INK_SECONDARY)
    ax.set_ylabel("|mass drift| / mass$_0$", color=INK_PRIMARY)
    ax.set_title("Mass conservation", color=INK_PRIMARY, fontsize=11)
    ax.legend(frameon=False, fontsize=8, labelcolor=INK_SECONDARY)

    fig.tight_layout()
    fig.savefig(outpath, dpi=200, facecolor="white")
    plt.close(fig)
