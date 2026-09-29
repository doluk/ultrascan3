"""Exact full-grid NNLS optimum (TI+RI noise projected out) of the fine grids of
gen_cases.py, for each benchmark dataset:  out/gen_ref_<case>.json, keyed
"<mix>__<seed>" as out/fullgrid_ref.json (which serves the 64 x 64 case g64).

usage: python3 gen_ref.py [case ...]     (default: g100 g60 gU)
"""
import json, os, sys, time
import numpy as np
from scipy.optimize import nnls
from fullgrid_ref import proj, dump, MIXTURES, SEEDS, NS, NP, N127, HERE


def columns(case):
    if case == 'gU':
        uidx = json.load(open(os.path.join(HERE, 'gen_cases.json')))['union_proxy_indices']
        cols = np.memmap(os.path.join(HERE, 'cols', 'proxy127.f32'), dtype=np.float32,
                         mode='r', shape=(N127 * N127, NS, NP))
        a = np.asarray(cols[np.array(uidx)], dtype=np.float64)
    else:
        n = {'g100': 10000, 'g60': 3600}[case]
        a = np.fromfile(os.path.join(HERE, 'cols', case + '.f32'),
                        dtype=np.float32).reshape(n, NS, NP).astype(np.float64)
    return proj(a).reshape(len(a), -1).T.copy()


def main():
    for case in sys.argv[1:] or ['g100', 'g60', 'gU']:
        fn = os.path.join(HERE, 'out', f'gen_ref_{case}.json')
        res = json.load(open(fn)) if os.path.exists(fn) else {}
        A = columns(case)
        for mix in MIXTURES:
            for seed in SEEDS:
                key = f'{mix}__{seed}'
                if key in res:
                    continue
                d = [dump(f'bench{mix}s{seed}'), dump(f'bench{mix}clean')]
                b = proj(d[0]).ravel(); sig = proj(d[1]).ravel()
                t = time.time()
                c, rn = nnls(A, b, maxiter=50000)
                fit = A @ c
                res[key] = {'rmsd': float(rn / np.sqrt(b.size)),
                            'signal_err_rmsd': float(np.linalg.norm(fit - sig) / np.sqrt(b.size)),
                            'nnls_s': time.time() - t,
                            'nonzero': int(np.count_nonzero(c))}
                print(case, key, round(res[key]['rmsd'], 7), round(res[key]['nnls_s'], 1),
                      flush=True)
                with open(fn, 'w') as f:            # after each dataset:  resumable
                    json.dump(res, f)
        del A


if __name__ == '__main__':
    main()
