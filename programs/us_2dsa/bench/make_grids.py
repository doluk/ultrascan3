"""Write 64x64 custom grids (s 1-10 S, f/f0 1-4) with subgrid labels.
Row-major component order (f/f0 outer, s inner), as the grid editor stores it."""
import numpy as np, sys, os
S = np.linspace(1, 10, 64); K = np.linspace(1, 4, 64)
x = np.tile(np.arange(64), 64); y = np.repeat(np.arange(64), 64)

def write(name, lab, sv=None, kv=None):
    sv = S[x] if sv is None else sv; kv = K[y] if kv is None else kv
    with open(os.path.join(os.environ.get('BENCH_DIR', os.getcwd()), 'grids', name + '.csv'), 'w') as f:
        for s, k, l in zip(sv, kv, lab):
            f.write(f"{s:.10g},{k:.10g},{int(l)}\n")

def hnf(a, b, c):
    return (y % c) * a + ((x - b * (y // c)) % a)

if __name__ == '__main__':
    write('modulo64', np.arange(4096) % 64)
    write('classic8x8', (x % 8) + 8 * (y % 8))
    write('rect4x16', (x % 4) + 4 * (y % 16))
    write('full1', np.zeros(4096))
    for arg in sys.argv[1:]:          # extra lattices "a,b,c"
        a, b, c = map(int, arg.split(','))
        write(f'lattice_{a}_{b}_{c}', hnf(a, b, c))
