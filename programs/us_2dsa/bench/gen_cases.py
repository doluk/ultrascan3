"""Partition cases beyond 64 subgrids of a 64 x 64 grid, where the general
sublattice rule of the subgrid-coverage proposal (Hermite normal forms,
Algorithm 1) differs from the classic shifted grid:

  g100  100 x 100 grid, 101 subgrids (the odd-count override: modulo gives
        diagonals, no rectangle leaves every subgrid populated)
  g64   64 x 64 grid, 41 subgrids (prime; modulo generator 23 is benign)
  g60   60 x 60 grid, 60 subgrids (modulo gives columns)
  gU    union of two partial grids, 32 subgrids: a fine band (64 x 21,
        f/f0 1-1.95) stored first and a coarse band (22 x 15, f/f0 2-4)

Partitions:  the current modulo rule over the row-major component order; the
best sublattice in index units (Stage 1); the best sublattice in the FEM metric
(Stage 2); for g60 also the best rectangle in index units; for the union,
Algorithm 1 (sublattice per partial grid, rotated offsets, size balancing) in
index units and in the FEM metric, and the interleaved farthest-point partition.
All grids lie in s 1-10 S, f/f0 1-4, the region of the 127 x 127 proxy.

usage:  python3 gen_cases.py points   (solute lists for us_2dsa_bench columns)
        python3 gen_cases.py grids    (partitions, grids/gen_*.csv, gen_cases.json)
"""
import json, os, sys, time
import numpy as np

NS, NP, N127 = 60, 384, 127
HERE = os.environ.get('BENCH_DIR', os.getcwd())
S127 = np.linspace(1, 10, N127)
K127 = np.linspace(1, 4, N127)
PX = np.tile(np.arange(N127), N127)          # proxy column (s) index
PY = np.repeat(np.arange(N127), N127)        # proxy row (f/f0) index


def divisors(n):
    return [d for d in range(1, n + 1) if n % d == 0]


