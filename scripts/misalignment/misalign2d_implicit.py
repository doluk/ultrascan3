#!/usr/bin/env python3
"""
Implicit 2D Stokes-Boussinesq-Lamm solver for a channel whose walls are
tilted by psi against the (planar, curl-free) centrifugal field.  Physics and
frame as in misalign2d.py:

  dc/dt + div( c (u + s G g) - D grad c ) = 0            no flux on walls
  mu lap^2 Psi = beta [ sin(psi) d(cG)/dx - cos(psi) d(cG)/dy ]
  u = dPsi/dy, v = -dPsi/dx,  Psi = dPsi/dn = 0 on walls  (no slip)

Time stepping: linearly implicit backward Euler of the coupled (c, Psi)
system.  The bilinear advective flux c*u(Psi) is linearised about step n,
  F = a^n c_up^{n+1} + (U^{n+1} - U^n) c_hr^n + a^n (c_hr^n - c_up^n),
(a = total face velocity, c_up = upwind face value, c_hr = minmod-limited
second-order face value; the last term is an explicit deferred correction),
so each step solves one sparse linear system for (c^{n+1}, Psi^{n+1}).  The
buoyancy-flow coupling, which makes explicit schemes unusable, is implicit.
"""
import numpy as np
import scipy.sparse as sp
import scipy.sparse.linalg as spla
import time, argparse, json
from misalign2d import d2_dirichlet, d4_clamped
try:                                   # PARDISO (MKL) if available
    import pypardiso
    def _solve(A, b):
        return pypardiso.spsolve(A.tocsr(), b)
except ImportError:
    def _solve(A, b):
        return spla.spsolve(A.tocsc(), b)


def minmod(a, b):
    return np.where(a * b > 0, np.sign(a) * np.minimum(abs(a), abs(b)), 0.0)


