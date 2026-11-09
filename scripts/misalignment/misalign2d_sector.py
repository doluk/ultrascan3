#!/usr/bin/env python3
"""
2D Stokes-Boussinesq-Lamm solver in true sector geometry.

Frame: the cell.  The channel is the annular sector
    rho in [r_m, r_m + L],  theta in [-alpha/2, alpha/2]
about the sector vertex O (polar coordinates rho, theta), so its walls are
exactly radial lines of O.  A rotation of the cell by delta about the pivot
P = (r_p, 0) is equivalent to moving the rotor axis to
    O' = P + R(-delta) (O - P) = ( r_p (1 - cos delta), r_p sin delta ),
and the centrifugal field is g = omega^2 (X - O') (curl-free).  For delta = 0
the field is radial in the sector and there is no flow.  Compared with the
planar model (misalign2d_bdf2.py) this adds the sector divergence of the
channel, the curvature of the field lines across the channel and the radial
variation of the wall tilt psi(rho) = asin(r_p sin(delta) / rho).

Equations (u = (u_rho, u_theta), Psi streamfunction, omega vorticity):
  dc_k/dt + div( c_k (u + s_k g) - D_k grad c_k ) = 0      no flux on walls
  u_rho = (1/rho) dPsi/dtheta,  u_theta = -dPsi/drho,  omega = -lap Psi
  -mu lap omega = beta [ g_theta dc/drho - g_rho (1/rho) dc/dtheta ],
  c = sum_k c_k (common partial specific volume);  no slip (Wood) on walls.

Discretisation as in misalign2d_bdf2.py / misalign2d_multi.py (finite
volumes with polar metric, wall-clustered theta grid, BDF2 with adaptive
step, Newton-Picard coupling); the discrete flow is exactly divergence-free.

Usage:
  misalign2d_sector.py --delta 1 --c0 0.1 --out runs/s_p1_c0.1.npz
  misalign2d_sector.py --delta 1 --species "4.4e-13,6.0e-7,0.574;6.6e-13,4.5e-7,0.126" ...
"""
import numpy as np
import scipy.sparse as sp
import time, argparse, json
from misalign2d_bdf2 import stretched, lap1d_nodes
from misalign2d_multi import MultiSolver


