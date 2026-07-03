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

This expects the driver's sweep-by-tag naming convention: tags starting
with `N_` are the spatial (mesh refine off, dt fixed) sweep and `dt_` are
the temporal (N fixed) sweep -- see `../astfvm_convergence/README.md` for
an example sweep script. It writes:

- `convergence.png` -- log-log error vs. N and error vs. dt, with fitted
  slope annotations (Fig 1: reproduces the "dt matters more than N" result).
- `mass_drift.png` -- relative mass drift vs. time per run (the numerical
  confirmation of exact mass conservation, companion to Fig 1).
- `mesh_tracking.png` -- space-time scatter of adaptive-mesh vertices,
  colored by C(r,t), showing the mesh clustering follow the band (Fig 2).
- `elem_h_profile.png` -- local element size vs. radius at several times
  (companion to Fig 2).

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

## Node indexing convention

`trace_nodes.csv` rows carry `is_midpoint` (0=vertex, 1=element midpoint),
matching the solver's `u[]` layout (`u[2j]`=vertex j, `u[2j+1]`=midpoint of
element j). Convergence and mesh-tracking figures use vertices only
(`is_midpoint == 0`); `TraceRun.nodes_at_time()` selects vertices by default.
