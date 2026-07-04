# us3utils

Post-processing for `US_LammAstfvm` ASTFVM solution traces (the Chapter 4
convergence and mesh-tracking figures, plus the mass-conservation check).
Consumes the `<tag>_c<comp>_trace_steps.csv` / `..._trace_nodes.csv` pairs
written by `US_LammAstfvm::setSolutionTrace()` (see the implementation
spec and `../astfvm_convergence` for the headless driver that produces them).

## Install

```sh
pip install -e .
```

## Use

```sh
python -m us3utils.cli /path/to/trace/dir --outdir figures --time 5400
```

This expects the driver's sweep-by-tag naming convention:
`N_<value>[_<series>]` for the spatial sweep and `dt_<value>[_<series>]`
for the temporal sweep -- see `../astfvm_convergence/README.md` for an
example sweep script. It writes:

- `convergence.png` -- log-log error vs. N and error vs. dt, with fitted
  slope annotations (Fig 1: reproduces the "dt matters more than N" result).
- `mass_drift.png` -- relative mass drift vs. time (one representative run
  per series; the numerical confirmation of exact mass conservation,
  companion to Fig 1).
- `grids_vs_time.png` -- number of mesh vertices (Nv) vs. time, one line
  per series -- adaptive (refine=1) series grow/shrink the mesh over time;
  a fixed (refine=0) series stays flat (companion to Fig 2).
- `error_vs_time.png` -- Linf error vs. the reference run, evaluated at
  each series' own recorded times (`--error-stride` to subsample for long
  runs).
- `mesh_tracking.png` -- space-time scatter of adaptive-mesh vertices,
  colored by C(r,t), showing the mesh clustering follow the band (Fig 2).
- `elem_h_profile.png` -- local element size vs. radius at several times
  (companion to Fig 2).

### Comparing mesh configurations (refine / uniform variations)

The optional `_<series>` tag suffix groups runs that vary a mesh
configuration alongside N or dt, e.g. tags `N_200_R0_U1`, `N_200_R1_U1`,
`N_200_R1_U0` (refine=0/1, uniform=0/1). Each distinct series is plotted
as its own line/color in `convergence.png` and `mass_drift.png`, so e.g. a
fixed-uniform-mesh sweep (`R0_U1`) and a fully adaptive sweep (`R1_U0`)
never get averaged or overplotted into one curve. A tag with no `_<series>`
suffix (e.g. plain `N_100`) is treated as a single implicit series.

The convergence error needs one shared "ground truth" run: by default
`us3utils` picks the finest N (or smallest dt, if there's no N-sweep) from
the `R1_U0` series if present (the fully adaptive, non-uniform default),
otherwise from whichever series reaches the largest N. Override with
`--reference-series R1_U1` (etc.) if you want a different one. The chosen
reference tag is printed to stdout so you can confirm which run every
curve is measured against.

## Library

The pieces are also usable directly:

```python
from us3utils import load_trace, load_sweep, self_convergence_reference, \
    compute_norms, plot_convergence, plot_mesh_tracking

runs = load_sweep("trace_dir")
finest = max(runs, key=lambda r: r.N_init)
ref = self_convergence_reference(finest, time=5400.0)
errs = [compute_norms(r, ref, time=5400.0) for r in runs if r is not finest]
```

`us3utils.reference.pure_diffusion_reference` provides an analytical
reference for the `s=0` case (see its docstring for the approximation's
validity range); `self_convergence_reference` treats the finest run in a
sweep as surrogate ground truth and works for the general (non-ideal)
band-forming case.

`error_over_time(run, ref_run, stride=1)` gives the L2/Linf error at each
of a run's own recorded times (used by `plot_error_vs_time`); `run.steps`
carries `Nv`/`Ne` directly (used by `plot_grids_vs_time`), no reference
solution needed for that one.

## Node indexing convention

`trace_nodes.csv` rows carry `is_midpoint` (0=vertex, 1=element midpoint),
matching the solver's `u[]` layout (`u[2j]`=vertex j, `u[2j+1]`=midpoint of
element j). Convergence and mesh-tracking figures use vertices only
(`is_midpoint == 0`); `TraceRun.nodes_at_time()` selects vertices by default.