class SectorSolver(MultiSolver):
    def __init__(self, species, delta_deg=1.0, pivot=6.5, alpha_deg=2.5,
                 rpm=50000., r_m=6.2, L=0.9, vbar=0.73, rho0=1.0, mu=0.01,
                 Nx=120, Ny=40, ybeta=3.0, flow=True, order=2):
        self.species = [tuple(map(float, s_)) for s_ in species]
        self.nsp = len(self.species)
        self.delta = np.deg2rad(delta_deg)
        self.alpha = np.deg2rad(alpha_deg)
        self.pivot = pivot
        self.w2 = (rpm * np.pi / 30.) ** 2
        self.r_m, self.L = r_m, L
        self.W = self.alpha * (r_m + 0.5 * L)          # mean width (for reporting)
        self.mu = mu
        self.beta = (1. - vbar * rho0) * 1e-3
        self.Nx, self.Ny, self.order = Nx, Ny, order
        self.flow = flow and delta_deg != 0.0
        self.ox = pivot * (1 - np.cos(self.delta))       # rotor axis in the cell frame
        self.oy = pivot * np.sin(self.delta)
        # coordinates: x = rho - r_m, y = theta + alpha/2
        self.xn = np.linspace(0, L, Nx + 1)
        self.yn = stretched(Ny, self.alpha, ybeta)
        self.hx = L / Nx
        self.hy = np.diff(self.yn)
        self.xc = 0.5 * (self.xn[1:] + self.xn[:-1])
        self.yc = 0.5 * (self.yn[1:] + self.yn[:-1])
        self.x, self.y = self.xc, self.yc
        self.dyc = np.diff(self.yc)
        self.rn, self.rc = r_m + self.xn, r_m + self.xc
        self.c0 = sum(c for _, _, c in self.species)
        self.nc = Nx * Ny
        self.npsi = (Nx - 1) * (Ny - 1)
        self.nom = (Nx + 1) * (Ny + 1)
        self.U = np.zeros((Nx + 1, Ny))
        self.V = np.zeros((Nx, Ny + 1))
        self.t = 0.0
        # sedimentation velocities per unit s on faces
        grx, _ = self.field(self.rn[:, None], self.yc[None, :])        # rho faces
        _, gty = self.field(self.rc[:, None], self.yn[None, :])        # theta faces
        grx[0, :] = grx[-1, :] = 0
        gty[:, 0] = gty[:, -1] = 0
        self._build_static()
        self.vsx_k = [s * grx for s, _, _ in self.species]
        self.vsy_k = [s * gty for s, _, _ in self.species]
        self.Ld_k = [self.Ldiff1 * D for _, D, _ in self.species]
        self.C = np.array([np.full((Nx, Ny), c) for _, _, c in self.species])
        self.C_prev = None
        self.dt_prev = None

    # ------------------------------------------------------------- field
    def field(self, rho, y):
        th = y - 0.5 * self.alpha
        X, Y = rho * np.cos(th) - self.ox, rho * np.sin(th) - self.oy
        gr = self.w2 * (X * np.cos(th) + Y * np.sin(th))
        gt = self.w2 * (-X * np.sin(th) + Y * np.cos(th))
        return gr * np.ones_like(th), gt * np.ones_like(rho)

    def radius(self, rho, y):
        """Distance from the rotor axis (what the optics measure)."""
        th = y - 0.5 * self.alpha
        return np.hypot(rho * np.cos(th) - self.ox, rho * np.sin(th) - self.oy)

    # ------------------------------------------------------------ operators
    def _build_static(self):
        Nx, Ny, hx, yn, hy = self.Nx, self.Ny, self.hx, self.yn, self.hy
        rn, rc = self.rn, self.rc
        I, J = np.meshgrid(np.arange(1, Nx), np.arange(1, Ny), indexing="ij")
        I, J = I.ravel(), J.ravel()
        jj = J - 1
        p = self.pid(I, J)
        ri = rn[I]
        ylo, ydi, yup = lap1d_nodes(yn)
        # polar Laplacian at interior nodes
        cxm = (ri - hx / 2) / (ri * hx * hx)
        cxp = (ri + hx / 2) / (ri * hx * hx)
        cyl, cyd, cyu = ylo[jj] / ri ** 2, ydi[jj] / ri ** 2, yup[jj] / ri ** 2
        rows, cols, vals = [], [], []
        for di, w in [(-1, cxm), (1, cxp)]:
            m = (I + di >= 1) & (I + di <= Nx - 1)
            rows.append(p[m]); cols.append(self.pid(I[m] + di, J[m])); vals.append(w[m])
        for dj, w in [(-1, cyl), (1, cyu)]:
            m = (J + dj >= 1) & (J + dj <= Ny - 1)
            rows.append(p[m]); cols.append(self.pid(I[m], J[m] + dj)); vals.append(w[m])
        rows.append(p); cols.append(p); vals.append(-(cxm + cxp) + cyd)
        self.LapP = self._coo(rows, cols, vals, (self.npsi, self.npsi))
        rows, cols, vals = [], [], []
        for di, w in [(-1, cxm), (1, cxp)]:
            rows.append(p); cols.append(self.oid(I + di, J)); vals.append(w)
        for dj, w in [(-1, cyl), (1, cyu)]:
            rows.append(p); cols.append(self.oid(I, J + dj)); vals.append(w)
        rows.append(p); cols.append(self.oid(I, J)); vals.append(-(cxm + cxp) + cyd)
        self.LapO = self._coo(rows, cols, vals, (self.npsi, self.nom))
        self.Eint = sp.csr_matrix((np.ones(p.size), (p, self.oid(I, J))), shape=(self.npsi, self.nom))
        # wall vorticity (Wood), normal distance h
        brow_o, brow_p, wo, wp = [], [], [], []
        k = 0
        def add_wall(ib, jb, ii, ji, h):
            nonlocal k
            brow_o.extend([k, k]); wo.extend([(self.oid(ib, jb), 1.0), (self.oid(ii, ji), 0.5)])
            if 1 <= ii <= Nx - 1 and 1 <= ji <= Ny - 1:
                brow_p.append(k); wp.append((self.pid(ii, ji), 3.0 / h ** 2))
            k += 1
        for j in range(1, Ny):
            add_wall(0, j, 1, j, hx)
            add_wall(Nx, j, Nx - 1, j, hx)
        for i in range(1, Nx):
            add_wall(i, 0, i, 1, rn[i] * (yn[1] - yn[0]))
            add_wall(i, Ny, i, Ny - 1, rn[i] * (yn[Ny] - yn[Ny - 1]))
        for (ib, jb) in [(0, 0), (0, Ny), (Nx, 0), (Nx, Ny)]:
            brow_o.append(k); wo.append((self.oid(ib, jb), 1.0)); k += 1
        self.nb = k
        self.Wo = sp.csr_matrix(([w for _, w in wo], (brow_o, [c for c, _ in wo])), shape=(k, self.nom))
        self.Wp = sp.csr_matrix(([w for _, w in wp], (brow_p, [c for c, _ in wp])), shape=(k, self.npsi))
        assert self.npsi + self.nb == self.nom
        # buoyancy: beta [ g_theta dc/drho - g_rho (1/rho) dc/dtheta ] at nodes
        gr, gt = self.field(ri, yn[J])
        dyc = self.dyc[J - 1]
        rows, cols, vals = [], [], []
        for di, dj, fx, fy in [(0, 0, 1, 1), (0, -1, 1, -1), (-1, 0, -1, 1), (-1, -1, -1, -1)]:
            rows.append(p); cols.append(self.cid(I + di, J + dj))
            vals.append(self.beta * (gt * fx / (2 * hx) - gr * fy / (2 * ri * dyc)))
        self.R = self._coo(rows, cols, vals, (self.npsi, self.nc))
        # Psi -> u_rho on interior rho faces, u_theta on interior theta faces
        rows, cols, vals = [], [], []
        I2, J2 = np.meshgrid(np.arange(1, Nx), np.arange(Ny), indexing="ij")
        I2, J2 = I2.ravel(), J2.ravel()
        f = I2 * Ny + J2
        m = J2 + 1 <= Ny - 1
        rows.append(f[m]); cols.append(self.pid(I2[m], J2[m] + 1)); vals.append(1 / (rn[I2[m]] * hy[J2[m]]))
        m = J2 >= 1
        rows.append(f[m]); cols.append(self.pid(I2[m], J2[m])); vals.append(-1 / (rn[I2[m]] * hy[J2[m]]))
        self.Gx = self._coo(rows, cols, vals, ((Nx + 1) * Ny, self.npsi))
        rows, cols, vals = [], [], []
        I3, J3 = np.meshgrid(np.arange(Nx), np.arange(1, Ny), indexing="ij")
        I3, J3 = I3.ravel(), J3.ravel()
        f = I3 * (Ny + 1) + J3
        m = I3 + 1 <= Nx - 1
        rows.append(f[m]); cols.append(self.pid(I3[m] + 1, J3[m])); vals.append(np.full(m.sum(), -1 / hx))
        m = I3 >= 1
        rows.append(f[m]); cols.append(self.pid(I3[m], J3[m])); vals.append(np.full(m.sum(), 1 / hx))
        self.Gy = self._coo(rows, cols, vals, (Nx * (Ny + 1), self.npsi))
        # divergence (cells <- face fluxes per unit length), polar metric
        Ic, Jc = np.meshgrid(np.arange(Nx), np.arange(Ny), indexing="ij")
        c = self.cid(Ic, Jc).ravel()
        fR = ((Ic + 1) * Ny + Jc).ravel(); fL = (Ic * Ny + Jc).ravel()
        rcc = rc[Ic].ravel()
        self.Dx = sp.csr_matrix((np.r_[rn[Ic + 1].ravel() / (rcc * hx), -rn[Ic].ravel() / (rcc * hx)],
                                 (np.r_[c, c], np.r_[fR, fL])), shape=(self.nc, (Nx + 1) * Ny))
        ih = (1 / (rc[Ic] * hy[Jc])).ravel()
        fT = (Ic * (Ny + 1) + Jc + 1).ravel(); fB = (Ic * (Ny + 1) + Jc).ravel()
        self.Dy = sp.csr_matrix((np.r_[ih, -ih], (np.r_[c, c], np.r_[fT, fB])),
                                shape=(self.nc, Nx * (Ny + 1)))
        # diffusion operator per unit D:  div grad c, no flux at walls
        rows, cols, vals = [], [], []
        Ix, Jx = np.meshgrid(np.arange(1, Nx), np.arange(Ny), indexing="ij")
        Lc, Rc = self.cid(Ix - 1, Jx).ravel(), self.cid(Ix, Jx).ravel()
        g = (rn[Ix] / hx).ravel()
        aL, aR = (rc[Ix - 1] * hx).ravel(), (rc[Ix] * hx).ravel()
        for r_, c_, v_ in [(Lc, Lc, -g / aL), (Lc, Rc, g / aL), (Rc, Rc, -g / aR), (Rc, Lc, g / aR)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        Iy, Jy = np.meshgrid(np.arange(Nx), np.arange(1, Ny), indexing="ij")
        Bc, Tc = self.cid(Iy, Jy - 1).ravel(), self.cid(Iy, Jy).ravel()
        r2 = (rc[Iy] ** 2).ravel()
        g = (1 / self.dyc[Jy - 1]).ravel()
        hB, hT = hy[Jy - 1].ravel(), hy[Jy].ravel()
        for r_, c_, v_ in [(Bc, Bc, -g / (hB * r2)), (Bc, Tc, g / (hB * r2)),
                           (Tc, Tc, -g / (hT * r2)), (Tc, Bc, g / (hT * r2))]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        self.Ldiff1 = self._coo(rows, cols, vals, (self.nc, self.nc))
        self.Kflow = sp.bmat([[self.LapP, self.Eint],
                              [None, -self.mu * self.LapO],
                              [self.Wp, self.Wo]], format="csr")

    def _upwind(self, ax, ay):
        Nx, Ny, hx, rn, rc = self.Nx, self.Ny, self.hx, self.rn, self.rc
        rows, cols, vals = [], [], []
        I, J = np.meshgrid(np.arange(1, Nx), np.arange(Ny), indexing="ij")
        a = (ax[1:-1] * rn[1:-1, None]).ravel()
        L = self.cid(I - 1, J).ravel(); R = self.cid(I, J).ravel()
        aL, aR = (rc[I - 1] * hx).ravel(), (rc[I] * hx).ravel()
        ap, am = np.maximum(a, 0), np.minimum(a, 0)
        for r_, c_, v_ in [(L, L, ap / aL), (L, R, am / aL), (R, L, -ap / aR), (R, R, -am / aR)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        I, J = np.meshgrid(np.arange(Nx), np.arange(1, Ny), indexing="ij")
        a = ay[:, 1:-1].ravel()
        B = self.cid(I, J - 1).ravel(); T = self.cid(I, J).ravel()
        hB = (rc[I] * self.hy[J - 1]).ravel(); hT = (rc[I] * self.hy[J]).ravel()
        ap, am = np.maximum(a, 0), np.minimum(a, 0)
        for r_, c_, v_ in [(B, B, ap / hB), (B, T, am / hB), (T, B, -ap / hT), (T, T, -am / hT)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        return self._coo(rows, cols, vals, (self.nc, self.nc))

    def mass(self):
        A = self.rc[:, None] * self.hy[None, :]
        return (self.C.sum(axis=0) * A).sum() / A.sum()

    # --------------------------------------------------------------- output
    def iso_radius_mean(self, C, r_out):
        """Area-weighted mean of C (nscan, Nx, Ny) on circles of radius r_out
        about the rotor axis (iso-radius optics)."""
        out = np.zeros((C.shape[0], len(r_out)))
        wsum = np.zeros(len(r_out))
        for j in range(self.Ny):
            R = self.radius(self.rc, self.yc[j])          # monotone in rho
            w = np.interp(r_out, R, self.rc) * self.hy[j]
            for k in range(C.shape[0]):
                out[k] += np.interp(r_out, R, C[k, :, j]) * w
            wsum += w
        return out / wsum


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--delta", type=float, default=1.0)
    ap.add_argument("--pivot", type=float, default=6.5)
    ap.add_argument("--alpha", type=float, default=2.5)
    ap.add_argument("--c0", type=float, default=0.1)
    ap.add_argument("--species", default=None, help="s,D,c0;... (overrides --c0: BSA monomer)")
    ap.add_argument("--rpm", type=float, default=50000.)
    ap.add_argument("--r_m", type=float, default=6.2)
    ap.add_argument("--L", type=float, default=0.9)
    ap.add_argument("--Nx", type=int, default=120)
    ap.add_argument("--Ny", type=int, default=40)
    ap.add_argument("--ybeta", type=float, default=3.0)
    ap.add_argument("--dt", type=float, default=5.0, help="maximum time step")
    ap.add_argument("--tend", type=float, default=4000.)
    ap.add_argument("--nscan", type=int, default=10)
    ap.add_argument("--noflow", action="store_true")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    spc = ([tuple(float(v) for v in t.split(",")) for t in a.species.split(";")]
           if a.species else [(4.4e-13, 6.0e-7, a.c0)])
    sim = SectorSolver(spc, delta_deg=a.delta, pivot=a.pivot, alpha_deg=a.alpha, rpm=a.rpm,
                       r_m=a.r_m, L=a.L, Nx=a.Nx, Ny=a.Ny, ybeta=a.ybeta, flow=not a.noflow)
    print(f"rotor axis in cell frame: ({sim.ox:.2e}, {sim.oy:.4f}) cm; "
          f"wall tilt at r_m/pivot/bottom = "
          + " / ".join(f"{np.rad2deg(np.arcsin(sim.oy / r)):.3f}" for r in (a.r_m, a.pivot, a.r_m + a.L))
          + " deg", flush=True)
    t_out = np.linspace(a.tend / a.nscan, a.tend, a.nscan)
    C = sim.run(t_out, a.dt, dt0=min(a.dt, 0.5))
    if a.out:
        np.savez_compressed(a.out, C=C, t=t_out, x=sim.xc, y=sim.yc, yn=sim.yn,
                            r_m=sim.r_m, species=np.array(spc), args=json.dumps(vars(a)))
