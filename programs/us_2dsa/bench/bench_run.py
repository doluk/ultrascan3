"""Run the us_2dsa_bench fit matrix sequentially (one fit at a time, 4 threads)
on the us_astfem_sim datasets in ~/ultrascan/results/bench<mix>s<seed>.

usage: python3 bench_run.py single [configs]|iterated [configs]|arrival|mergepool [N]|gen
BENCH_SET_SUFFIX appends a suffix to the result set directory (e.g. for
another us_2dsa_bench build given by US_2DSA_BENCH).
Results: out/<set>/<config>__<mixture>__<seed>[__rep].json
"""
import os, subprocess, sys, itertools, json

HERE = os.environ.get('BENCH_DIR', os.getcwd())
BIN = os.environ.get('US_2DSA_BENCH', os.path.normpath(os.path.join(
    os.path.dirname(os.path.abspath(__file__)), '../../../build/bin/us_2dsa_bench')))
ENV = dict(os.environ, LD_LIBRARY_PATH=os.path.join(
    os.path.dirname(os.path.dirname(BIN)), 'lib'))
MIXTURES = ['M1', 'M2', 'M3', 'H1', 'H2', 'H3', 'A1']
SEEDS = [1, 2, 3]
THREADS = 4


def configs():
    fg = json.load(open(os.path.join(HERE, 'fem_grids.json')))
    a, b, c = fg['stage2_exact_best']['hnf']
    ya, yb, yc = fg['g128x32_best']['hnf']
    return {
        'modulo': ('grids/modulo64.csv', 64),
        'classic8x8': ('grids/classic8x8.csv', 64),
        'ugrid8': ('ugrid:64:8', 64),
        'rect4x16': ('grids/rect4x16.csv', 64),
        'lattice': (f'grids/lattice_{a}_{b}_{c}.csv', 64),
        'g128x32_8x8': ('grids/g128x32_8x8.csv', 64),
        'g128x32_lattice': (f'grids/g128x32_lattice_{ya}_{yb}_{yc}.csv', 64),
        'fem4096': ('grids/fem4096_ifp.csv', 64),
        'fem1024': ('grids/fem1024_ifp.csv', 64),
    }


RESULTS = os.environ.get('BENCH_RESULTS', os.path.expanduser('~/ultrascan/results'))
SUFFIX = os.environ.get('BENCH_SET_SUFFIX', '')     # e.g. '_nnlsfix'


def run(outdir, name, grid, nsub, mix, seed, iters, env=ENV, tag=''):
    os.makedirs(outdir, exist_ok=True)
    out = os.path.join(outdir, f'{name}__{mix}__{seed}{tag}.json')
    if os.path.exists(out):
        return
    rundir = os.path.join(RESULTS, f'bench{mix}s{seed}')
    truedir = os.path.join(RESULTS, f'bench{mix}clean')
    cmd = [BIN, 'fit', rundir, truedir, grid, str(nsub), str(iters),
           str(THREADS), out]
    r = subprocess.run(cmd, cwd=HERE, env=env, capture_output=True, text=True)
    line = [l for l in r.stderr.splitlines() if l.startswith('fit ')]
    print(name, mix, seed, tag, line[-1] if line else r.stderr[-500:], flush=True)


def main():
    which = sys.argv[1]
    cf = configs()
    if which == 'single':
        names = sys.argv[2].split(',') if len(sys.argv) > 2 else list(cf)
        for mix, seed, name in itertools.product(MIXTURES, SEEDS, names):
            grid, nsub = cf[name]
            run(os.path.join(HERE, 'out', 'single' + SUFFIX), name, grid, nsub, mix, seed, 1)
    elif which == 'iterated':
        names = sys.argv[2].split(',') if len(sys.argv) > 2 else \
            ['modulo', 'classic8x8', 'lattice', 'fem4096']
        for mix, seed, name in itertools.product(MIXTURES, SEEDS, names):
            grid, nsub = cf[name]
            run(os.path.join(HERE, 'out', 'iterated' + SUFFIX), name, grid, nsub, mix, seed, 10)
    elif which == 'arrival':
        arr = dict(ENV, BENCH_ARRIVAL='1')
        for name in ['classic8x8', 'lattice']:
            grid, nsub = cf[name]
            for rep in range(6):
                run(os.path.join(HERE, 'out', 'arrival' + SUFFIX), name, grid, nsub,
                    'M1', 1, 1, env=arr, tag=f'__arr{rep}')
            for rep in range(3):
                run(os.path.join(HERE, 'out', 'arrival' + SUFFIX), name, grid, nsub,
                    'M1', 1, 1, tag=f'__ord{rep}')
    elif which == 'gen':
        # Partition cases of gen_cases.py (single pass); duplicates of an
        # earlier configuration of the same case are not fitted again
        gen = json.load(open(os.path.join(HERE, 'gen_cases.json')))['configs']
        for c in gen:
            if c['same_as']:
                continue
            for mix, seed in itertools.product(MIXTURES, SEEDS):
                run(os.path.join(HERE, 'out', 'gen' + SUFFIX), c['name'], c['grid'],
                    c['nsub'], mix, seed, 1)
    elif which == 'mergepool':
        # Single pass with a larger merge pool (2DSA-MergePool debug setting)
        pool = int(sys.argv[2]) if len(sys.argv) > 2 else 512
        env = dict(ENV, BENCH_DEBUG=f'2DSA-MergePool={pool}')
        for mix, seed, name in itertools.product(
                MIXTURES, SEEDS, ['classic8x8', 'lattice', 'fem4096', 'fem1024']):
            grid, nsub = cf[name]
            run(os.path.join(HERE, 'out', f'pool{pool}' + SUFFIX), name, grid, nsub,
                mix, seed, 1, env=env)


if __name__ == '__main__':
    main()
