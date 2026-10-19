"""Gauss-Newton search for meniscus and bottom, emulated on a stored grid.

The proposal:  L(theta) = 1/2 sum e_ij^2 with theta = (meniscus, bottom),
Jacobian by forward differences (h = 1e-4 cm), the 2x2 normal equations
(J^T J) delta = -J^T e, and a backtracking line search (alpha = 1, 1/2,
...) that accepts the first trial with a lower loss.

A stored grid holds the RMSD at the grid nodes only, so the replay
evaluates the objective on a surrogate:  a tensor-product cubic spline
(parabolic ends) of RMSD^2 (proportional to L for a fixed number of data points).
J^T J is replaced by the surrogate's Hessian; the two agree when the
residuals are linear in theta near the minimum, and the step solves the
same 2x2 system.  Where that Hessian is not positive definite the
absolute eigenvalues are used, because J^T J is never indefinite.

The surrogate is smooth between the nodes.  The replay is therefore a
best case for Gauss-Newton:  it shows the convergence on the shape of
the RMSD surface (basins, flat bottom, start point) but not the effect of
finite-difference noise of real 2DSA fits at h = 1e-4 cm.

Bottom undetermined:  if the curvature along the bottom (in grid-step
units) is below BOTTOM_FLAT times that along the meniscus, or the 2x2
system is near singular (d < SINGULAR * A * D), the step changes the
meniscus only (flag bottom_flat).

Cost, in 2DSA fits (one per objective evaluation):
  fits_full   every simulation of the proposal is a 2DSA fit:  1 baseline,
              per iteration 1 perturbation per free parameter plus the
              line-search trials
  fits_proj   perturbations are forward ASTFEM simulations of the fitted
              solutes with the fitted noise held fixed (cheap); only the
              baseline and the trials are 2DSA fits
"""

import numpy as np

H_FD = 1e-4          # finite-difference step of the proposal (cm)
TOL = 1e-5           # stop when a step moves both parameters < 0.1 um
MAX_IT = 25
MAX_HALVINGS = 20
BOTTOM_FLAT = 1e-3
SINGULAR = 1e-6


def _spline_m2(x, y):
    """Second derivatives of the cubic spline through (x, y), with
    parabolic ends (M0 = M1, Mn-1 = Mn-2:  a natural spline would have no
    curvature at the grid edges);  y may have several columns."""
    n = len(x)
    y = np.asarray(y, float)
    if n < 3:
        return np.zeros_like(y)
    h = np.diff(x)
    a = np.zeros(n)
    b = np.ones(n)
    c = np.zeros(n)
    r = np.zeros_like(y)
    c[0] = -1.0
    a[n - 1] = -1.0
    for i in range(1, n - 1):
        a[i] = h[i - 1] / 6.0
        b[i] = (h[i - 1] + h[i]) / 3.0
        c[i] = h[i] / 6.0
        r[i] = (y[i + 1] - y[i]) / h[i] - (y[i] - y[i - 1]) / h[i - 1]
    # Thomas algorithm
    cp = np.zeros(n)
    rp = np.zeros_like(y)
    cp[0] = c[0] / b[0]
    rp[0] = r[0] / b[0]
    for i in range(1, n):
        den = b[i] - a[i] * cp[i - 1]
        cp[i] = c[i] / den if i < n - 1 else 0.0
        rp[i] = (r[i] - a[i] * rp[i - 1]) / den
    M = np.zeros_like(y)
    M[n - 1] = rp[n - 1]
    for i in range(n - 2, -1, -1):
        M[i] = rp[i] - cp[i] * M[i + 1]
    return M


def _spline_eval(x, y, M, t):
    """Value, first and second derivative at t (clamped to [x0, xn])."""
    n = len(x)
    if n == 1:
        z = np.zeros_like(y[0])
        return y[0], z, z
    t = min(max(t, x[0]), x[-1])
    k = min(max(np.searchsorted(x, t) - 1, 0), n - 2)
    h = x[k + 1] - x[k]
    A = (x[k + 1] - t) / h
    B = (t - x[k]) / h
    v = A * y[k] + B * y[k + 1] + \
        ((A ** 3 - A) * M[k] + (B ** 3 - B) * M[k + 1]) * h * h / 6.0
    d1 = (y[k + 1] - y[k]) / h - (3 * A * A - 1) / 6.0 * h * M[k] + \
        (3 * B * B - 1) / 6.0 * h * M[k + 1]
    d2 = A * M[k] + B * M[k + 1]
    return v, d1, d2


