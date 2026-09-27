"""Summarize us_2dsa_bench results: accuracy, speed, resources per configuration.

usage: python3 analyze.py single|iterated|arrival
"""
import collections, glob, json, os, sys
import numpy as np
from scipy.stats import wasserstein_distance

HERE = os.environ.get('BENCH_DIR', os.getcwd())
sys.path.insert(0, HERE)
TRUTH = {
    'M1': [(3.3, 1.25, 0.4), (6.1, 1.55, 0.6)],
    'M2': [(2.2, 1.4, 0.3), (4.7, 1.3, 0.4), (8.4, 2.6, 0.3)],
    'M3': [(2.5, 1.2, 0.5), (5.5, 3.5, 0.5)],
    'H1': [(4.3, 1.3, 0.95), (6.4, 1.45, 0.05)],
    'H2': [(2.0 + 6.0 * i / 11, 1.2 + 1.6 * i / 11, 1 / 12) for i in range(12)],
    'H3': [(4.0, 1.2, 0.5), (4.0, 2.5, 0.5)],
    'A1': [(6.5, 1.5, 0.90), (9.5, 1.6, 0.07), (3.5, 1.4, 0.03)],
}
ORDER = ['modulo', 'classic8x8', 'ugrid8', 'rect4x16', 'lattice', 'g128x32_8x8',
         'g128x32_lattice', 'fem4096', 'fem1024']


def species_errors(sol, truth):
    """Assign fitted solutes to the nearest true species (s/9, f/f0/3 scaled);
    return per-species (dc, ds, dk) errors of concentration-weighted means."""
    sol = np.array(sol) if len(sol) else np.zeros((0, 3))
    tr = np.array(truth)
    d = ((sol[:, None, 0] - tr[None, :, 0]) / 9) ** 2 + \
        ((sol[:, None, 1] - tr[None, :, 1]) / 3) ** 2
    lab = d.argmin(1)
    out = []
    for j, (s, k, c) in enumerate(truth):
        m = lab == j
        cj = sol[m, 2].sum()
        if cj > 0:
            out.append((cj - c, (sol[m, 2] * sol[m, 0]).sum() / cj - s,
                        (sol[m, 2] * sol[m, 1]).sum() / cj - k))
        else:
            out.append((-c, np.nan, np.nan))
    return out


def dist_errors(sol, truth):
    sol = np.array(sol); tr = np.array(truth)
    ws = wasserstein_distance(sol[:, 0], tr[:, 0], sol[:, 2], tr[:, 2])
    wk = wasserstein_distance(sol[:, 1], tr[:, 1], sol[:, 2], tr[:, 2])
    return ws, wk


def load(subset):
    rows = []
    for fn in sorted(glob.glob(os.path.join(HERE, 'out', subset, '*.json'))):
        j = json.load(open(fn))
        base = os.path.basename(fn)[:-5].split('__')
        j['config'], j['mix'], j['seedn'] = base[0], base[1], int(base[2])
        j['tag'] = base[3] if len(base) > 3 else ''
        rows.append(j)
    return rows


def enrich(r, ref):
    truth = TRUTH[r['mix']]
    sol = [s[:3] for s in r['solutes']]
    r['ws'], r['wk'] = dist_errors(sol, truth)
    se = species_errors(sol, truth)
    r['sp_dc'] = max(abs(e[0]) for e in se)
    r['sp_ds'] = np.nanmax([abs(e[1]) for e in se])
    r['sp_dk'] = np.nanmax([abs(e[2]) for e in se])
    rf = ref.get(f"{r['mix']}__{r['seedn']}")
    r['excess'] = r['rmsd'] / rf['rmsd'] - 1 if rf else np.nan
    t = r['tasks']
    r['ntasks'] = len(t)
    r['maxdepth'] = max(x[1] for x in t)
    r['merge_in_max'] = max([x[4] for x in t if x[1] > 0 or x[3]] or [0])
    r['surv0'] = int(np.sum([x[5] for x in t if x[1] == 0 and not x[3] and x[0] == 0]))
    r['nsol'] = len(sol)
    return r


