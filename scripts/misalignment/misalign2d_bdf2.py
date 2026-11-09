#!/usr/bin/env python3
"""
Implicit 2D Stokes-Boussinesq-Lamm solver, second order in time (BDF2) and
on a wall-clustered (tanh-stretched) lateral grid.  Physics and frame as in
misalign2d.py / misalign2d_implicit.py:

  dc/dt + div( c (u + s G g) - D grad c ) = 0              no flux on walls
  -mu lap(omega) = beta [ sin(psi) d(cG)/dx - cos(psi) d(cG)/dy ]
  omega = -lap(Psi),  u = dPsi/dy,  v = -dPsi/dx
  Psi = 0 and dPsi/dn = 0 on walls (no slip), imposed through Wood's
  second-order wall vorticity  omega_w = -3 Psi_1 / h1^2 - omega_1 / 2.

Discretisation
  * c: finite volumes on cells; minmod-limited second-order upwind face
    values for all advection (nonuniform spacing), central diffusion.
  * Psi on interior nodes, omega on all nodes; nonuniform 3-point Laplacians.
  * Time: BDF2 (first step backward Euler), fixed dt.  The bilinear flux
    c*u(Psi) and the limiter are converged with Newton-Picard iterations, so
    each step solves the fully implicit nonlinear system; every iteration is
    one sparse solve of the coupled (c, Psi, omega) system.
"""
import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla
import time, argparse, json

try:                                   # PARDISO (MKL) if available
    import pypardiso

    def _solve(A, b):
        return pypardiso.spsolve(A.tocsr(), b)
except ImportError:
    def _solve(A, b):
        return spla.spsolve(A.tocsc(), b)


def minmod(a, b):
    return np.where(a * b > 0, np.sign(a) * np.minimum(abs(a), abs(b)), 0.0)


def stretched(n, W, beta):
    """n cells on [0, W], tanh clustering at both walls (beta=0: uniform)."""
    eta = np.linspace(0.0, 1.0, n + 1)
    if beta <= 0:
        return W * eta
    return 0.5 * W * (1.0 + np.tanh(beta * (2 * eta - 1)) / np.tanh(beta))


def lap1d_nodes(z):
    """3-point second derivative at interior nodes of the node set z:
    returns (lower, diag, upper) coefficient arrays for nodes 1..n-1."""
    hm = z[1:-1] - z[:-2]
    hp = z[2:] - z[1:-1]
    lo = 2.0 / (hm * (hm + hp))
    up = 2.0 / (hp * (hm + hp))
    return lo, -(lo + up), up


