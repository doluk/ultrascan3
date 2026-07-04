"""The 'mesh density tracks the band' figure: a space-time view of the
adaptive mesh over the C(r,t) field, plus an element-size companion."""

from __future__ import annotations

from typing import Sequence

from ._palette import GRID, INK_MUTED, INK_PRIMARY, INK_SECONDARY, sequential_blues, sequential_blue_cmap
from .sweep import series_label, series_style
from .trace_io import TraceRun


def plot_mesh_tracking(run: TraceRun, outpath: str, max_nodes: int = 200_000) -> None:
    """Headline figure: scatter of adaptive-mesh vertices (r, time), colored
    by concentration C, so mesh clustering visibly tracks the band peak.
    """
    import matplotlib.pyplot as plt

    nodes = run.nodes[run.nodes["is_midpoint"] == 0]
    if len(nodes) > max_nodes:
        nodes = nodes.sample(max_nodes, random_state=0).sort_values(["step", "r"])

    fig, ax = plt.subplots(figsize=(7, 5), facecolor="white")
    ax.set_facecolor("white")
    sc = ax.scatter(nodes["r"], nodes["time"], c=nodes["C"], s=3,
                    cmap=sequential_blue_cmap(), linewidths=0, zorder=2)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(INK_MUTED)
    ax.spines["bottom"].set_color(INK_MUTED)
    ax.tick_params(colors=INK_SECONDARY)
    ax.set_xlabel("r (cm)", color=INK_SECONDARY)
    ax.set_ylabel("time (s)", color=INK_SECONDARY)
    ax.set_title(f"Adaptive mesh tracking the band ({run.tag})",
                color=INK_PRIMARY, fontsize=11)

    cbar = fig.colorbar(sc, ax=ax)
    cbar.set_label("C(r,t)", color=INK_PRIMARY)
    cbar.ax.tick_params(colors=INK_SECONDARY)

    fig.tight_layout()
    fig.savefig(outpath, dpi=200, facecolor="white")
    plt.close(fig)


def plot_grids_vs_time(series_runs: dict, outpath: str, field: str = "Nv") -> None:
    """Number of mesh vertices (or elements) vs. time, one line per
    mesh-config series (e.g. the finest N run in each) -- shows adaptive
    series (refine=1) growing/shrinking the mesh as the band moves, versus
    a fixed series (refine=0) staying flat.
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

    for series, run in sorted(series_runs.items()):
        st = styles[series]
        ax.plot(run.steps["time"], run.steps[field], color=st["color"],
               linewidth=1.8, label=f"{series_label(series)} (N={run.N_init})", zorder=3)

    ax.set_xlabel("time (s)", color=INK_SECONDARY)
    ax.set_ylabel(f"number of grids ({field})", color=INK_PRIMARY)
    ax.set_title("Mesh size vs. time", color=INK_PRIMARY, fontsize=11)
    ax.legend(frameon=False, fontsize=8, labelcolor=INK_SECONDARY)

    fig.tight_layout()
    fig.savefig(outpath, dpi=200, facecolor="white")
    plt.close(fig)


def plot_elem_h_profile(run: TraceRun, times: Sequence[float], outpath: str) -> None:
    """Companion figure: local element size h(r) at selected times, ordered
    (sequential blue ramp) so 'later' reads as darker.
    """
    import matplotlib.pyplot as plt

    colors = sequential_blues(len(times))

    fig, ax = plt.subplots(figsize=(6, 4), facecolor="white")
    ax.set_facecolor("white")
    ax.grid(True, color=GRID, linewidth=0.8, zorder=0)
    ax.spines["top"].set_visible(False)
    ax.spines["right"].set_visible(False)
    ax.spines["left"].set_color(INK_MUTED)
    ax.spines["bottom"].set_color(INK_MUTED)
    ax.tick_params(colors=INK_SECONDARY)
    ax.set_yscale("log")

    for t, color in zip(times, colors):
        nodes = run.nodes_at_time(t, vertices_only=True)
        ax.plot(nodes["r"], nodes["elem_h"], color=color, linewidth=1.5,
               label=f"t={t:.0f}s", zorder=3)

    ax.set_xlabel("r (cm)", color=INK_SECONDARY)
    ax.set_ylabel("element size h (cm)", color=INK_PRIMARY)
    ax.set_title(f"Mesh resolution vs. radius ({run.tag})", color=INK_PRIMARY, fontsize=11)
    ax.legend(frameon=False, fontsize=8, labelcolor=INK_SECONDARY)

    fig.tight_layout()
    fig.savefig(outpath, dpi=200, facecolor="white")
    plt.close(fig)
