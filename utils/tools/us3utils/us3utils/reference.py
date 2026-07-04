"""Reference solutions for the error axis of the convergence figure.

Two strategies (see the implementation spec, Sec. 5):

1. ``pure_diffusion_reference`` -- an analytical, closed-form approximation
   for the s=0 (pure diffusion) case, giving an unambiguous convergence
   rate. It treats the radial diffusion locally as 1-D (superposition of
   error functions for a rectangular initial band), which is accurate when
   the diffusion boundary-layer width is small relative to the radius --
   true for the short-time / high-resolution regime used in a convergence
   study, but it does **not** include the reflecting meniscus/bottom
   boundaries or the (1/r) curvature term of the full cylindrical Lamm
   operator. Use it only for the s=0 clean-rate check; use
   ``self_convergence_reference`` for the general (non-ideal, s != 0) case.

2. ``self_convergence_reference`` -- treats the finest available run in a
   sweep as surrogate ground truth and interpolates it onto arbitrary radii.
"""

from __future__ import annotations

import math

import numpy as np

from .trace_io import TraceRun

_erf = np.vectorize(math.erf)


def pure_diffusion_reference(r: np.ndarray, t: float, D: float,
                             a: float, b: float, C0: float = 1.0) -> np.ndarray:
    """C(r, t) for an initial rectangular band of concentration C0 on [a, b],
    diffusing with coefficient D, approximated as a 1-D free-space problem
    (see module docstring for the approximation's validity range).
    """
    if t <= 0.0:
        return np.where((r >= a) & (r <= b), C0, 0.0)
    denom = 2.0 * np.sqrt(D * t)
    return 0.5 * C0 * (_erf((r - a) / denom) - _erf((r - b) / denom))


def self_convergence_reference(finest_run: TraceRun, time: float):
    """Return a callable C_ref(r) for the finest run at ``time``, built by
    linearly blending the two recorded steps bracketing ``time`` in time
    (each first interpolated in r) -- the same f0/f1 blend the solver
    itself uses for output scans. Snapping to only the nearest recorded
    step (as a plain lookup would) produces a sawtooth error-vs-time
    artifact whenever the query times don't land exactly on the
    reference's own recorded steps; blending removes that.
    """
    t0, nodes0, t1, nodes1 = finest_run.nodes_bracketing_time(time, vertices_only=True)
    r0, c0 = nodes0["r"].to_numpy(), nodes0["C"].to_numpy()

    if t1 <= t0:  # time lands exactly on a step (or only one step available)
        def _interp(r: np.ndarray) -> np.ndarray:
            return np.interp(r, r0, c0, left=c0[0], right=c0[-1])
        return _interp

    r1, c1 = nodes1["r"].to_numpy(), nodes1["C"].to_numpy()
    f0 = (t1 - time) / (t1 - t0)
    f1 = (time - t0) / (t1 - t0)

    def _interp(r: np.ndarray) -> np.ndarray:
        c0r = np.interp(r, r0, c0, left=c0[0], right=c0[-1])
        c1r = np.interp(r, r1, c1, left=c1[0], right=c1[-1])
        return f0 * c0r + f1 * c1r

    return _interp