class BDF2Solver:
    def __init__(self, psi_deg=1.0, rpm=50000., r_m=6.2, L=0.9, W=0.28,
                 s=4.4e-13, D=6.0e-7, c0=1.0, vbar=0.73, rho0=1.0, mu=0.01,
                 Nx=240, Ny=80, ybeta=0.0, flow=True, order=2):
        self.psi = np.deg2rad(psi_deg)
        self.w2 = (rpm * np.pi / 30.) ** 2
        self.r_m, self.L, self.W = r_m, L, W
        self.s, self.D, self.mu = s, D, mu
        self.beta = (1. - vbar * rho0) * 1e-3
        self.Nx, self.Ny = Nx, Ny
        self.order = order
        self.flow = flow and psi_deg != 0.0
        self.xn = np.linspace(0, L, Nx + 1)
        self.yn = stretched(Ny, W, ybeta)
        self.hx = L / Nx
        self.hy = np.diff(self.yn)                       # cell widths (Ny)
        self.xc = 0.5 * (self.xn[1:] + self.xn[:-1])
        self.yc = 0.5 * (self.yn[1:] + self.yn[:-1])
        self.x, self.y = self.xc, self.yc                # analysis compatibility
        self.dyc = np.diff(self.yc)                      # centre distances (Ny-1)
        cp, sn = np.cos(self.psi), np.sin(self.psi)
        G = lambda x, y: self.w2 * (r_m + x[:, None] * cp + (y[None, :] - W / 2) * sn)
        self.Gc = G(self.xc, self.yc)
        self.vsx = s * G(self.xn, self.yc) * cp          # (Nx+1, Ny)
        self.vsy = s * G(self.xc, self.yn) * sn          # (Nx, Ny+1)
        self.vsx[0, :] = self.vsx[-1, :] = 0
        self.vsy[:, 0] = self.vsy[:, -1] = 0
        self.c0 = c0
        self.c = np.full((Nx, Ny), c0)
        self.c_prev = None
        self.nc = Nx * Ny
        self.npsi = (Nx - 1) * (Ny - 1)
        self.nom = (Nx + 1) * (Ny + 1)
        self.U = np.zeros((Nx + 1, Ny))
        self.V = np.zeros((Nx, Ny + 1))
        self.t = 0.0
        self._build_static()

    # ------------------------------------------------------------ indices
    def cid(self, i, j):
        return i * self.Ny + j

    def pid(self, i, j):          # interior node, i in 1..Nx-1, j in 1..Ny-1
        return (i - 1) * (self.Ny - 1) + (j - 1)

    def oid(self, i, j):          # any node
        return i * (self.Ny + 1) + j

    @staticmethod
    def _coo(rows, cols, vals, shape):
        return sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                             shape=shape)

    def _build_static(self):
        Nx, Ny, hx = self.Nx, self.Ny, self.hx
        yn = self.yn
        # --- Laplacian pieces at interior nodes
        lx = np.array([1, -2, 1]) / hx**2
        ylo, ydi, yup = lap1d_nodes(yn)                  # for j = 1..Ny-1
        I, J = np.meshgrid(np.arange(1, Nx), np.arange(1, Ny), indexing="ij")
        I, J = I.ravel(), J.ravel()
        jj = J - 1
        # Lap(Psi) at interior nodes (Psi = 0 on boundary): psi rows x psi cols
        rows, cols, vals = [], [], []
        p = self.pid(I, J)
        for di, w in [(-1, lx[0]), (1, lx[2])]:
            m = (I + di >= 1) & (I + di <= Nx - 1)
            rows.append(p[m]); cols.append(self.pid(I[m] + di, J[m])); vals.append(np.full(m.sum(), w))
        for dj, w in [(-1, ylo[jj]), (1, yup[jj])]:
            m = (J + dj >= 1) & (J + dj <= Ny - 1)
            rows.append(p[m]); cols.append(self.pid(I[m], J[m] + dj)); vals.append(w[m])
        rows.append(p); cols.append(p); vals.append(lx[1] + ydi[jj])
        self.LapP = self._coo(rows, cols, vals, (self.npsi, self.npsi))
        # Lap(omega) at interior nodes: psi rows x omega cols (all nodes)
        rows, cols, vals = [], [], []
        for di, w in [(-1, lx[0]), (1, lx[2])]:
            rows.append(p); cols.append(self.oid(I + di, J)); vals.append(np.full(p.size, w))
        for dj, w in [(-1, ylo[jj]), (1, yup[jj])]:
            rows.append(p); cols.append(self.oid(I, J + dj)); vals.append(w)
        rows.append(p); cols.append(self.oid(I, J)); vals.append(lx[1] + ydi[jj])
        self.LapO = self._coo(rows, cols, vals, (self.npsi, self.nom))
        # identity: omega at interior nodes
        self.Eint = sp.csr_matrix((np.ones(p.size), (p, self.oid(I, J))), shape=(self.npsi, self.nom))
        # --- wall vorticity (Wood) rows, one per boundary node
        brow_o, brow_p, wo, wp = [], [], [], []
        bnodes = []
        k = 0
        def add_wall(ib, jb, ii, ji, h):
            nonlocal k
            bnodes.append(self.oid(ib, jb))
            brow_o.extend([k, k]); wo.extend([(self.oid(ib, jb), 1.0), (self.oid(ii, ji), 0.5)])
            if 1 <= ii <= Nx - 1 and 1 <= ji <= Ny - 1:
                brow_p.append(k); wp.append((self.pid(ii, ji), 3.0 / h**2))
            k += 1
        for j in range(1, Ny):
            add_wall(0, j, 1, j, hx)
            add_wall(Nx, j, Nx - 1, j, hx)
        for i in range(1, Nx):
            add_wall(i, 0, i, 1, yn[1] - yn[0])
            add_wall(i, Ny, i, Ny - 1, yn[Ny] - yn[Ny - 1])
        for (ib, jb) in [(0, 0), (0, Ny), (Nx, 0), (Nx, Ny)]:    # corners: omega = 0
            brow_o.append(k); wo.append((self.oid(ib, jb), 1.0)); k += 1
        self.nb = k
        self.Wo = sp.csr_matrix(([w for _, w in wo], (brow_o, [c for c, _ in wo])), shape=(k, self.nom))
        self.Wp = sp.csr_matrix(([w for _, w in wp], (brow_p, [c for c, _ in wp])), shape=(k, self.npsi))
        assert self.npsi + self.nb == self.nom
        # --- buoyancy map R: cells -> interior nodes
        sn, cs, b = np.sin(self.psi), np.cos(self.psi), self.beta
        dyc = self.dyc[J - 1]                            # yc_j - yc_{j-1}
        rows, cols, vals = [], [], []
        for di, dj, fx, fy in [(0, 0, 1, 1), (0, -1, 1, -1), (-1, 0, -1, 1), (-1, -1, -1, -1)]:
            ci, cj = I + di, J + dj
            rows.append(p); cols.append(self.cid(ci, cj))
            vals.append(b * self.Gc[ci, cj] * (fx * sn / (2 * hx) - fy * cs / (2 * dyc)))
        self.R = self._coo(rows, cols, vals, (self.npsi, self.nc))
        # --- Psi -> face velocities
        hy = self.hy
        rows, cols, vals = [], [], []
        I2, J2 = np.meshgrid(np.arange(1, Nx), np.arange(Ny), indexing="ij")
        I2, J2 = I2.ravel(), J2.ravel()
        f = I2 * Ny + J2
        m = J2 + 1 <= Ny - 1
        rows.append(f[m]); cols.append(self.pid(I2[m], J2[m] + 1)); vals.append(1 / hy[J2[m]])
        m = J2 >= 1
        rows.append(f[m]); cols.append(self.pid(I2[m], J2[m])); vals.append(-1 / hy[J2[m]])
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
        # --- face divergence operators (cells <- faces)
        Ic, Jc = np.meshgrid(np.arange(Nx), np.arange(Ny), indexing="ij")
        c = self.cid(Ic, Jc).ravel()
        fR = ((Ic + 1) * Ny + Jc).ravel(); fL = (Ic * Ny + Jc).ravel()
        self.Dx = sp.csr_matrix((np.r_[np.full(c.size, 1 / hx), np.full(c.size, -1 / hx)],
                                 (np.r_[c, c], np.r_[fR, fL])), shape=(self.nc, (Nx + 1) * Ny))
        ihy = (1 / hy[Jc]).ravel()
        fT = (Ic * (Ny + 1) + Jc + 1).ravel(); fB = (Ic * (Ny + 1) + Jc).ravel()
        self.Dy = sp.csr_matrix((np.r_[ihy, -ihy], (np.r_[c, c], np.r_[fT, fB])),
                                shape=(self.nc, Nx * (Ny + 1)))
        # --- diffusion: D * div(grad c), no flux at walls
        rows, cols, vals = [], [], []
        # x faces
        Ix, Jx = np.meshgrid(np.arange(1, Nx), np.arange(Ny), indexing="ij")
        Lc, Rc = self.cid(Ix - 1, Jx).ravel(), self.cid(Ix, Jx).ravel()
        g = np.full(Lc.size, self.D / hx)                # flux coefficient
        for r_, c_, v_ in [(Lc, Lc, -g / hx), (Lc, Rc, g / hx), (Rc, Rc, -g / hx), (Rc, Lc, g / hx)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        Iy, Jy = np.meshgrid(np.arange(Nx), np.arange(1, Ny), indexing="ij")
        Bc, Tc = self.cid(Iy, Jy - 1).ravel(), self.cid(Iy, Jy).ravel()
        g = (self.D / self.dyc[Jy - 1]).ravel()
        hB, hT = hy[Jy - 1].ravel(), hy[Jy].ravel()
        for r_, c_, v_ in [(Bc, Bc, -g / hB), (Bc, Tc, g / hB), (Tc, Tc, -g / hT), (Tc, Bc, g / hT)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        self.Ldiff = self._coo(rows, cols, vals, (self.nc, self.nc))
        # static flow block: [ Lap_P   Eint ] rows psi  (omega + lap Psi = 0)
        #                    [ 0     -mu LapO] rows psi (vorticity eq.)  + R c
        #                    [ Wp      Wo   ] rows wall
        self.Kflow = sp.bmat([[self.LapP, self.Eint],
                              [None, -self.mu * self.LapO],
                              [self.Wp, self.Wo]], format="csr")

    # ------------------------------------------------------- face values
    def _faces(self, c, ax, ay):
        hx = self.hx
        # x direction (uniform)
        g = np.diff(c, axis=0) / hx
        sl = np.zeros_like(c); sl[1:-1] = minmod(g[:-1], g[1:])
        cxh = np.where(ax[1:-1] > 0, c[:-1] + 0.5 * hx * sl[:-1], c[1:] - 0.5 * hx * sl[1:])
        cxu = np.where(ax[1:-1] > 0, c[:-1], c[1:])
        # y direction (nonuniform)
        g = np.diff(c, axis=1) / self.dyc[None, :]
        sl = np.zeros_like(c); sl[:, 1:-1] = minmod(g[:, :-1], g[:, 1:])
        dlo = (self.yn[1:-1] - self.yc[:-1])[None, :]    # from lower centre up to face
        dhi = (self.yc[1:] - self.yn[1:-1])[None, :]
        cyh = np.where(ay[:, 1:-1] > 0, c[:, :-1] + dlo * sl[:, :-1], c[:, 1:] - dhi * sl[:, 1:])
        cyu = np.where(ay[:, 1:-1] > 0, c[:, :-1], c[:, 1:])
        return cxh, cxu, cyh, cyu

    def _upwind(self, ax, ay):
        Nx, Ny = self.Nx, self.Ny
        rows, cols, vals = [], [], []
        I, J = np.meshgrid(np.arange(1, Nx), np.arange(Ny), indexing="ij")
        a = ax[1:-1].ravel(); L = self.cid(I - 1, J).ravel(); R = self.cid(I, J).ravel()
        ap, am = np.maximum(a, 0) / self.hx, np.minimum(a, 0) / self.hx
        for r_, c_, v_ in [(L, L, ap), (L, R, am), (R, L, -ap), (R, R, -am)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        I, J = np.meshgrid(np.arange(Nx), np.arange(1, Ny), indexing="ij")
        a = ay[:, 1:-1].ravel(); B = self.cid(I, J - 1).ravel(); T = self.cid(I, J).ravel()
        hB, hT = self.hy[J - 1].ravel(), self.hy[J].ravel()
        ap, am = np.maximum(a, 0), np.minimum(a, 0)
        for r_, c_, v_ in [(B, B, ap / hB), (B, T, am / hB), (T, B, -ap / hT), (T, T, -am / hT)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        return self._coo(rows, cols, vals, (self.nc, self.nc))

    def _div(self, Fx_int, Fy_int):
        Fx = np.zeros((self.Nx + 1, self.Ny)); Fx[1:-1] = Fx_int
        Fy = np.zeros((self.Nx, self.Ny + 1)); Fy[:, 1:-1] = Fy_int
        return self.Dx @ Fx.ravel() + self.Dy @ Fy.ravel()

    # ----------------------------------------------------------------- step
    def step(self, dt, iters=10, tol=1e-7):
        bdf2 = self.order == 2 and self.c_prev is not None
        if bdf2:
            gam, chist = 2.0 / 3.0, (4.0 * self.c - self.c_prev) / 3.0
        else:
            gam, chist = 1.0, self.c
        chist = chist.ravel()
        ck, Uk, Vk = self.c, self.U, self.V
        for it in range(iters):
            ax, ay = Uk + self.vsx, Vk + self.vsy
            ax[0, :] = ax[-1, :] = 0; ay[:, 0] = ay[:, -1] = 0
            cxh, cxu, cyh, cyu = self._faces(ck, ax, ay)
            Mcc = sp.identity(self.nc) / (gam * dt) + self._upwind(ax, ay) - self.Ldiff
            Fx_e = -Uk[1:-1] * cxh + ax[1:-1] * (cxh - cxu)
            Fy_e = -Vk[:, 1:-1] * cyh + ay[:, 1:-1] * (cyh - cyu)
            rhs_c = chist / (gam * dt) - self._div(Fx_e, Fy_e)
            if self.flow:
                Wx = np.zeros((self.Nx + 1, self.Ny)); Wx[1:-1] = cxh
                Wy = np.zeros((self.Nx, self.Ny + 1)); Wy[:, 1:-1] = cyh
                Mcp = self.Dx @ sp.diags(Wx.ravel()) @ self.Gx + self.Dy @ sp.diags(Wy.ravel()) @ self.Gy
                Zco = sp.csr_matrix((self.nc, self.nom))
                top = sp.hstack([Mcc, Mcp, Zco])
                Rfull = sp.vstack([sp.csr_matrix((self.npsi, self.nc)), -self.R,
                                   sp.csr_matrix((self.nb, self.nc))])
                K = sp.vstack([top, sp.hstack([Rfull, self.Kflow])]).tocsr()
                x = _solve(K, np.r_[rhs_c, np.zeros(self.npsi + self.nom)])
                c_new = x[:self.nc]
                P = x[self.nc:self.nc + self.npsi]
                Unew = (self.Gx @ P).reshape(self.Nx + 1, self.Ny)
                Vnew = (self.Gy @ P).reshape(self.Nx, self.Ny + 1)
            else:
                c_new = _solve(Mcc, rhs_c)
                Unew, Vnew = Uk, Vk
            c_new = c_new.reshape(self.Nx, self.Ny)
            dif = abs(c_new - ck).max() / self.c0
            ck, Uk, Vk = c_new, Unew, Vnew
            if dif < tol:
                break
        self.last_iter = (it + 1, dif)
        self.c_prev, self.c, self.U, self.V = self.c, ck, Uk, Vk
        self.t += dt

    def run(self, t_out, dt, verbose=True):
        out, t0 = [], time.time()
        for tt in t_out:
            n = int(round((tt - self.t) / dt))
            assert abs(n * dt - (tt - self.t)) < 1e-6, "dt must divide the output interval"
            for _ in range(n):
                self.step(dt)
            out.append(self.c.copy())
            if verbose:
                um = max(abs(self.U).max(), abs(self.V).max())
                print(f"  t={self.t:7.0f}s it={self.last_iter[0]} res={self.last_iter[1]:.1e}"
                      f" |u|max={um:.2e} cm/s mass={self.mass()/self.c0:.10f}"
                      f" cmin={self.c.min()/self.c0:+.2e} wall={time.time()-t0:6.1f}s", flush=True)
        return np.array(out)

    def mass(self):
        return (self.c * self.hy[None, :]).sum() * self.hx / (self.L * self.W)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--psi", type=float, default=1.0)
    ap.add_argument("--c0", type=float, default=0.01)
    ap.add_argument("--Nx", type=int, default=120)
    ap.add_argument("--Ny", type=int, default=40)
    ap.add_argument("--ybeta", type=float, default=0.0, help="tanh wall clustering")
    ap.add_argument("--dt", type=float, default=5.0)
    ap.add_argument("--order", type=int, default=2)
    ap.add_argument("--tend", type=float, default=4000.)
    ap.add_argument("--nscan", type=int, default=10)
    ap.add_argument("--noflow", action="store_true")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    sim = BDF2Solver(psi_deg=a.psi, c0=a.c0, Nx=a.Nx, Ny=a.Ny, ybeta=a.ybeta,
                     flow=not a.noflow, order=a.order)
    t_out = np.linspace(a.tend / a.nscan, a.tend, a.nscan)
    C = sim.run(t_out, a.dt)
    if a.out:
        np.savez_compressed(a.out, C=C, t=t_out, x=sim.xc, y=sim.yc, yn=sim.yn,
                            r_m=sim.r_m, args=json.dumps(vars(a)))