class Implicit2D:
    def __init__(self, psi_deg=1.0, rpm=50000., r_m=6.2, L=0.9, W=0.28,
                 s=4.4e-13, D=6.0e-7, c0=1.0, vbar=0.73, rho0=1.0, mu=0.01,
                 Nx=240, Ny=80, flow=True, hr=False):
        self.hr = hr                  # explicit 2nd-order deferred correction
        self.psi = np.deg2rad(psi_deg)
        self.w2 = (rpm * np.pi / 30.) ** 2
        self.r_m, self.L, self.W = r_m, L, W
        self.s, self.D, self.mu = s, D, mu
        self.beta = (1. - vbar * rho0) * 1e-3
        self.Nx, self.Ny = Nx, Ny
        hx, hy = L / Nx, W / Ny
        self.hx, self.hy = hx, hy
        self.flow = flow and psi_deg != 0.0
        self.xc = (np.arange(Nx) + 0.5) * hx
        self.yc = (np.arange(Ny) + 0.5) * hy
        xf, yf = np.arange(Nx + 1) * hx, np.arange(Ny + 1) * hy
        cp, sn = np.cos(self.psi), np.sin(self.psi)
        G = lambda x, y: self.w2 * (r_m + x[:, None] * cp + (y[None, :] - W / 2) * sn)
        self.Gc = G(self.xc, self.yc)
        # sedimentation velocity on faces (walls: zero)
        self.vsx = s * G(xf, self.yc) * cp            # (Nx+1, Ny)
        self.vsy = s * G(self.xc, yf) * sn            # (Nx, Ny+1)
        self.vsx[0, :] = self.vsx[-1, :] = 0
        self.vsy[:, 0] = self.vsy[:, -1] = 0
        self.c = np.full((Nx, Ny), c0)
        self.c0 = c0
        self.nc = Nx * Ny
        self.npsi = (Nx - 1) * (Ny - 1)
        self.t = 0.0
        self._build_static()
        self.U = np.zeros((Nx + 1, Ny))
        self.V = np.zeros((Nx, Ny + 1))

    # ------------------------------------------------------------ operators
    def cid(self, i, j):
        return i * self.Ny + j

    def nid(self, i, j):          # interior node (i in 1..Nx-1, j in 1..Ny-1)
        return (i - 1) * (self.Ny - 1) + (j - 1)

    def _build_static(self):
        Nx, Ny, hx, hy = self.Nx, self.Ny, self.hx, self.hy
        nxi, nyi = Nx - 1, Ny - 1
        # biharmonic (nodes)
        Ix, Iy = sp.identity(nxi), sp.identity(nyi)
        self.B = (self.mu * (sp.kron(d4_clamped(nxi, hx), Iy)
                             + 2 * sp.kron(d2_dirichlet(nxi, hx), d2_dirichlet(nyi, hy))
                             + sp.kron(Ix, d4_clamped(nyi, hy)))).tocsr()
        # buoyancy map R: cells -> nodes
        sn, cs, b = np.sin(self.psi), np.cos(self.psi), self.beta
        rows, cols, vals = [], [], []
        I, J = np.meshgrid(np.arange(1, Nx), np.arange(1, Ny), indexing="ij")
        I, J = I.ravel(), J.ravel()
        n = self.nid(I, J)
        for di, dj, fx, fy in [(0, 0, 1, -1), (0, -1, 1, 1), (-1, 0, -1, -1), (-1, -1, -1, 1)]:
            ci, cj = I + di, J + dj
            rows.append(n); cols.append(self.cid(ci, cj))
            vals.append(b * self.Gc[ci, cj] * (fx * sn / (2 * hx) + fy * cs / (2 * hy)))
        self.R = sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                               shape=(self.npsi, self.nc))
        # Psi -> face velocities
        rows, cols, vals = [], [], []
        # x-faces i=1..Nx-1, j=0..Ny-1:  U = (Psi[i,j+1]-Psi[i,j])/hy
        I, J = np.meshgrid(np.arange(1, Nx), np.arange(Ny), indexing="ij")
        I, J = I.ravel(), J.ravel()
        f = I * Ny + J                                   # index into (Nx+1, Ny)
        m = J + 1 <= Ny - 1
        rows.append(f[m]); cols.append(self.nid(I[m], J[m] + 1)); vals.append(np.full(m.sum(), 1 / hy))
        m = J >= 1
        rows.append(f[m]); cols.append(self.nid(I[m], J[m])); vals.append(np.full(m.sum(), -1 / hy))
        self.Gx = sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                                shape=((Nx + 1) * Ny, self.npsi))
        rows, cols, vals = [], [], []
        # y-faces i=0..Nx-1, j=1..Ny-1:  V = -(Psi[i+1,j]-Psi[i,j])/hx
        I, J = np.meshgrid(np.arange(Nx), np.arange(1, Ny), indexing="ij")
        I, J = I.ravel(), J.ravel()
        f = I * (Ny + 1) + J
        m = I + 1 <= Nx - 1
        rows.append(f[m]); cols.append(self.nid(I[m] + 1, J[m])); vals.append(np.full(m.sum(), -1 / hx))
        m = I >= 1
        rows.append(f[m]); cols.append(self.nid(I[m], J[m])); vals.append(np.full(m.sum(), 1 / hx))
        self.Gy = sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                                shape=(Nx * (Ny + 1), self.npsi))
        # diffusion (cells, no flux)
        Lx = self._lap1d(Nx, hx)
        Ly = self._lap1d(Ny, hy)
        self.Ldiff = self.D * (sp.kron(Lx, sp.identity(Ny)) + sp.kron(sp.identity(Nx), Ly))

    @staticmethod
    def _lap1d(n, h):
        e = np.ones(n)
        d = -2 * e
        d[0] = d[-1] = -1
        return sp.diags([e[:-1], d, e[:-1]], [-1, 0, 1]) / h**2

    # ------------------------------------------------------- face values
    def _hr_faces(self, c, ax, ay):
        """minmod 2nd-order upwind face values on internal faces."""
        dcx = np.diff(c, axis=0)
        slx = np.zeros_like(c); slx[1:-1] = minmod(dcx[:-1], dcx[1:])
        cxf = np.where(ax[1:-1] > 0, c[:-1] + 0.5 * slx[:-1], c[1:] - 0.5 * slx[1:])
        cxu = np.where(ax[1:-1] > 0, c[:-1], c[1:])
        dcy = np.diff(c, axis=1)
        sly = np.zeros_like(c); sly[:, 1:-1] = minmod(dcy[:, :-1], dcy[:, 1:])
        cyf = np.where(ay[:, 1:-1] > 0, c[:, :-1] + 0.5 * sly[:, :-1], c[:, 1:] - 0.5 * sly[:, 1:])
        cyu = np.where(ay[:, 1:-1] > 0, c[:, :-1], c[:, 1:])
        return cxf, cxu, cyf, cyu

    def _upwind_matrix(self, ax, ay):
        """div of upwind flux a*c_up  as matrix on cells."""
        Nx, Ny, hx, hy = self.Nx, self.Ny, self.hx, self.hy
        rows, cols, vals = [], [], []
        # x faces between (i-1,j) [L] and (i,j) [R], i=1..Nx-1
        I, J = np.meshgrid(np.arange(1, Nx), np.arange(Ny), indexing="ij")
        a = ax[1:-1].ravel(); L = self.cid(I - 1, J).ravel(); R = self.cid(I, J).ravel()
        ap, am = np.maximum(a, 0) / hx, np.minimum(a, 0) / hx
        for r_, c_, v_ in [(L, L, ap), (L, R, am), (R, L, -ap), (R, R, -am)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        I, J = np.meshgrid(np.arange(Nx), np.arange(1, Ny), indexing="ij")
        a = ay[:, 1:-1].ravel(); L = self.cid(I, J - 1).ravel(); R = self.cid(I, J).ravel()
        ap, am = np.maximum(a, 0) / hy, np.minimum(a, 0) / hy
        for r_, c_, v_ in [(L, L, ap), (L, R, am), (R, L, -ap), (R, R, -am)]:
            rows.append(r_); cols.append(c_); vals.append(v_)
        return sp.csr_matrix((np.concatenate(vals), (np.concatenate(rows), np.concatenate(cols))),
                             shape=(self.nc, self.nc))

    def _div_faces(self, Fx_int, Fy_int):
        """divergence of internal-face fluxes (walls zero) -> cells (flat)."""
        Fx = np.zeros((self.Nx + 1, self.Ny)); Fx[1:-1] = Fx_int
        Fy = np.zeros((self.Nx, self.Ny + 1)); Fy[:, 1:-1] = Fy_int
        return (np.diff(Fx, axis=0) / self.hx + np.diff(Fy, axis=1) / self.hy).ravel()

    def _div_face_matrix(self, wx, wy):
        """matrix: Psi -> div( w * U(Psi) ) with face weights w (internal)."""
        Nx, Ny = self.Nx, self.Ny
        # D_x: cells <- x faces ; flux at face i contributes +1/hx to cell i-1, -1/hx to cell i
        Wx = np.zeros((Nx + 1, Ny)); Wx[1:-1] = wx
        Wy = np.zeros((Nx, Ny + 1)); Wy[:, 1:-1] = wy
        Dx = self._Dx(); Dy = self._Dy()
        return (Dx @ sp.diags(Wx.ravel()) @ self.Gx + Dy @ sp.diags(Wy.ravel()) @ self.Gy)

    def _Dx(self):
        if not hasattr(self, "_dx"):
            Nx, Ny = self.Nx, self.Ny
            I, J = np.meshgrid(np.arange(Nx), np.arange(Ny), indexing="ij")
            c = self.cid(I, J).ravel()
            fR = ((I + 1) * Ny + J).ravel(); fL = (I * Ny + J).ravel()
            self._dx = sp.csr_matrix((np.r_[np.full(c.size, 1 / self.hx), np.full(c.size, -1 / self.hx)],
                                      (np.r_[c, c], np.r_[fR, fL])), shape=(self.nc, (Nx + 1) * Ny))
        return self._dx

    def _Dy(self):
        if not hasattr(self, "_dy"):
            Nx, Ny = self.Nx, self.Ny
            I, J = np.meshgrid(np.arange(Nx), np.arange(Ny), indexing="ij")
            c = self.cid(I, J).ravel()
            fR = (I * (Ny + 1) + J + 1).ravel(); fL = (I * (Ny + 1) + J).ravel()
            self._dy = sp.csr_matrix((np.r_[np.full(c.size, 1 / self.hy), np.full(c.size, -1 / self.hy)],
                                      (np.r_[c, c], np.r_[fR, fL])), shape=(self.nc, Nx * (Ny + 1)))
        return self._dy

    # ----------------------------------------------------------------- step
    def step(self, dt, iters=None, tol=1e-6):
        """Backward Euler step; Newton-Picard iterations on the bilinear flux
        (and on the deferred high-resolution correction when hr=True)."""
        iters = iters if iters is not None else (8 if self.hr else 1)
        cn_old = self.c.ravel()
        ck, Uk, Vk = self.c, self.U, self.V
        for it in range(iters):
            ax, ay = Uk + self.vsx, Vk + self.vsy
            ax[0, :] = ax[-1, :] = 0; ay[:, 0] = ay[:, -1] = 0
            cxf, cxu, cyf, cyu = self._hr_faces(ck, ax, ay)
            if not self.hr:
                cxf, cyf = cxu, cyu
            A = self._upwind_matrix(ax, ay)
            Mcc = sp.identity(self.nc) / dt + A - self.Ldiff
            # explicit parts:  -U^k c_hr^k + a^k (c_hr^k - c_up^k)
            Fx_e = -Uk[1:-1] * cxf + ax[1:-1] * (cxf - cxu)
            Fy_e = -Vk[:, 1:-1] * cyf + ay[:, 1:-1] * (cyf - cyu)
            rhs_c = cn_old / dt - self._div_faces(Fx_e, Fy_e)
            if self.flow:
                Mcp = self._div_face_matrix(cxf, cyf)
                K = sp.bmat([[Mcc, Mcp], [-self.R, self.B]], format="csr")
                x = _solve(K, np.r_[rhs_c, np.zeros(self.npsi)])
                c_new, P = x[:self.nc], x[self.nc:]
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
        self.c, self.U, self.V = ck, Uk, Vk
        self.t += dt

    def run(self, t_out, dt, verbose=True):
        out, t0 = [], time.time()
        for tt in t_out:
            while self.t < tt - 1e-9:
                self.step(min(dt, tt - self.t))
            out.append(self.c.copy())
            if verbose:
                um = max(abs(self.U).max(), abs(self.V).max())
                print(f"  t={self.t:7.0f}s it={self.last_iter[0]} res={self.last_iter[1]:.1e} |u|max={um:.2e} cm/s mass={self.c.mean()/self.c0:.8f}"
                      f" cmin={self.c.min()/self.c0:+.2e} wall={time.time()-t0:6.1f}s", flush=True)
        return np.array(out)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--psi", type=float, default=1.0)
    ap.add_argument("--c0", type=float, default=1.0)
    ap.add_argument("--Nx", type=int, default=240)
    ap.add_argument("--Ny", type=int, default=80)
    ap.add_argument("--dt", type=float, default=5.0)
    ap.add_argument("--tend", type=float, default=4000.)
    ap.add_argument("--nscan", type=int, default=10)
    ap.add_argument("--hr", action="store_true")
    ap.add_argument("--noflow", action="store_true", help="no buoyancy-driven flow")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    sim = Implicit2D(psi_deg=a.psi, c0=a.c0, Nx=a.Nx, Ny=a.Ny, hr=a.hr,
                     flow=not a.noflow)
    t_out = np.linspace(a.tend / a.nscan, a.tend, a.nscan)
    C = sim.run(t_out, a.dt)
    if a.out:
        np.savez_compressed(a.out, C=C, t=t_out, x=sim.xc, y=sim.yc, r_m=sim.r_m,
                            args=json.dumps(vars(a)))
