"""Exact full-grid NNLS optimum (64 x 64 grid, TI+RI noise projected out) for
each benchmark dataset, from the same ASTFEM columns and data as the fits.

Writes out/fullgrid_ref.json: {"<mix>__<seed>": {rmsd, signal_err_rmsd, solutes}}
"""
import json, os, subprocess, time
import numpy as np
from scipy.optimize import nnls

HERE = os.environ.get('BENCH_DIR', os.getcwd())
BIN = os.environ.get('US_2DSA_BENCH', os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '../../../build/bin/us_2dsa_bench')))
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.join(
    os.path.dirname(os.path.dirname(BIN)), 'lib'))
NS, NP, N127 = 60, 384, 127
MIXTURES = ['M1', 'M2', 'M3', 'H1', 'H2', 'H3', 'A1']
SEEDS = [1, 2, 3]


def proj(a):
    a = a - a.mean(-2, keepdims=True)
    return a - a.mean(-1, keepdims=True)


def dump(run):
    fn = os.path.join(HERE, 'out', 'data', run + '.f32')
    if not os.path.exists(fn):
        os.makedirs(os.path.dirname(fn), exist_ok=True)
        subprocess.run([BIN, 'dump', os.path.join(os.environ.get('BENCH_RESULTS', os.path.expanduser('~/ultrascan/results')), run),
                        fn], env=ENV, capture_output=True, check=True)
    return np.fromfile(fn, dtype=np.float32).astype(np.float64).reshape(NS, NP)


def main():
    cols = np.memmap(os.path.join(HERE, 'cols', 'proxy127.f32'), dtype=np.float32,
                     mode='r', shape=(N127 * N127, NS, NP))
    px = np.tile(np.arange(N127), N127); py = np.repeat(np.arange(N127), N127)
    gidx = np.where((px % 2 == 0) & (py % 2 == 0))[0]
    A = proj(np.asarray(cols[gidx], dtype=np.float64)).reshape(4096, -1).T.copy()
    S = np.linspace(1, 10, 64); K = np.linspace(1, 4, 64)
    sv = np.tile(S, 64); kv = np.repeat(K, 64)
    res = {}
    for mix in MIXTURES:
        for seed in SEEDS:
            d = [dump(f'bench{mix}s{seed}'), dump(f'bench{mix}clean')]
            b = proj(d[0]).ravel(); sig = proj(d[1]).ravel()
            t = time.time()
            c, rn = nnls(A, b, maxiter=50000)
            el = time.time() - t
            fit = A @ c
            nz = np.nonzero(c)[0]
            res[f'{mix}__{seed}'] = {
                'rmsd': float(rn / np.sqrt(b.size)),
                'signal_err_rmsd': float(np.linalg.norm(fit - sig) / np.sqrt(b.size)),
                'noise_only_rmsd': float(np.linalg.norm(b - sig) / np.sqrt(b.size)),
                'nnls_s': el,
                'solutes': [[float(sv[j]), float(kv[j]), float(c[j])] for j in nz]}
            print(mix, seed, {k: round(v, 6) for k, v in res[f'{mix}__{seed}'].items()
                              if k != 'solutes'}, len(nz), flush=True)
    with open(os.path.join(HERE, 'out', 'fullgrid_ref.json'), 'w') as f:
        json.dump(res, f)


if __name__ == '__main__':
    main()
