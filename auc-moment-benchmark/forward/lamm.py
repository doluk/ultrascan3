"""
Reference Lamm-equation solver (Layer 0).

Independent of UltraScan/Qt so that the whole handoff package is runnable by
someone with only numpy/scipy.  Layer 1 (generate/generate.cpp) produces the
same files from UltraScan's ASTFEM/ASTFVM solvers; the two are meant to be
cross-validated against each other (see FINDINGS.md, Phase A).

Equation solved, sector-shaped cell, single non-interacting species:

    dc/dt = (1/r) d/dr [ r D dc/dr - s w^2 r^2 c ]

written in conservative flux form  dc/dt = -(1/r) d(r F)/dr  with

    F(r) = -D dc/dr + s w^2 r c .

Face fluxes use Scharfetter-Gummel exponential fitting rather than upwinding.
This matters: plain upwind injects numerical diffusion of order v*dr/2, which
is indistinguishable from real diffusion and would corrupt every second and
higher moment.  SG is exact for the local advection-diffusion steady state and
so carries no artificial width.

Time integration is Crank-Nicolson (2nd order, unconditionally stable) with a
tridiagonal solve per step.
"""

import numpy as np
from scipy.linalg import solve_banded

OMEGA_PER_RPM = 2.0 * np.pi / 60.0


def omega(rpm):
    """Angular velocity [rad/s] from rotor speed [rpm]."""
    return rpm * OMEGA_PER_RPM


def _bernoulli(x):
    """B(x) = x / (exp(x) - 1), stable at x -> 0 and for large |x|."""
    x = np.asarray(x, dtype=float)
    out = np.empty_like(x)
    small = np.abs(x) < 1e-8
    out[small] = 1.0 - x[small] / 2.0
    big = ~small
    xb = x[big]
    # exp overflow guard: B(x) -> 0 for large +x, -> -x for large -x
    with np.errstate(over="ignore"):
        out[big] = np.where(
            xb > 700.0, 0.0,
            np.where(xb < -700.0, -xb, xb / np.expm1(xb)),
        )
    return out


class LammGrid:
    """Uniform finite-volume grid in r over [meniscus, bottom]."""

    def __init__(self, meniscus, bottom, n_cells):
        self.rm = float(meniscus)
        self.rb = float(bottom)
        self.n = int(n_cells)
        self.dr = (self.rb - self.rm) / self.n
        self.faces = np.linspace(self.rm, self.rb, self.n + 1)
        self.r = 0.5 * (self.faces[:-1] + self.faces[1:])   # cell centres
        self.vol = self.r * self.dr                          # sector volume / (angle*h)


def _cn_matrices(grid, s, D, w2):
    """
    Build the spatial operator L with  dc/dt = L c  for the SG-discretised
    Lamm equation, returned in banded (tridiagonal) form.

    Zero flux is imposed at both end faces, so the scheme conserves
    sum(vol * c) exactly up to the linear solve.
    """
    n = grid.n
    rf = grid.faces[1:-1]            # interior faces
    h = grid.dr
    v = s * w2 * rf                  # advective velocity at interior faces
    P = v * h / D                    # face Peclet number
    a_left = _bernoulli(-P)          # weight on c_i
    a_right = _bernoulli(P)          # weight on c_{i+1}
    # F_{i+1/2} = (D/h) [ B(-P) c_i - B(P) c_{i+1} ]
    coef = D / h
    # r*F at interior faces
    A_i = rf * coef * a_left         # multiplies c_i
    A_ip = -rf * coef * a_right      # multiplies c_{i+1}

    lower = np.zeros(n)
    diag = np.zeros(n)
    upper = np.zeros(n)

    # dc_i/dt = -( (rF)_{i+1/2} - (rF)_{i-1/2} ) / vol_i
    # contribution of face k (between cell k and k+1) to cells k and k+1
    diag[:-1] += -A_i / grid.vol[:-1]
    upper[:-1] += -A_ip / grid.vol[:-1]      # upper[k] multiplies c_{k+1} in row k
    lower[1:] += A_i / grid.vol[1:]          # lower[k] multiplies c_{k-1} in row k
    diag[1:] += A_ip / grid.vol[1:]
    return lower, diag, upper


def _banded(lower, diag, upper):
    """Pack tridiagonal (lower, diag, upper) into scipy banded layout."""
    n = diag.size
    ab = np.zeros((3, n))
    ab[0, 1:] = upper[:-1]
    ab[1, :] = diag
    ab[2, :-1] = lower[1:]
    return ab


