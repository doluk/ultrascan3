"""Summarize the partition cases of gen_cases.py (single pass, both solvers):
per configuration the median excess RMSD over the exact optimum of its own
fine grid, fitted-signal error, distribution errors, cost, and paired
comparisons with the modulo rule and with the index-unit sublattice.

usage: python3 gen_analyze.py [out.json]     (prints markdown tables)
"""
import collections, glob, json, os, sys
import numpy as np
import analyze

HERE = analyze.HERE
CASES = [('g100', '100 × 100, 101 subgrids'), ('g64', '64 × 64, 41 subgrids'),
         ('g60', '60 × 60, 60 subgrids'), ('gU', 'union of partial grids, 32 subgrids')]


def refs():
    r = {'g64': json.load(open(os.path.join(HERE, 'out', 'fullgrid_ref.json')))}
    for c in ('g100', 'g60', 'gU'):
        fn = os.path.join(HERE, 'out', f'gen_ref_{c}.json')
        if os.path.exists(fn):
            r[c] = json.load(open(fn))
    return r


def main():
    gen = json.load(open(os.path.join(HERE, 'gen_cases.json')))['configs']
    ref = refs()
    summary = {}
    for subset, solver in [('gen', 'production'), ('gen_exact', 'corrected')]:
        rows = collections.defaultdict(dict)
        for r in analyze.load(subset):
            case = r['config'].split('_')[1]
            if case not in ref:
                continue
            rows[r['config']][(r['mix'], r['seedn'])] = analyze.enrich(r, ref[case])
        for case, title in CASES:
            cfgs = [c for c in gen if c['case'] == case]
            print(f'\n### {title}, {solver} solver\n')
            print('| configuration | method | FEM worst h | n | excess | (max) | signal err '
                  '(1e-3) | W_s | W_f/f0 | vs modulo | vs index | time (s) | MB |')
            print('|---|---|---|---|---|---|---|---|---|---|---|---|---|')
            for c in cfgs:
                name = c['same_as'] or c['name']
                d = rows.get(name, {})
                if not d:
                    continue
                def med(k):
                    return float(np.median([v[k] for v in d.values()]))
                def paired(other):
                    o = rows.get(other, {})
                    ks = [k for k in d if k in o]
                    if not ks or other == name:
                        return None
                    rat = np.array([d[k]['signal_err_rmsd'] / o[k]['signal_err_rmsd'] for k in ks])
                    return [int(np.sum(rat < 1)), len(ks), float(np.median(rat))]
                ex = [v['excess'] for v in d.values()]
                s = {'n': len(d), 'excess': float(np.median(ex)), 'excess_max': float(np.max(ex)),
                     'signal_err': med('signal_err_rmsd'), 'ws': med('ws'), 'wk': med('wk'),
                     'sp_dc': med('sp_dc'), 'wall_s': med('wall_s'),
                     'rss_mb': med('rss_peak_kb') / 1024, 'simulations': med('simulations'),
                     'vs_modulo': paired(f'gen_{case}_modulo'),
                     'vs_index': paired(f'gen_{case}_index' if case != 'gU'
                                        else 'gen_gU_alg1_index'),
                     'fem_worst_h': c['fem_worst_h'], 'method': c['method']}
                summary[f'{solver}:{c["name"]}'] = s
                fmt = lambda p: '--' if p is None else f'{p[0]}/{p[1]} ({p[2]:.2f})'
                print(f'| {c["name"]} | {c["method"]} | {c["fem_worst_h"]:.1f} | {s["n"]} | '
                      f'{100 * s["excess"]:.1f}% | {100 * s["excess_max"]:.1f}% | '
                      f'{1e3 * s["signal_err"]:.2f} | {s["ws"]:.3f} | {s["wk"]:.3f} | '
                      f'{fmt(s["vs_modulo"])} | {fmt(s["vs_index"])} | {s["wall_s"]:.1f} | '
                      f'{s["rss_mb"]:.0f} |')
        for case, _ in CASES:
            if case in ref:
                v = [x['signal_err_rmsd'] for x in ref[case].values()]
                summary[f'optimum:{case}'] = {'signal_err': float(np.median(v)), 'n': len(v)}
    if len(sys.argv) > 1:
        json.dump(summary, open(sys.argv[1], 'w'), indent=1)


if __name__ == '__main__':
    main()
