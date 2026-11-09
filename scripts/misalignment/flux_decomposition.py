"""
Lateral-mean convective flux of each species in a 2D run, decomposed as
  F_k(x) = <U c_k>_y = v_k c_k - K_k dc_k/dx     (x < L - 0.12 cm)
by least squares at each scan.  v_k / v_sed is the convective drift, K_k / D_k
the convective dispersion.  The flow is recomputed from the saved fields.

Usage: flux_decomposition.py runs/m_p1.npz 3,7,11,15,23,31,39
"""
import sys, json, numpy as np
sys.path.insert(0, __import__("os").path.dirname(__file__))
from misalign2d_bdf2 import _solve
from misalign2d_multi import MultiSolver
fn = sys.argv[1]
d = np.load(fn); a = json.loads(str(d["args"])); spc = d["species"]
S = MultiSolver(spc, psi_deg=a["psi"], rpm=a["rpm"], r_m=a["r_m"], L=a["L"], Nx=a["Nx"], Ny=a["Ny"], ybeta=a["ybeta"])
hy, W = S.hy, S.W; xf = S.xn[1:-1]; m = xf < S.L - 0.12
K0 = S.Kflow.tocsc()
for it in [int(s) for s in sys.argv[2].split(",")]:
    C = d["C"][it]; ct = C.sum(0)
    sol = _solve(K0, np.r_[np.zeros(S.npsi), S.R @ ct.ravel(), np.zeros(S.nb)])
    U = (S.Gx @ sol[:S.npsi]).reshape(S.Nx + 1, S.Ny)[1:-1]
    line = f"t={d['t'][it]:6.0f} "
    for k in range(C.shape[0]):
        ck = C[k] / spc[k, 2]
        F = (U * 0.5 * (ck[1:] + ck[:-1]) * hy).sum(1) / W
        cb = (ck * hy).sum(1) / W
        cf = 0.5 * (cb[1:] + cb[:-1]); g = np.diff(cb) / S.hx
        A = np.c_[cf[m], -g[m]]
        (v, K), *_ = np.linalg.lstsq(A, F[m], rcond=None)
        r2 = 1 - ((A @ [v, K] - F[m])**2).sum() / (F[m]**2).sum()
        vs = spc[k, 0] * S.w2 * (S.r_m + 0.3)
        line += f"| sp{k}: v/vsed={v/vs:+.4f} K/D={K/spc[k,1]:+.3f} R2={r2:.3f} "
    print(line, flush=True)
