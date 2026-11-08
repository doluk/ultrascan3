#!/usr/bin/env python3
"""
Reduced (1D) model of a rotationally misaligned AUC sector cell.

Geometry (exact, small-angle forms in comments)
-----------------------------------------------
Sector of half-angle alpha, apex on the rotor axis O, rotated by delta about a
pivot P on the channel centre line at radius r_p (normally the cell centre).
A wall originally along polar angle phi0 = +-alpha becomes the straight line
of direction phi0+delta and distance p from O:
    p(phi0) = r_p [ sin(phi0 + delta) - sin(phi0) ]          (~ r_p delta)
At radius r the wall makes the angle psi with the radial direction:
    sin psi(r) = p / r                                       (~ delta r_p / r)
and lies at polar angle phi0 + delta - asin(p/r).  The angular width changes
only at O(alpha delta^2) (both walls rotate together), i.e. the radial
cross-section A(r) ~ 2 alpha r h is preserved; the meniscus (free surface)
stays an iso-potential surface r = r_m.

Hydrodynamics (closure)
-----------------------
At a non-radial wall, the wall-normal sedimentation flux c v sin(psi) must be
cancelled by diffusion -> a depleted layer forms on the 'upstream' wall and an
enriched layer on the 'downstream' wall.  Both layers are lateral density
anomalies in the centrifugal field, which cannot be in hydrostatic equilibrium:
the depleted layer rises (towards the meniscus) to its neutral level (inside
the boundary), the enriched layer slides to the cell bottom (Boycott effect;
Ponder 1925, Nakamura & Kuroda 1937, Acrivos & Herbolzheimer 1979).  In the
convection-dominated limit the cross-section-averaged equation is

  dc/dt = -(1/r) d/dr [ r ( s w^2 r c - D dc/dr ) ] - u(r) dc/dr       (1)
  u(r)  = kappa * Int_r^b s w^2 r' sin psi(r') dr' / ( 2 alpha r )
        ~ kappa s w^2 delta r_p (b - r) / ( 2 alpha r )

with the solute removed by the non-conservative term deposited at the bottom
(it travels there in the downstream wall layer).  Eq. (1) is written in
conservative form for solvers as
  v_eff = s w^2 r (1 + eps(r)),  eps = u / (s w^2 r)
  sink  = -k(r) c,  k = -(1/r) d(r u)/dr  ( = kappa s w^2 delta r_p/(2 alpha r) )
For a sharp boundary this reduces to the PNK result
  dr_b/dt = s w^2 r_b [ 1 + kappa delta r_p (b - r_b) / (2 alpha r_b^2) ].
kappa in [0,1] is the transport efficiency of the wall layers: 1 = fully
convective (PNK limit), 0 = no convection (ideal sector).
"""
import numpy as np
from scipy.integrate import solve_ivp
from scipy.optimize import least_squares

DEG = np.pi / 180.0


# ------------------------------------------------------------------ geometry
class MisalignGeometry:
    def __init__(self, delta_deg, alpha_deg=1.25, r_p=6.5, beta_deg=0.0):
        """delta: rotation of the cell about its centre (deg, +ccw)
           alpha: sector half-angle (deg); beta: polar offset of the channel
           centre line from the cell centre line (deg, double-sector cells)."""
        self.d = delta_deg * DEG
        self.a = alpha_deg * DEG
        self.rp = r_p
        self.b = beta_deg * DEG

    def wall_offset(self, sign):
        """Distance p of wall (+1/-1) line from the rotor axis.
        Pivot at (r_p, 0); walls at polar angle beta +- alpha."""
        phi0 = self.b + sign * self.a
        return self.rp * (np.sin(phi0 + self.d) - np.sin(phi0))

    def wall_tilt(self, r, sign=+1):
        """Angle psi between wall and radial direction at radius r (rad)."""
        return np.arcsin(self.wall_offset(sign) / r)

    def wall_phi(self, r, sign):
        phi0 = self.b + sign * self.a
        return phi0 + self.d - np.arcsin(self.wall_offset(sign) / r)

    def angular_width(self, r):
        return self.wall_phi(r, +1) - self.wall_phi(r, -1)

    def centre_phi(self, r):
        return 0.5 * (self.wall_phi(r, +1) + self.wall_phi(r, -1))

    def radial_shift(self):
        """Radial displacement of material points of an off-centre channel
        (rigid rotation about the pivot), first order: -delta r_p sin(beta)."""
        return -self.d * self.rp * np.sin(self.b)

    def beam_margin(self, r, phi_beam=None):
        """Distance (cm) from the optical beam (at polar angle phi_beam,
        default the nominal channel centre beta) to the nearest wall;
        negative -> beam clipped by a wall."""
        if phi_beam is None:
            phi_beam = self.b
        return np.minimum(self.wall_phi(r, +1) - phi_beam,
                          phi_beam - self.wall_phi(r, -1)) * r

    def bottom_smear(self, r_b):
        """Radial extent over which a flat bottom face (normal to the channel
        axis) spans after rotation: width * sin(delta)."""
        return 2 * self.a * r_b * abs(np.sin(self.d))

    def eps(self, r, r_bot, kappa):
        """Relative velocity enhancement eps(r) of the closure (exact psi)."""
        # Int_r^b r' sin psi(r') dr' = p (b - r) exactly since sin psi = p/r'
        p = abs(self.wall_offset(+1) + self.wall_offset(-1)) * 0.5
        return kappa * p * (r_bot - r) / (self.angular_width(r) * r * r)

    def sink(self, r, s_w2, kappa):
        """k(r) = -(1/r) d(r u)/dr for u = s w^2 r eps(r)."""
        p = abs(self.wall_offset(+1) + self.wall_offset(-1)) * 0.5
        return kappa * s_w2 * p / (self.angular_width(r) * r)


