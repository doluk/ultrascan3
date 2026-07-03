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
    """Return a callable C_ref(r) built by interpolating the finest run's
    vertex solution at the closest available step to ``time``.
    """
    nodes = finest_run.nodes_at_time(time, vertices_only=True)
    r_ref = nodes["r"].to_numpy()
    c_ref = nodes["C"].to_numpy()

    def _interp(r: np.ndarray) -> np.ndarray:
        return np.interp(r, r_ref, c_ref, left=c_ref[0], right=c_ref[-1])

    return _interp