def solve_species(grid, s, D, rpm, times, c0=1.0, cfl=0.03):
    """
    Solve the Lamm equation for one species and sample it at `times`.

    Parameters
    ----------
    grid   : LammGrid
    s      : sedimentation coefficient [s]   (e.g. 4e-13 for 4 S)
    D      : diffusion coefficient [cm^2/s]
    rpm    : rotor speed
    times  : 1-D array of sample times [s], ascending, all >= 0
    cfl    : accuracy knob for the adaptive time step (smaller = finer)
    c0     : uniform loading concentration, or an initial profile
             on the grid cells (used by the reacting-system splitter)

    Returns
    -------
    (n_times, n_cells) array of concentrations at cell centres.
    """
    times = np.asarray(times, dtype=float)
    if np.any(np.diff(times) < 0):
        raise ValueError("times must be ascending")
    w2 = omega(rpm) ** 2
    lower, diag, upper = _cn_matrices(grid, s, D, w2)

    c = np.asarray(c0, dtype=float)
    c = np.full(grid.n, float(c0)) if c.ndim == 0 else c.copy()
    out = np.zeros((times.size, grid.n))

    # Sub-step between requested times so that accuracy is set by dt_max,
    # not by the (possibly coarse) scan schedule.
    t_now = 0.0
    cache = {}
    for k, t_target in enumerate(times):
        if t_target < t_now - 1e-12:
            raise ValueError("times must be ascending")
        span = t_target - t_now
        if span > 0:
            dt_max = _accuracy_dt(grid, s, D, w2, t_now, cfl)
            n_sub = max(1, int(np.ceil(span / dt_max)))
            dt = span / n_sub
            key = round(dt, 12)
            if key not in cache:
                ab = _banded(-0.5 * dt * lower, 1.0 - 0.5 * dt * diag, -0.5 * dt * upper)
                cache[key] = ab
            ab = cache[key]
            for _ in range(n_sub):
                rhs = c + 0.5 * dt * _tri_mv(lower, diag, upper, c)
                c = solve_banded((1, 1), ab, rhs)
            t_now = t_target
        out[k] = c
    return out


def _tri_mv(lower, diag, upper, x):
    """Tridiagonal matrix-vector product."""
    y = diag * x
    y[:-1] += upper[:-1] * x[1:]
    y[1:] += lower[1:] * x[:-1]
    return y


def _accuracy_dt(grid, s, D, w2, t_now, cfl=0.03):
    """
    Adaptive time step.

    Crank-Nicolson is unconditionally stable, so the step is set by accuracy,
    not stability.  The scale that must be resolved is the *boundary width*
    sqrt(4 D t), not the cell size: a step is acceptable if the boundary moves
    a small fraction of its own width, and if diffusion spreads it by a small
    fraction of its current width.  The first gives dt <= cfl*width/v, the
    second dt <= 2 cfl^2 t.  Early on, width collapses to the cell size and
    the step is floored.

    Taking dt from the cell size instead (the naive CFL) costs two orders of
    magnitude in runtime for no accuracy gain; this is verified by the
    time-step convergence check in analysis/phaseA.py.
    """
    v_max = abs(s) * w2 * grid.rb
    width = max(np.sqrt(4.0 * D * max(t_now, 0.0)), grid.dr)
    dt_adv = cfl * width / v_max if v_max > 0 else np.inf
    dt_dif = 2.0 * cfl * cfl * t_now if t_now > 0 else np.inf
    return max(min(dt_adv, dt_dif), 0.5)


def simulate_model(grid, components, rpm, times, cfl=0.03):
    """
    Sum of non-interacting species.

    components : iterable of dicts with keys 's', 'D', 'c' (loading conc).
    Returns (n_times, n_cells) total concentration.
    """
    total = np.zeros((len(times), grid.n))
    for comp in components:
        total += solve_species(grid, comp["s"], comp["D"], rpm, times,
                               c0=comp["c"], cfl=cfl)
    return total


# --------------------------------------------------------------------------
# Faxen / Fujita approximate analytical solution, for Phase A.1 validation
# --------------------------------------------------------------------------

def faxen(grid_r, s, D, rpm, t, c0=1.0, rm=None):
    """
    Faxen's approximate solution for the sedimenting boundary, valid once the
    boundary is clear of the meniscus and well before it reaches the bottom.

        c(r,t) = (c0/2) exp(-2 s w^2 t) erfc( (rm e^{s w^2 t} - r) / sqrt(4 D t eps) )

    with the standard Faxen correction eps = (1 - ...) folded in through the
    moving-boundary width.  We use the common form in which the boundary
    position is rm*exp(s w^2 t), the plateau carries the square-dilution factor
    exp(-2 s w^2 t), and the boundary half-width is sqrt(2 D t) scaled by the
    radial stretching of the sedimentation field.
    """
    from scipy.special import erfc

    rm = grid_r[0] if rm is None else rm
    w2 = omega(rpm) ** 2
    rbnd = rm * np.exp(s * w2 * t)
    plateau = c0 * np.exp(-2.0 * s * w2 * t)
    # width of the boundary in r: diffusive spread, stretched by the divergence
    # of the sedimentation field over the elapsed time
    width = np.sqrt(4.0 * D * t)
    tau = 2.0 * s * w2 * t
    # Faxen's stretch correction for the sector geometry
    stretch = np.sqrt((np.expm1(tau) / tau) if tau > 1e-12 else 1.0)
    # depletion is BELOW the boundary, plateau above it
    return 0.5 * plateau * erfc((rbnd - grid_r) / (width * stretch))