def hnfs(n):
    return [(a, b, n // a) for a in divisors(n) for b in range(a)]


def hnf_labels(x, y, a, b, c):
    return (y % c) * a + ((x - b * (y // c)) % a)


def rect_grid(ns, nk):
    """Row-major (f/f0 outer, s inner) uniform grid over s 1-10, f/f0 1-4."""
    x = np.tile(np.arange(ns), nk)
    y = np.repeat(np.arange(nk), ns)
    return x, y, np.linspace(1, 10, ns)[x], np.linspace(1, 4, nk)[y]


def union_grid():
    """Fine band first (f/f0 rows 0..40 of the proxy, every 2nd), then the
    coarse band (rows 42..126, every 6th); proxy indices, local coordinates."""
    parts = []
    for rows, step in [(range(0, 41, 2), 2), (range(42, 127, 6), 6)]:
        cols = range(0, 127, step)
        idx = np.array([r * N127 + c for r in rows for c in cols])
        gx = np.tile(np.arange(len(cols)), len(rows))
        gy = np.repeat(np.arange(len(rows)), len(cols))
        parts.append((idx, gx, gy))
    return parts


def load_cols(name, n):
    a = np.fromfile(os.path.join(HERE, 'cols', name), dtype=np.float32)
    a = a.reshape(n, NS, NP).astype(np.float64)
    a -= a.mean(1, keepdims=True)                 # TI projection
    a -= a.mean(2, keepdims=True)                 # RI projection
    return a.reshape(n, NS * NP)


def sqd(na, nb, G):
    """Squared distances from norms and the Gram block G (overwritten)."""
    G *= -2.0
    G += na[:, None]
    G += nb[None, :]
    return np.maximum(G, 0.0, out=G)


def cover(D2, lab, n):
    """Worst and median subgrid covering radius; D2 is omega x points."""
    if np.bincount(lab, minlength=n).min() == 0:
        return np.inf, np.inf
    order = np.argsort(lab, kind='stable')
    starts = np.searchsorted(lab[order], np.arange(n))
    h = np.sqrt(np.minimum.reduceat(D2[:, order], starts, axis=1).max(0))
    return float(h.max()), float(np.median(h))


def index_d2(x, y):
    xf, yf = x.astype(np.float32), y.astype(np.float32)
    return np.subtract.outer(xf, xf) ** 2 + np.subtract.outer(yf, yf) ** 2


def best_hnf(x, y, n, D2, only_rect=False):
    res = []
    for h in hnfs(n):
        if only_rect and h[1] != 0:
            continue
        w, m = cover(D2, hnf_labels(x, y, *h), n)
        if np.isfinite(w):
            res.append((w, m, h))
    res.sort()
    return res[0]


def balance(lab, n, dist):
    """Sizes as the k mod n decoding requires (the first N mod n subgrids one
    point larger), keeping the labels:  repeatedly move, from the subgrid most
    over its size to the one most under, the point that lies farthest from the
    receiving subgrid (Algorithm 1, lines 9-11)."""
    lab = lab.copy()
    N = len(lab)
    need = np.array([(N - i + n - 1) // n for i in range(n)])
    sizes = np.bincount(lab, minlength=n)
    moves = 0
    while True:
        over = np.where(sizes > need)[0]
        if len(over) == 0:
            return lab, moves
        under = np.where(sizes < need)[0]
        i = over[np.argmax(sizes[over] - need[over])]
        j = under[np.argmax(need[under] - sizes[under])]
        pi, pj = np.where(lab == i)[0], np.where(lab == j)[0]
        p = pi[np.argmax(dist(pi, pj).min(1))] if len(pj) else pi[0]
        lab[p] = j
        sizes[i] -= 1
        sizes[j] += 1
        moves += 1


def write_grid(name, s, k, lab):
    with open(os.path.join(HERE, 'grids', name + '.csv'), 'w') as f:
        for sv, kv, lv in zip(s, k, lab):
            f.write(f'{sv:.10g},{kv:.10g},{int(lv)}\n')


def points():
    os.makedirs(os.path.join(HERE, 'cols'), exist_ok=True)
    for name, ns, nk in [('g100', 100, 100), ('g60', 60, 60)]:
        _, _, s, k = rect_grid(ns, nk)
        with open(os.path.join(HERE, 'cols', name + '.csv'), 'w') as f:
            for sv, kv in zip(s, k):
                f.write(f'{sv:.10g},{kv:.10g}\n')


def grids():
    t0 = time.time()
    out = {'configs': []}
    X = load_cols('proxy127.f32', N127 * N127)
    nX = (X * X).sum(1)
    print(f'proxy loaded in {time.time() - t0:.0f} s', flush=True)

    def add(case, name, s, k, lab, n, D2fem, method):
        lab = np.asarray(lab)
        w, m = cover(D2fem, lab, n)
        dup = next((c['name'] for c in out['configs'] if c['case'] == case
                    and c['labels_hash'] == hash(lab.tobytes())), None)
        cfg = {'case': case, 'name': name, 'nsub': n, 'method': method,
               'fem_worst_h': w, 'fem_median_h': m,
               'union_h': float(np.sqrt(D2fem.min(1).max())),
               'labels_hash': hash(lab.tobytes()), 'same_as': dup,
               'grid': f'grids/{name}.csv'}
        out['configs'].append(cfg)
        if dup is None:
            write_grid(name, s, k, lab)
        print(f'  {name:22s} {method:40s} FEM worst h {w:6.2f} median {m:6.2f}'
              + (f'  (same as {dup})' if dup else ''), flush=True)

    # ---- rectangular grids (GEN_CASES:  comma-separated subset, for tests)
    only = os.environ.get('GEN_CASES', 'g100,g64,g60,gU').split(',')
    for case, ns, nk, n, colsrc in [('g100', 100, 100, 101, 'g100.f32'),
                                    ('g64', 64, 64, 41, None),
                                    ('g60', 60, 60, 60, 'g60.f32')]:
        if case not in only:
            continue
        t = time.time()
        x, y, s, k = rect_grid(ns, nk)
        if colsrc:
            Y = load_cols(colsrc, ns * nk)
        else:                                       # 64 x 64 = every other proxy point
            gidx = np.where((PX % 2 == 0) & (PY % 2 == 0))[0]
            Y = X[gidx]
        nY = (Y * Y).sum(1)
        D2f = sqd(nX, nY, X @ Y.T)                  # proxy x grid, FEM metric
        D2i = index_d2(x, y)                        # grid x grid, index units

        def fdist(pi, pj):
            return np.sqrt(sqd(nY[pi], nY[pj], Y[pi] @ Y[pj].T))

        def idist(pi, pj):
            return np.sqrt(np.subtract.outer(x[pi], x[pj]) ** 2.0
                           + np.subtract.outer(y[pi], y[pj]) ** 2.0)

        add(case, f'gen_{case}_modulo', s, k, np.arange(ns * nk) % n, n, D2f,
            f'modulo (g = {ns % n})')
        wi, mi, hi = best_hnf(x, y, n, D2i)
        lab, mv = balance(hnf_labels(x, y, *hi), n, idist)
        add(case, f'gen_{case}_index', s, k, lab, n, D2f,
            f'index-unit sublattice {hi}, {mv} moves')
        if case == 'g60':
            wr, mr, hr = best_hnf(x, y, n, D2i, only_rect=True)
            lab, mv = balance(hnf_labels(x, y, *hr), n, idist)
            add(case, f'gen_{case}_rect', s, k, lab, n, D2f,
                f'index-unit rectangle {hr}, {mv} moves')
        wf, mf, hf = best_hnf(x, y, n, D2f)
        lab, mv = balance(hnf_labels(x, y, *hf), n, fdist)
        add(case, f'gen_{case}_fem', s, k, lab, n, D2f,
            f'FEM sublattice {hf}, {mv} moves')
        del D2f, D2i, Y
        print(f'{case} done in {time.time() - t:.0f} s', flush=True)

    # ---- union of two partial grids, 32 subgrids
    if 'gU' not in only:
        return
    t = time.time()
    n = 32
    parts = union_grid()
    uidx = np.concatenate([p[0] for p in parts])
    s, k = S127[PX[uidx]], K127[PY[uidx]]
    Y = X[uidx]
    nY = (Y * Y).sum(1)
    D2f = sqd(nX, nY, X @ Y.T)                      # full proxy x union
    N = len(uidx)
    part_of = np.concatenate([np.full(len(p[0]), i) for i, p in enumerate(parts)])
    xs = (s - 1) / 9.0
    ks = (k - 1) / 3.0

    def fdist(pi, pj):
        return np.sqrt(sqd(nY[pi], nY[pj], Y[pi] @ Y[pj].T))

    def pdist(pi, pj):                             # normalized parameter units
        return np.sqrt(np.subtract.outer(xs[pi], xs[pj]) ** 2
                       + np.subtract.outer(ks[pi], ks[pj]) ** 2)

    add('gU', 'gen_gU_modulo', s, k, np.arange(N) % n, n, D2f,
        'modulo over the stored order')
    for metric in ('index', 'fem'):
        lab = np.zeros(N, int)
        o, off = 0, 0
        chosen = []
        for pi, (idx, gx, gy) in enumerate(parts):
            sl = slice(off, off + len(idx))
            if metric == 'index':
                D2 = index_d2(gx, gy)
            else:                                   # proxy rows of this band
                rows = np.unique(PY[idx])
                om = np.where((PY >= rows.min()) & (PY <= rows.max()))[0]
                D2 = D2f[om][:, sl]
            w, m, h = best_hnf(gx, gy, n, D2)
            lab[sl] = (hnf_labels(gx, gy, *h) + o) % n
            o = (o + len(idx)) % n
            off += len(idx)
            chosen.append(h)
        lab, mv = balance(lab, n, fdist if metric == 'fem' else pdist)
        add('gU', f'gen_gU_alg1_{metric}', s, k, lab, n, D2f,
            f'Algorithm 1 ({metric}) {chosen}, {mv} moves')
    # interleaved farthest-point partition in the FEM metric
    D2uu = sqd(nY, nY, Y @ Y.T)
    lab = -np.ones(N, int)
    mind = np.full((n, N), np.inf)
    glob = np.full(N, np.inf)
    for step in range(N):
        i = step % n
        score = mind[i] if np.isfinite(mind[i]).any() else glob
        j = int(np.argmax(np.where(lab < 0, score, -1.0)))
        lab[j] = i
        mind[i] = np.minimum(mind[i], D2uu[j])
        glob = np.minimum(glob, D2uu[j])
    add('gU', 'gen_gU_ifp', s, k, lab, n, D2f, 'interleaved farthest-point (FEM)')
    out['union_proxy_indices'] = uidx.tolist()
    print(f'gU done in {time.time() - t:.0f} s', flush=True)

    for c in out['configs']:
        del c['labels_hash']
    with open(os.path.join(HERE, 'gen_cases.json'), 'w') as f:
        json.dump(out, f, indent=1)
    print(f'all done in {time.time() - t0:.0f} s')


if __name__ == '__main__':
    {'points': points, 'grids': grids}[sys.argv[1]]()
