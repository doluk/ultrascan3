#!/usr/bin/env python3
"""
Multi-species extension of the BDF2 2D Stokes-Boussinesq-Lamm solver
(misalign2d_bdf2.py).  All species share the partial specific volume, so the
buoyancy source of the vorticity equation acts on the total concentration;
each species has its own s and D.

Time stepping: variable-step BDF2,
  (1+2w)/(1+w) c^{n+1} - (1+w) c^n + w^2/(1+w) c^{n-1} = dt_n f(c^{n+1}),
  w = dt_n / dt_{n-1};
a step whose Newton-Picard iteration does not converge is rejected and
retried with dt/2; dt grows again by 1.25 after 10 accepted steps, up to
dt_max.  Output times are hit exactly.

Usage (BSA monomer/dimer, conditions of Channell et al. 2018):
  misalign2d_multi.py --psi 2 --out runs/m_p2.npz
"""
import numpy as np
import scipy.sparse as sp
import time, argparse, json
from misalign2d_bdf2 import BDF2Solver, _solve


class MultiSolver(BDF2Solver):
    def __init__(self, species, **kw):
        """species: list of (s [s], D [cm^2/s], c0 [mg/mL])"""
        self.species = [tuple(map(float, sp_)) for sp_ in species]
        s0, D0, _ = self.species[0]
        ctot = sum(c for _, _, c in self.species)
        super().__init__(s=s0, D=D0, c0=ctot, **kw)
        self.nsp = len(self.species)
        self.vsx_k = [self.vsx * (s / s0) for s, _, _ in self.species]
        self.vsy_k = [self.vsy * (s / s0) for s, _, _ in self.species]
        self.Ld_k = [self.Ldiff * (D / D0) for _, D, _ in self.species]
        self.C = np.array([np.full((self.Nx, self.Ny), c) for _, _, c in self.species])
        self.C_prev = None
        self.dt_prev = None

    # one (possibly rejected) step; returns True if converged
    def step_multi(self, dt, iters=12, tol=1e-7, fail=1e-5):
        nsp, nc = self.nsp, self.nc
        if self.order == 2 and self.C_prev is not None:
            w = dt / self.dt_prev
            gam = (1 + w) / (1 + 2 * w)
            chist = ((1 + w) ** 2 * self.C - w ** 2 * self.C_prev) / (1 + 2 * w)
        else:
            gam, chist = 1.0, self.C.copy()
        Ck, Uk, Vk = self.C.copy(), self.U, self.V
        dif = np.inf
        for it in range(iters):
            Mcc, Mcp, rhs = [], [], []
            for k in range(nsp):
                ax, ay = Uk + self.vsx_k[k], Vk + self.vsy_k[k]
                ax[0, :] = ax[-1, :] = 0; ay[:, 0] = ay[:, -1] = 0
                cxh, cxu, cyh, cyu = self._faces(Ck[k], ax, ay)
                Mcc.append(sp.identity(nc) / (gam * dt) + self._upwind(ax, ay) - self.Ld_k[k])
                Fx_e = -Uk[1:-1] * cxh + ax[1:-1] * (cxh - cxu)
                Fy_e = -Vk[:, 1:-1] * cyh + ay[:, 1:-1] * (cyh - cyu)
                rhs.append(chist[k].ravel() / (gam * dt) - self._div(Fx_e, Fy_e))
                if self.flow:
                    Wx = np.zeros((self.Nx + 1, self.Ny)); Wx[1:-1] = cxh
                    Wy = np.zeros((self.Nx, self.Ny + 1)); Wy[:, 1:-1] = cyh
                    Mcp.append(self.Dx @ sp.diags(Wx.ravel()) @ self.Gx
                               + self.Dy @ sp.diags(Wy.ravel()) @ self.Gy)
            if self.flow:
                top = sp.hstack([sp.block_diag(Mcc), sp.vstack(Mcp),
                                 sp.csr_matrix((nsp * nc, self.nom))])
                Rk = sp.vstack([sp.csr_matrix((self.npsi, nc)), -self.R,
                                sp.csr_matrix((self.nb, nc))])
                K = sp.vstack([top, sp.hstack([sp.hstack([Rk] * nsp), self.Kflow])]).tocsr()
                x = _solve(K, np.r_[np.concatenate(rhs), np.zeros(self.npsi + self.nom)])
                Cn = x[:nsp * nc].reshape(nsp, self.Nx, self.Ny)
                P = x[nsp * nc:nsp * nc + self.npsi]
                Un = (self.Gx @ P).reshape(self.Nx + 1, self.Ny)
                Vn = (self.Gy @ P).reshape(self.Nx, self.Ny + 1)
            else:
                Cn = np.array([_solve(Mcc[k], rhs[k]) for k in range(nsp)]).reshape(nsp, self.Nx, self.Ny)
                Un, Vn = Uk, Vk
            if not np.all(np.isfinite(Cn)):
                dif = np.inf
                break
            dif = abs(Cn - Ck).max() / self.c0
            Ck, Uk, Vk = Cn, Un, Vn
            if dif < tol:
                break
        self.last_iter = (it + 1, dif)
        if not dif < fail:
            return False
        self.C_prev, self.C, self.U, self.V = self.C, Ck, Uk, Vk
        self.dt_prev = dt
        self.t += dt
        return True

    def run(self, t_out, dt_max, dt0=None, verbose=True):
        out, t0 = [], time.time()
        dt = dt0 or dt_max
        nok, nrej = 0, 0
        for tt in t_out:
            while self.t < tt - 1e-9:
                h = min(dt, tt - self.t)
                if self.dt_prev is not None and h / self.dt_prev > 2.0:
                    h = 2.0 * self.dt_prev          # BDF2 step-ratio limit
                if self.step_multi(h):
                    nok += 1
                    if nok % 10 == 0:
                        dt = min(dt_max, dt * 1.25)
                else:
                    nrej += 1
                    dt = 0.5 * h
                    if dt < 1e-3:
                        raise RuntimeError("time step collapsed")
            out.append(self.C.copy())
            if verbose:
                um = max(abs(self.U).max(), abs(self.V).max())
                print(f"  t={self.t:7.0f}s dt={dt:.2f} it={self.last_iter[0]} res={self.last_iter[1]:.1e}"
                      f" acc/rej={nok}/{nrej} |u|max={um:.2e} cm/s mass={self.mass()/self.c0:.10f}"
                      f" cmin={self.C.min()/self.c0:+.2e} wall={time.time()-t0:7.1f}s", flush=True)
        return np.array(out)                         # (nscan, nsp, Nx, Ny)

    def mass(self):
        return (self.C.sum(axis=0) * self.hy[None, :]).sum() * self.hx / (self.L * self.W)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--psi", type=float, default=1.0)
    ap.add_argument("--species", default="4.4e-13,6.0e-7,0.574;6.6e-13,4.5e-7,0.126",
                    help="s,D,c0 per species separated by ';' (default: BSA 82/18 %% of 0.7 mg/mL)")
    ap.add_argument("--rpm", type=float, default=45000.)
    ap.add_argument("--r_m", type=float, default=6.0)
    ap.add_argument("--L", type=float, default=1.2)
    ap.add_argument("--Nx", type=int, default=120)
    ap.add_argument("--Ny", type=int, default=40)
    ap.add_argument("--ybeta", type=float, default=3.0)
    ap.add_argument("--dt", type=float, default=2.0, help="maximum time step")
    ap.add_argument("--tend", type=float, default=14400.)
    ap.add_argument("--nscan", type=int, default=48)
    ap.add_argument("--noflow", action="store_true")
    ap.add_argument("--out", default=None)
    a = ap.parse_args()
    spc = [tuple(float(v) for v in t.split(",")) for t in a.species.split(";")]
    sim = MultiSolver(spc, psi_deg=a.psi, rpm=a.rpm, r_m=a.r_m, L=a.L, Nx=a.Nx, Ny=a.Ny,
                      ybeta=a.ybeta, flow=not a.noflow)
    t_out = np.linspace(a.tend / a.nscan, a.tend, a.nscan)
    C = sim.run(t_out, a.dt, dt0=min(a.dt, 0.5))
    if a.out:
        np.savez_compressed(a.out, C=C, t=t_out, x=sim.xc, y=sim.yc, yn=sim.yn,
                            r_m=sim.r_m, species=np.array(spc), args=json.dumps(vars(a)))
