"""Stage 2 and Stage 3 grids of the subgrid-coverage proposal, computed from
ASTFEM simulations (us_2dsa_bench columns) of the benchmark experiment.

Inputs (float32, one nscans*npoints column per solute, f/f0 outer loop):
  cols/proxy127.f32   127 x 127 candidate set / proxy of the parameter region
  cols/g128x32.f32    uniform 128 x 32 grid
Outputs: grids/*.csv (s, f/f0, subgrid label) and fem_grids.json (metrics).
Distances are the FEM metric of the proposal: Euclidean distance between
solute simulations with TI and RI components projected out.
"""
import json, os, time
import numpy as np
from scipy.interpolate import RegularGridInterpolator

NS, NP, N127 = 60, 384, 127
HERE = os.environ.get('BENCH_DIR', os.getcwd())


def load(name, n):
    a = np.fromfile(os.path.join(HERE, 'cols', name), dtype=np.float32)
    a = a.reshape(n, NS, NP).astype(np.float64)
    a -= a.mean(1, keepdims=True)                 # TI projection
    a -= a.mean(2, keepdims=True)                 # RI projection
    return a.reshape(n, NS * NP)


def divisors(n):
    return [d for d in range(1, n + 1) if n % d == 0]


def hnf_labels(x, y, a, b, c):
    return (y % c) * a + ((x - b * (y // c)) % a)


def hnfs(n):
    return [(a, b, n // a) for a in divisors(n) for b in range(a)]


def cover_stats(D2, lab, nsub, D2pts=None):
    """D2: squared distances proxy x points; lab: subgrid label per point.
    Returns worst and median subgrid covering radius, min separation."""
    if np.bincount(lab, minlength=nsub).min() == 0:
        return np.inf, np.inf, np.nan             # some subgrid would be empty
    order = np.argsort(lab, kind='stable')
    starts = np.searchsorted(lab[order], np.arange(nsub))
    mins = np.minimum.reduceat(D2[:, order], starts, axis=1)     # proxy x nsub
    h = np.sqrt(mins.max(0))
    sep = np.nan
    if D2pts is not None:
        sep = np.inf
        for i in range(nsub):
            m = np.where(lab == i)[0]
            dd = D2pts[np.ix_(m, m)].copy()
            np.fill_diagonal(dd, np.inf)
            sep = min(sep, dd.min())
        sep = float(np.sqrt(sep))
    return float(h.max()), float(np.median(h)), sep


def sqd(na, nb, G):
    return np.maximum(na[:, None] + nb[None, :] - 2 * G, 0.0)


def write_grid(name, s, k, lab):
    with open(os.path.join(HERE, 'grids', name + '.csv'), 'w') as f:
        for sv, kv, lv in zip(s, k, lab):
            f.write(f'{sv:.10g},{kv:.10g},{int(lv)}\n')


def main():
    out = {}
    t0 = time.time()
    X = load('proxy127.f32', N127 * N127)
    nX = (X * X).sum(1)
    G = X @ X.T
    print(f'  proxy Gram in {time.time() - t0:.0f} s', flush=True)
    px = np.tile(np.arange(N127), N127)
    py = np.repeat(np.arange(N127), N127)
    S127 = np.linspace(1, 10, N127)
    K127 = np.linspace(1, 4, N127)

    # 64 x 64 fine grid = every other proxy point, row-major (f/f0 outer)
    gidx = np.where((px % 2 == 0) & (py % 2 == 0))[0]
    gx, gy = px[gidx] // 2, py[gidx] // 2
    assert np.all(gy * 64 + gx == np.arange(4096))
    D2g = sqd(nX, nX[gidx], G[:, gidx])            # proxy x grid
    D2gg = D2g[gidx]                               # grid x grid
    out['uniform64_union_h'] = float(np.sqrt(D2g.min(1).max()))

    # ---- Stage 2: all sublattices of index 64, exact FEM metric
    t = time.time()
    cands = hnfs(64)
    if os.environ.get('STAGE2_CANDIDATES'):      # reuse an earlier full scan
        cands = [tuple(map(int, c.split(','))) for c in
                 os.environ['STAGE2_CANDIDATES'].split(';')]
    res = [((a, b, c),) + cover_stats(D2g, hnf_labels(gx, gy, a, b, c), 64, D2gg)
           for a, b, c in cands]
    res.sort(key=lambda r: (r[1], -r[3]))
    print(f'  {len(res)} sublattices in {time.time() - t:.0f} s; best {res[:3]}',
          flush=True)
    best = res[0][0]
    out['stage2_exact_best'] = {'hnf': best, 'worst_h': res[0][1],
                                'median_h': res[0][2], 'min_sep': res[0][3]}
    out['stage2_top5'] = [[list(r[0]), r[1]] for r in res[:5]]
    for nm, lab in [('modulo', np.arange(4096) % 64),
                    ('classic8x8', (gx % 8) + 8 * (gy % 8)),
                    ('rect4x16', (gx % 4) + 4 * (gy % 16))]:
        out['cover_' + nm] = cover_stats(D2g, lab, 64, D2gg)
    # best rectangle
    rects = sorted(((h, cover_stats(D2g, hnf_labels(gx, gy, *h), 64)[0])
                    for h in hnfs(64) if h[1] == 0), key=lambda r: r[1])
    out['best_rect'] = [list(rects[0][0]), rects[0][1]]

    # ---- Stage 2, approximate: diagonal local metric from 3x3 anchors
    anch = [0, 31, 63]
    ls = np.zeros((3, 3)); lk = np.zeros((3, 3))
    for i, ax in enumerate(anch):
        for j, ay in enumerate(anch):
            p = ay * 64 + ax
            qs = ay * 64 + (ax + 1 if ax < 63 else ax - 1)
            qk = (ay + 1 if ay < 63 else ay - 1) * 64 + ax
            ls[j, i] = np.sqrt(D2gg[p, qs]); lk[j, i] = np.sqrt(D2gg[p, qk])
    pts = np.c_[gy, gx]
    LS = np.exp(RegularGridInterpolator((anch, anch), np.log(ls))(pts))
    LK = np.exp(RegularGridInterpolator((anch, anch), np.log(lk))(pts))
    dx = (gx[:, None] - gx[None, :]) * 0.5 * (LS[:, None] + LS[None, :])
    dy = (gy[:, None] - gy[None, :]) * 0.5 * (LK[:, None] + LK[None, :])
    D2a = dx * dx + dy * dy                         # grid x grid, approximate
    del dx, dy
    ares = sorted(((h, cover_stats(D2a, hnf_labels(gx, gy, *h), 64)[0])
                   for h in cands), key=lambda r: r[1])
    out['stage2_anchor3x3_best'] = list(ares[0][0])
    print('  3x3-anchor choice', ares[0], '| exact choice', best, flush=True)
    del D2a
    write_grid(f'lattice_{best[0]}_{best[1]}_{best[2]}',
               S127[px[gidx]], K127[py[gidx]], hnf_labels(gx, gy, *best))
    if tuple(ares[0][0]) != tuple(best):
        a2 = ares[0][0]
        write_grid(f'lattice_{a2[0]}_{a2[1]}_{a2[2]}',
                   S127[px[gidx]], K127[py[gidx]], hnf_labels(gx, gy, *a2))

    # ---- Stage 2 on a re-proportioned uniform grid, 128 x 32
    Y = load('g128x32.f32', 4096)
    nY = (Y * Y).sum(1)
    D2y = sqd(nX, nY, X @ Y.T)
    D2yy = sqd(nY, nY, Y @ Y.T)
    del Y
    hx = np.tile(np.arange(128), 32); hy = np.repeat(np.arange(32), 128)
    yres = [((a, b, c),) + cover_stats(D2y, hnf_labels(hx, hy, a, b, c), 64)
            for a, b, c in hnfs(64)]
    yres.sort(key=lambda r: r[1])
    yb = yres[0][0]
    lab8 = (hx % 8) + 8 * (hy % 8)
    out['g128x32_best'] = {'hnf': yb, 'worst_h': yres[0][1],
                           'min_sep': cover_stats(D2y, hnf_labels(hx, hy, *yb), 64, D2yy)[2]}
    out['g128x32_8x8'] = cover_stats(D2y, lab8, 64, D2yy)
    out['g128x32_union_h'] = float(np.sqrt(D2y.min(1).max()))
    S2 = np.linspace(1, 10, 128); K2 = np.linspace(1, 4, 32)
    write_grid(f'g128x32_lattice_{yb[0]}_{yb[1]}_{yb[2]}', S2[hx], K2[hy],
               hnf_labels(hx, hy, *yb))
    write_grid('g128x32_8x8', S2[hx], K2[hy], lab8)
    print('  128x32 best', yres[0], '| 8x8 offsets', out['g128x32_8x8'], flush=True)
    del D2y, D2yy

    # ---- Stage 3: farthest-point placement + interleaved FP partition
    t = time.time()
    sel = [0]                                        # start at (1 S, f/f0 1)
    mind2 = np.maximum(nX + nX[0] - 2 * G[0], 0)
    while len(sel) < 4096:
        j = int(np.argmax(mind2)); sel.append(j)
        mind2 = np.minimum(mind2, np.maximum(nX + nX[j] - 2 * G[j], 0))
    sel = np.array(sel)
    print(f'  FPS 4096 in {time.time() - t:.0f} s', flush=True)
    for npts in (4096, 1024):
        p = sel[:npts]
        D2p = sqd(nX, nX[p], G[:, p])               # proxy x placed points
        D2pp = D2p[p]
        lab = -np.ones(npts, int)
        mind = np.full((64, npts), np.inf)
        glob = np.full(npts, np.inf)
        for step in range(npts):                   # round-robin farthest point
            i = step % 64
            score = mind[i] if np.isfinite(mind[i]).any() else glob
            j = int(np.argmax(np.where(lab < 0, score, -1.0)))
            lab[j] = i
            mind[i] = np.minimum(mind[i], D2pp[j])
            glob = np.minimum(glob, D2pp[j])
        cs = cover_stats(D2p, lab, 64, D2pp)
        out[f'fem{npts}'] = {'union_h': float(np.sqrt(D2p.min(1).max())),
                             'worst_h': cs[0], 'median_h': cs[1], 'min_sep': cs[2],
                             'distinct_s': int(len(np.unique(px[p]))),
                             'distinct_k': int(len(np.unique(py[p])))}
        write_grid(f'fem{npts}_ifp', S127[px[p]], K127[py[p]], lab)
        print(f'  FEM-placed {npts}:', out[f'fem{npts}'], flush=True)

    with open(os.path.join(HERE, 'fem_grids.json'), 'w') as f:
        json.dump(out, f, indent=1, default=float)
    print(f'done in {time.time() - t0:.0f} s')


if __name__ == '__main__':
    main()
