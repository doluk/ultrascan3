"""us3utils: post-processing for US_LammAstfvm solution traces.

Parses the ``trace_steps.csv`` / ``trace_nodes.csv`` files written by
``US_LammAstfvm::setSolutionTrace()`` and renders the Chapter 4 (ASTFVM)
convergence and mesh-tracking figures.
"""

from .trace_io import TraceRun, load_trace
from .reference import pure_diffusion_reference, self_convergence_reference
from .convergence import (
    compute_norms, convergence_sweep, error_over_time, plot_convergence, plot_mass_drift,
    plot_convergence_by_series, plot_mass_drift_by_series, plot_error_vs_time,
)
from .mesh_tracking import plot_mesh_tracking, plot_elem_h_profile, plot_grids_vs_time
from .sweep import parse_tag, series_label, series_style, group_sweep

__all__ = [
    "TraceRun",
    "load_trace",
    "pure_diffusion_reference",
    "self_convergence_reference",
    "compute_norms",
    "convergence_sweep",
    "error_over_time",
    "plot_convergence",
    "plot_mass_drift",
    "plot_convergence_by_series",
    "plot_mass_drift_by_series",
    "plot_error_vs_time",
    "plot_mesh_tracking",
    "plot_elem_h_profile",
    "plot_grids_vs_time",
    "parse_tag",
    "series_label",
    "series_style",
    "group_sweep",
]
