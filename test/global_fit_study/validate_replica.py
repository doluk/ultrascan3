#!/usr/bin/env python3
"""Check that the NNLS replica of nnls_study.py reproduces the real
US_SolveSim global fit (gf_basis fit), with the OD limit as the current code
applies it (unscaled, against normalized data) and as F1 applies it.

Usage:  validate_replica.py <basis-dir> <gf_basis binary>
"""
import json
import subprocess
import sys
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parent))
import nnls_study as N  # noqa: E402

bdir, gfb = sys.argv[1], sys.argv[2]
basis = N.Basis(bdir, 60000)
rng = np.random.default_rng(7)
cs = [2.09, 0.57]
data = [N.make_data(basis, c, 0.004, 0.005, rng) for c in cs]
worst = 0.0
for variant in ("cur", "F1"):
    files = []
    for e, (c, b) in enumerate(zip(cs, data)):
        f = Path(bdir) / ("val_b%d.bin" % e)
        (b / c).astype("<f8").tofile(f)
        files.append(str(f))
    ols = [1.5, 1.5] if variant == "cur" else [1.5 / c for c in cs]
    out = Path(bdir) / "val_fit.json"
    subprocess.run([gfb, "fit", bdir, "r60000,r60000b", ",".join(files),
                    ",".join("%.10g" % o for o in ols), str(out)],
                   check=True, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    real = json.loads(out.read_text())
    xr = np.zeros(len(basis.s))
    for s, k, c in real["solutes"]:
        jj = np.where((np.abs(basis.s - s) < 1e-6) & (np.abs(basis.k - k) < 1e-9))[0]
        xr[jj[0]] = c
    xp, _, masks = N.fit_variant(variant, list(zip([basis, basis], data)), cs, [1.5, 1.5])
    excl = [int((~m).sum()) for m in masks]
    diff = np.abs(xr - xp).max()
    worst = max(worst, diff)
    print("%-4s excluded points %s  sum x real %.6f replica %.6f  max|dx| %.2e"
          % (variant, excl, xr.sum(), xp.sum(), diff))
print("PASS" if worst < 1e-6 else "FAIL", "max |dx| %.2e" % worst)
