import numpy as np, glob, sys
from scipy.optimize import minimize_scalar
X = sys.argv[1]
def load(d):
    out = []
    for f in sorted(glob.glob(f"{X}/{d}/f*.ip5")):
        L = open(f).read().split("\n"); h = L[1].split()
        a = np.loadtxt(L[2:]); out.append((float(h[3]), a[:, 0], a[:, 1]))
    return out
H, S = load("hinderedsingle"), load("skewed")
def skew(x, c, k):
    g = np.gradient(c, x); rho = x + k * g
    rho = np.maximum.accumulate(rho)
    return np.interp(x, rho, c)
def err(k):
    return np.sqrt(np.mean(np.concatenate([(skew(h[1], h[2], k) - s[2]) ** 2 for h, s in zip(H, S)])))
print("rmsd(skewed - unskewed) =", err(0.0))
print("rmsd(skewed - ours, kappa'=2.69e-5) =", err(2.69e-5))
r = minimize_scalar(err, bounds=(0, 6e-5), method="bounded")
print("best kappa' =", r.x, "rmsd", r.fun)
k = 2.69e-5
for i in (0, 5, 15, 29):
    h, s = H[i], S[i]; d = skew(h[1], h[2], k) - s[2]
    print(f"scan t={h[0]:.0f}: max|SEDFIT skew effect| {np.abs(s[2]-h[2]).max():.3f}  max|ours - SEDFIT| {np.abs(d).max():.4f}")
