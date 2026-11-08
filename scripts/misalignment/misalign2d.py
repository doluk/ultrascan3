#!/usr/bin/env python3
"""
2D reference model: sedimentation-diffusion + buoyancy-driven Stokes flow in a
channel whose walls are tilted by an angle psi relative to the centrifugal
field (local model of a rotationally misaligned AUC sector).

Frame: x along the channel axis (outward, x=0 at meniscus), y across the
channel (0..W).  Centrifugal field: planar potential field tilted by psi,
  Phi = w^2 rho^2 / 2,  rho = r_m + x cos psi + (y - W/2) sin psi,
  g = grad Phi = w^2 rho (cos psi, sin psi)   (curl-free: a uniform solute
  distribution produces no flow).  CGS units, c in mg/mL.

Equations
  dc/dt + div( c (u + s G g) - D grad c ) = 0          (no flux on all walls)
  mu lap^2 Psi = d(f_y)/dx - d(f_x)/dy,  f = beta c G g  (Stokes, Boussinesq)
  u = dPsi/dy, v = -dPsi/dx,  Psi = dPsi/dn = 0 on walls (no slip)
  beta = (1 - vbar rho0) * 1e-3   [g/mL per mg/mL]

Rationale for 2D planar Stokes: flow is in the (r, phi) plane, perpendicular
to Omega.  For a planar incompressible field the Coriolis force 2 Omega x u is
a pure gradient and is absorbed into the pressure (Re << 1, Taylor-Proudman
makes the flow z-invariant away from window Ekman layers of thickness
sqrt(nu/Omega) ~ 14 um).  Radial dilution (sector divergence) is omitted; it
is identical for aligned and misaligned cells to O(alpha).

STATUS: exploratory.  With realistic parameters (1 mg/mL protein, 50 krpm)
buoyancy-driven velocities reach ~0.1 cm/s within seconds (~10^3 x the
sedimentation velocity), i.e. the coupled system is extremely stiff: the
Stokes relaxation rate of a density perturbation, beta G (dc/dr) L^2 / mu, is
>> 1/s.  The explicit time stepping used here therefore needs dt ~ 1e-2 s
or less, and becomes unstable on coarse grids.  The script is kept to
document this time-scale separation, which justifies the convection-
dominated (quasi-static) closure in misalign1d.py.  A quantitative 2D/3D
reference needs an implicit (IMEX) Stokes-transport coupling.
"""
import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla
import time, argparse, json


def d2_dirichlet(n, h):
    """Second difference on n interior nodes, homogeneous Dirichlet."""
    e = np.ones(n)
    return sp.diags([e[:-1], -2 * e, e[:-1]], [-1, 0, 1]) / h**2


def d4_clamped(n, h):
    """Fourth difference on n interior nodes, Psi=dPsi/dn=0 (ghost reflection)."""
    e = np.ones(n)
    main = 6 * e
    main[0] = main[-1] = 7.0
    return sp.diags([e[:-2], -4 * e[:-1], main, -4 * e[:-1], e[:-2]],
                    [-2, -1, 0, 1, 2]) / h**4