class Surrogate(object):
    """C2 surrogate of RMSD^2 over the meniscus x bottom grid (cm)."""

    def __init__(self, meni, bott, rmsd):
        self.m = np.asarray(meni, float)
        self.b = np.asarray(bott, float)
        self.Z = np.asarray(rmsd, float) ** 2          # nm x nb
        self.nm, self.nb = self.Z.shape
        # spline of every meniscus row along the bottom
        self.Mb = _spline_m2(self.b, self.Z.T) if self.nb > 1 else None

    def lo(self):
        return np.array([self.m[0], self.b[0]])

    def hi(self):
        return np.array([self.m[-1], self.b[-1]])

    def clamp(self, th):
        return np.minimum(np.maximum(th, self.lo()), self.hi())

    def _rows(self, bv):
        if self.nb == 1:
            z = np.zeros(self.nm)
            return self.Z[:, 0], z, z
        return _spline_eval(self.b, self.Z.T, self.Mb, bv)

    def value(self, th):
        v, _, _ = self._rows(th[1])
        if self.nm == 1:
            return float(v[0])
        return float(_spline_eval(self.m, v, _spline_m2(self.m, v), th[0])[0])

    def grad_hess(self, th):
        v, dv, d2v = self._rows(th[1])
        if self.nm == 1:
            return np.array([0.0, dv[0]]), np.array([[0.0, 0.0],
                                                      [0.0, d2v[0]]])
        _, Lm, Lmm = _spline_eval(self.m, v, _spline_m2(self.m, v), th[0])
        Lb, Lmb, _ = _spline_eval(self.m, dv, _spline_m2(self.m, dv), th[0])
        Lbb = _spline_eval(self.m, d2v, _spline_m2(self.m, d2v), th[0])[0]
        return (np.array([float(Lm), float(Lb)]),
                np.array([[float(Lmm), float(Lmb)], [float(Lmb), float(Lbb)]]))


def _psd(H):
    """J^T J is positive semidefinite:  use |eigenvalues|."""
    w, V = np.linalg.eigh(H)
    return (V * np.abs(w)) @ V.T


def gauss_newton(S, start):
    """Run the proposal on surrogate S from start (cm).  Returns a dict."""
    free = [S.nm > 1, S.nb > 1]
    nfree = sum(free)
    hm = (S.m[-1] - S.m[0]) / (S.nm - 1) if S.nm > 1 else 1.0
    hb = (S.b[-1] - S.b[0]) / (S.nb - 1) if S.nb > 1 else 1.0
    th = S.clamp(np.array(start, float))
    L = S.value(th)
    fits_full = fits_proj = 1
    status = "maxit"
    flat = False
    it = 0
    for it in range(1, MAX_IT + 1):
        g, H = S.grad_hess(th)
        H = _psd(H)
        fits_full += nfree
        A, B, D = H[0, 0], H[0, 1], H[1, 1]
        As, Ds = A * hm * hm, D * hb * hb               # grid-step units
        delta = np.zeros(2)
        if nfree == 2 and Ds > BOTTOM_FLAT * As and \
                A * D - B * B > SINGULAR * A * D:
            delta = -np.linalg.solve(H, g)
        elif free[0] and A > 0:
            delta[0] = -g[0] / A
            flat = flat or nfree == 2
        elif free[1] and D > 0:
            delta[1] = -g[1] / D
        else:
            status = "no_curvature"
            break
        if np.all(np.abs(delta) < TOL):
            status = "converged"
            break
        alpha, accepted = 1.0, False
        for _ in range(MAX_HALVINGS):
            trial = S.clamp(th + alpha * delta)
            fits_full += 1
            fits_proj += 1
            Lt = S.value(trial)
            if Lt < L:
                accepted = True
                break
            alpha *= 0.5
        if not accepted:
            status = "no_descent"
            break
        step = trial - th
        th, L = trial, Lt
        if np.all(np.abs(step) < TOL):
            status = "converged"
            break
    return {"meniscus": float(th[0]), "bottom": float(th[1]),
            "iterations": it, "fits_full": fits_full,
            "fits_proj": fits_proj, "status": status, "bottom_flat": flat,
            "rmsd": float(np.sqrt(max(L, 0.0)))}