def table(rows, keys, fmt):
    by = collections.defaultdict(list)
    for r in rows:
        by[r['config']].append(r)
    names = [n for n in ORDER if n in by] + sorted(set(by) - set(ORDER))
    hdr = '| config | n | ' + ' | '.join(keys) + ' |'
    print(hdr); print('|' + '---|' * (len(keys) + 2))
    for n in names:
        vals = []
        for k, f in zip(keys, fmt):
            v = np.array([r[k] for r in by[n]], float)
            vals.append(f.format(np.nanmedian(v)) + (f' ({f.format(np.nanmax(v))})'
                                                    if k in ('excess', 'signal_err_rmsd', 'ws', 'wk', 'sp_dc') else ''))
        print(f'| {n} | {len(by[n])} | ' + ' | '.join(vals) + ' |')


def per_mix(rows, key, fmt):
    by = collections.defaultdict(lambda: collections.defaultdict(list))
    for r in rows:
        by[r['config']][r['mix']].append(r[key])
    mixes = list(TRUTH)
    names = [n for n in ORDER if n in by]
    print('| config | ' + ' | '.join(mixes) + ' |'); print('|' + '---|' * (len(mixes) + 1))
    for n in names:
        print(f'| {n} | ' + ' | '.join(fmt.format(np.mean(by[n][m])) if by[n][m] else '-'
                                        for m in mixes) + ' |')


def paired(rows, base, key):
    """Per dataset, compare each configuration with a base configuration."""
    by = collections.defaultdict(dict)
    for r in rows:
        by[r['config']][(r['mix'], r['seedn'], r['tag'])] = r[key]
    if base not in by:
        return
    print(f'| config | datasets | better than {base} | median ratio | worst ratio |')
    print('|---|---|---|---|---|')
    for n in [n for n in ORDER if n in by and n != base]:
        ks = [k for k in by[n] if k in by[base]]
        rat = np.array([by[n][k] / by[base][k] for k in ks])
        print(f'| {n} | {len(ks)} | {np.sum(rat < 1)} | {np.median(rat):.2f} | '
              f'{rat.max():.2f} |')


def main():
    subset = sys.argv[1]
    ref = {}
    rp = os.path.join(HERE, 'out', 'fullgrid_ref.json')
    if os.path.exists(rp):
        ref = json.load(open(rp))
    rows = [enrich(r, ref) for r in load(subset)]
    if ref:
        v = np.array([x['rmsd'] for x in ref.values()])
        e = np.array([x['signal_err_rmsd'] for x in ref.values()])
        print(f'full-grid optimum: rmsd median {np.median(v):.6f}, '
              f'signal err median {np.median(e):.6f} (max {e.max():.6f})\n')
    print('### Accuracy (median over datasets; max in parentheses)\n')
    table(rows, ['rmsd', 'excess', 'signal_err_rmsd', 'ws', 'wk', 'sp_dc', 'nsol'],
          ['{:.5f}', '{:.1%}', '{:.5f}', '{:.3f}', '{:.3f}', '{:.3f}', '{:.0f}'])
    print('\n### Speed and resources (median)\n')
    table(rows, ['wall_s', 'cpu_user_s', 'rss_peak_kb', 'simulations', 'ntasks',
                 'maxdepth', 'merge_in_max', 'surv0', 'iterations'],
          ['{:.1f}', '{:.1f}', '{:.0f}', '{:.0f}', '{:.0f}', '{:.0f}', '{:.0f}',
           '{:.0f}', '{:.0f}'])
    print('\n### Excess RMSD over the full-grid optimum, mean per mixture\n')
    per_mix(rows, 'excess', '{:.1%}')
    print('\n### Fitted-signal error (RMSD vs noise-free truth), mean per mixture\n')
    per_mix(rows, 'signal_err_rmsd', '{:.5f}')
    print('\n### Paired with classic8x8: fitted-signal error\n')
    paired(rows, 'classic8x8', 'signal_err_rmsd')
    print('\n### Paired with classic8x8: wall time\n')
    paired(rows, 'classic8x8', 'wall_s')


if __name__ == '__main__':
    main()