class Sim2D:
    def __init__(self, psi_deg=1.0, rpm=50000., r_m=6.2, L=0.9, W=0.28,
                 s=4.4e-13, D=6.0e-7, c0=1.0, vbar=0.73, rho0=1.0, mu=0.01,
                 Nx=360, Ny=112, cfl=0.4, flow=True):
        self.psi = np.deg2rad(psi_deg)
        self.w2 = (rpm * np.pi / 30.) ** 2
        self.r_m, self.L, self.W = r_m, L, W
        self.s, self.D, self.mu = s, D, mu
        self.beta = (1. - vbar * rho0) * 1e-3
        self.Nx, self.Ny = Nx, Ny
        self.hx, self.hy = L / Nx, W / Ny
        self.cfl = cfl
        self.flow = flow and psi_deg != 0.0
        self.xc = (np.arange(Nx) + 0.5) * self.hx          # cell centers
        self.xf = np.arange(Nx + 1) * self.hx              # x faces / nodes
        self.yc = (np.arange(Ny) + 0.5) * self.hy
        self.yf = np.arange(Ny + 1) * self.hy
        cp, sp_ = np.cos(self.psi), np.sin(self.psi)
        G = lambda x, y: self.w2 * (r_m + x[:, None] * cp + (y[None, :] - W / 2) * sp_)
        self.Gc = G(self.xc, self.yc)                       # (Nx, Ny)
        self.Gfx = G(self.xf, self.yc)                      # (Nx+1, Ny)
        self.Gfy = G(self.xc, self.yf)                      # (Nx, Ny+1)
        self.c = np.full((Nx, Ny), c0, dtype=float)
        self.Psi = np.zeros((Nx + 1, Ny + 1))
        self.t = 0.0
        if self.flow:
            nxi, nyi = Nx - 1, Ny - 1
            Ix, Iy = sp.identity(nxi), sp.identity(nyi)
            Dxx, Dyy = d2_dirichlet(nxi, self.hx), d2_dirichlet(nyi, self.hy)
            B = (sp.kron(d4_clamped(nxi, self.hx), Iy)
                 + 2 * sp.kron(Dxx, Dyy)
                 + sp.kron(Ix, d4_clamped(nyi, self.hy)))
            t0 = time.time()
            self.lu = spla.splu((mu * B).tocsc())
            self.t_fact = time.time() - t0

    # ---------------------------------------------------------------- flow
    def solve_flow(self):
        if not self.flow:
            return
        c, hx, hy = self.c, self.hx, self.hy
        cg = c * self.Gc
        # gradients of c*G at interior nodes from the 4 adjacent cells
        dcgdx = ((cg[1:, 1:] + cg[1:, :-1]) - (cg[:-1, 1:] + cg[:-1, :-1])) / (2 * hx)
        dcgdy = ((cg[1:, 1:] + cg[:-1, 1:]) - (cg[1:, :-1] + cg[:-1, :-1])) / (2 * hy)
        rhs = self.beta * (np.sin(self.psi) * dcgdx - np.cos(self.psi) * dcgdy)
        self.Psi[1:-1, 1:-1] = self.lu.solve(rhs.ravel()).reshape(rhs.shape)

    def velocities(self):
        """Total transport velocity on faces (fluid + sedimentation)."""
        P = self.Psi
        u = (P[:, 1:] - P[:, :-1]) / self.hy               # (Nx+1, Ny) x-faces
        v = -(P[1:, :] - P[:-1, :]) / self.hx              # (Nx, Ny+1) y-faces
        ux = u + self.s * self.Gfx * np.cos(self.psi)
        vy = v + self.s * self.Gfy * np.sin(self.psi)
        ux[0, :] = ux[-1, :] = 0.0                         # no-flux walls
        vy[:, 0] = vy[:, -1] = 0.0
        return ux, vy, u, v

    # ----------------------------------------------------------- transport
    @staticmethod
    def _minmod(a, b):
        return np.where(a * b > 0, np.sign(a) * np.minimum(abs(a), abs(b)), 0.0)

    def _face_flux_x(self, c, ux):
        dc = np.diff(c, axis=0)                            # (Nx-1, Ny)
        slope = np.zeros_like(c)
        slope[1:-1] = self._minmod(dc[:-1], dc[1:])
        cL = c[:-1] + 0.5 * slope[:-1]                     # left state at face
        cR = c[1:] - 0.5 * slope[1:]
        a = ux[1:-1]
        F = np.zeros((self.Nx + 1, self.Ny))
        F[1:-1] = np.where(a > 0, a * cL, a * cR) - self.D * dc / self.hx
        return F

    def _face_flux_y(self, c, vy):
        dc = np.diff(c, axis=1)
        slope = np.zeros_like(c)
        slope[:, 1:-1] = self._minmod(dc[:, :-1], dc[:, 1:])
        cL = c[:, :-1] + 0.5 * slope[:, :-1]
        cR = c[:, 1:] - 0.5 * slope[:, 1:]
        a = vy[:, 1:-1]
        F = np.zeros((self.Nx, self.Ny + 1))
        F[:, 1:-1] = np.where(a > 0, a * cL, a * cR) - self.D * dc / self.hy
        return F

    def rate(self, c, ux, vy):
        Fx = self._face_flux_x(c, ux)
        Fy = self._face_flux_y(c, vy)
        return -(np.diff(Fx, axis=0) / self.hx + np.diff(Fy, axis=1) / self.hy)

    def step(self, dt_max, ux, vy):
        umax = max(np.abs(ux).max() / self.hx, np.abs(vy).max() / self.hy, 1e-30)
        dt = min(dt_max, self.cfl / umax,
                 0.2 / (self.D * (1 / self.hx**2 + 1 / self.hy**2)))
        c1 = self.c + dt * self.rate(self.c, ux, vy)        # SSP-RK2
        self.c = 0.5 * (self.c + c1 + dt * self.rate(c1, ux, vy))
        self.t += dt
        return dt

    def run(self, t_out, flow_every=1, verbose=True):
        out = []
        k, n, t0 = 0, 0, time.time()
        umax_hist = []
        for tt in t_out:
            while self.t < tt - 1e-9:
                if n % flow_every == 0:
                    self.solve_flow()
                    ux, vy, u, v = self.velocities()
                    umax_hist.append(max(np.abs(u).max(), np.abs(v).max()))
                self.step(tt - self.t, ux, vy)
                n += 1
            out.append(self.c.copy())
            if verbose:
                print(f"  t={self.t:8.1f}s steps={n:7d} |u|max={umax_hist[-1] if umax_hist else 0:.3e} cm/s"
                      f" mass={self.c.mean():.6f} wall={time.time()-t0:6.1f}s", flush=True)
        return np.array(out), np.array(umax_hist)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--psi", type=float, default=1.0)
    ap.add_argument("--c0", type=float, default=1.0)
    ap.add_argument("--s", type=float, default=4.4e-13)
    ap.add_argument("--D", type=float, default=6.0e-7)
    ap.add_argument("--rpm", type=float, default=50000.)
    ap.add_argument("--Nx", type=int, default=360)
    ap.add_argument("--Ny", type=int, default=112)
    ap.add_argument("--tend", type=float, default=4000.)
    ap.add_argument("--nscan", type=int, default=20)
    ap.add_argument("--flow_every", type=int, default=1)
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    sim = Sim2D(psi_deg=a.psi, c0=a.c0, s=a.s, D=a.D, rpm=a.rpm, Nx=a.Nx, Ny=a.Ny)
    if sim.flow:
        print(f"LU factorization {sim.t_fact:.2f}s")
    t_out = np.linspace(a.tend / a.nscan, a.tend, a.nscan)
    C, uh = sim.run(t_out, flow_every=a.flow_every)
    if a.out:
        np.savez_compressed(a.out, C=C, t=t_out, x=sim.xc, r_m=sim.r_m, W=sim.W,
                            args=json.dumps(vars(a)), umax=uh)