# ------------------------------------------------------------- 1D solver
def bernoulli(x):
    out = np.empty_like(x)
    small = np.abs(x) < 1e-6
    out[small] = 1 - x[small] / 2
    xs = x[~small]
    out[~small] = xs / np.expm1(xs)
    return out


def simulate(species, times, r_m=6.0, r_b=7.2, rpm=50000., geom=None,
             kappa=0.0, N=1200):
    """species: list of (s [s], D [cm^2/s], c0). Returns r (cell centres),
    C[nt, N] total concentration and per-species list."""
    w2 = (rpm * np.pi / 30.) ** 2
    rf = np.linspace(r_m, r_b, N + 1)
    rc = 0.5 * (rf[1:] + rf[:-1])
    h = rf[1] - rf[0]
    vol = rc * h
    out = []
    for s, D, c0 in species:
        if geom is not None and kappa > 0:
            epsf = geom.eps(rf[1:-1], r_b, kappa)
            k = geom.sink(rc, s * w2, kappa)
        else:
            epsf = np.zeros(N - 1)
            k = np.zeros(N)
        v = s * w2 * rf[1:-1] * (1 + epsf)
        Pe = v * h / D
        Bm, Bp = bernoulli(-Pe), bernoulli(Pe)
        rD_h = rf[1:-1] * D / h

        def rhs(t, c):
            F = rD_h * (Bm * c[:-1] - Bp * c[1:])      # outward face flux * r
            dc = np.zeros_like(c)
            dc[:-1] -= F
            dc[1:] += F
            dc /= vol
            loss = k * c
            dc -= loss
            dc[-1] += np.sum(loss * vol) / vol[-1]       # deposit at bottom
            return dc

        # Jacobian is tridiagonal + last row; let BDF estimate it (banded+1)
        sol = solve_ivp(rhs, (0, times[-1]), np.full(N, c0), t_eval=times,
                        method="LSODA", rtol=1e-7, atol=1e-10)
        out.append(sol.y.T)
    return rc, sum(out), out


# ----------------------------------------------------------------- fitting
def fit_species(r, data, times, n_species, r_m, r_b, rpm, guess, rmax=None,
                N=600):
    """Least-squares fit of ideal (aligned) model, parameters per species
    (s, D, c0).  Returns parameter array."""
    mask = r < (rmax if rmax else r_b - 0.05)

    def model(p):
        sp = [(p[3 * i] * 1e-13, p[3 * i + 1] * 1e-7, p[3 * i + 2])
              for i in range(n_species)]
        rr, C, _ = simulate(sp, times, r_m, r_b, rpm, N=N)
        return np.array([np.interp(r[mask], rr, Ci) for Ci in C])

    def res(p):
        return (model(p) - data[:, mask]).ravel()

    x0 = np.ravel([[g[0] * 1e13, g[1] * 1e7, g[2]] for g in guess])
    sol = least_squares(res, x0, x_scale="jac", diff_step=1e-4)
    rmsd = np.sqrt(np.mean(sol.fun ** 2))
    return sol.x.reshape(n_species, 3), rmsd


if __name__ == "__main__":
    g = MisalignGeometry(1.0)
    for r in (6.0, 6.5, 7.2):
        print(f"r={r}: psi={g.wall_tilt(r)/DEG:.4f} deg  width={g.angular_width(r)/DEG:.5f} deg"
              f"  eps(kappa=1)={g.eps(r, 7.2, 1.0):.4f}  beam margin={g.beam_margin(r)*1e4:.0f} um")
    print("bottom smear (um):", g.bottom_smear(7.2) * 1e4)
    print("radial shift beta=1.5deg (um):", MisalignGeometry(1.0, beta_deg=1.5).radial_shift() * 1e4)
