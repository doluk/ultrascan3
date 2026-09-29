"""Figures of the benchmark report (subgrid_benchmark.typ) from the result
files of bench_run.py and defects/defect_scan.py.

usage: BENCH_DIR=<benchmark directory> python3 make_figures.py [outdir]
"""
import collections, glob, json, os, sys
import numpy as np
import matplotlib
matplotlib.use('Agg')
import matplotlib.pyplot as plt

HERE = os.environ.get('BENCH_DIR', os.getcwd())
OUT = sys.argv[1] if len(sys.argv) > 1 else os.path.dirname(os.path.abspath(__file__))

# Computer Modern, to match the report text; recessive ink for axes and grid
plt.rcParams.update({
    'font.family': 'cmr10', 'mathtext.fontset': 'cm',
    'axes.formatter.use_mathtext': True, 'axes.unicode_minus': False,
    'font.size': 8.5, 'axes.titlesize': 9, 'axes.labelsize': 8.5,
    'xtick.labelsize': 7.5, 'ytick.labelsize': 7.5,
    'axes.edgecolor': '#8a8984', 'axes.linewidth': 0.6,
    'xtick.color': '#52514e', 'ytick.color': '#52514e',
    'xtick.major.width': 0.6, 'ytick.major.width': 0.6,
    'axes.labelcolor': '#0b0b0b', 'text.color': '#0b0b0b',
    'axes.grid': True, 'grid.color': '#e4e3df', 'grid.linewidth': 0.5,
    'axes.spines.top': False, 'axes.spines.right': False,
    'svg.fonttype': 'path', 'figure.dpi': 150,
})
BLUE, ORANGE, AQUA = '#2a78d6', '#eb6834', '#1baf7a'
REF = '#52514e'          # reference series: secondary ink, not a category

ORDER = ['modulo', 'classic8x8', 'ugrid8', 'rect4x16', 'lattice', 'g128x32_8x8',
         'g128x32_lattice', 'fem4096', 'fem1024']
LABEL = {'modulo': 'modulo\n(current)', 'classic8x8': 'classic\n8$\\times$8',
         'ugrid8': 'regular\npath', 'rect4x16': 'rect.\n4$\\times$16',
         'lattice': 'lattice\n(8,3,8)', 'g128x32_8x8': '128$\\times$32\n8$\\times$8',
         'g128x32_lattice': '128$\\times$32\nlattice', 'fem4096': 'FEM\n4096',
         'fem1024': 'FEM\n1024'}


def load(subset):
    rows = collections.defaultdict(dict)
    for fn in glob.glob(os.path.join(HERE, 'out', subset, '*.json')):
        b = os.path.basename(fn)[:-5].split('__')
        rows[b[0]][(b[1], int(b[2]))] = json.load(open(fn))
    return rows


def fig_single():
    """Excess RMSD over the full-grid optimum, single pass, both solvers."""
    ref = json.load(open(os.path.join(HERE, 'out', 'fullgrid_ref.json')))
    sets = [('single', 'production solver', ORANGE, -0.17),
            ('single_exact', 'corrected solver (SolveSim-ExactNoise)', BLUE, 0.17)]
    fig, ax = plt.subplots(figsize=(6.3, 2.9))
    rng = np.random.default_rng(3)
    for subset, name, col, dx in sets:
        rows = load(subset)
        for i, cfg in enumerate(ORDER):
            ex = [100.0 * (r['rmsd'] / ref[f'{m}__{s}']['rmsd'] - 1.0)
                  for (m, s), r in rows.get(cfg, {}).items()]
            if not ex:
                continue
            x = i + dx + rng.uniform(-0.07, 0.07, len(ex))
            ax.scatter(x, ex, s=9, color=col, alpha=0.75, linewidths=0,
                       label=name if i == 0 else None, zorder=3)
            ax.plot([i + dx - 0.13, i + dx + 0.13], [np.median(ex)] * 2,
                    color='#0b0b0b', lw=1.2, zorder=4, solid_capstyle='round')
    ax.set_yscale('symlog', linthresh=1.0, linscale=0.6)
    ax.set_ylim(-1.5, 1500)
    ax.set_yticks([-1, 0, 1, 10, 100, 1000])
    ax.set_yticklabels(['$-1$', '0', '1', '10', '100', '1000'])
    ax.set_ylabel('excess RMSD over optimum (%)')
    ax.set_xticks(range(len(ORDER)))
    ax.set_xticklabels([LABEL[c] for c in ORDER])
    ax.set_xlim(-0.6, len(ORDER) - 0.4)
    ax.grid(axis='x', visible=False)
    ax.axhline(0, color='#8a8984', lw=0.6, zorder=1)
    ax.legend(loc='upper right', frameon=False, fontsize=7.5, handletextpad=0.2,
              markerscale=1.6)
    fig.tight_layout()
    fig.savefig(os.path.join(OUT, 'fig_single.svg'))


def fig_defects():
    """Controlled defect scan: fitted-signal error and misplaced concentration."""
    rows = json.load(open(os.path.join(HERE, 'defects', 'defect_scan.json')))
    ris = [0.0, 0.002, 0.005, 0.02]
    series = [('exact', 'exact', REF), ('D2 only', 'D2 only', AQUA),
              ('D1 only', 'D1 only', ORANGE), ('both (US)', 'D1 + D2 (production)', BLUE)]
    panels = [(5, 1000.0, '(a) fitted-signal error (10$^{-3}$ OD)'),
              (6, 100.0, '(b) concentration away from true species (%)')]
    fig, axs = plt.subplots(1, 2, figsize=(6.3, 2.4))
    allends = []
    for ax, (col, scale, title) in zip(axs, panels):
        ends = []
        allends.append(ends)
        for key, name, color in series:
            y = [scale * np.nanmedian([r[col] for r in rows if r[0] == 'TI+RI'
                                       and r[2] == ri and r[3] == key]) for ri in ris]
            ax.plot(range(len(ris)), y, color=color, lw=2 if key != 'exact' else 1.4,
                    marker='o', ms=4, mec='white', mew=0.8, zorder=3)
            ends.append((y[-1], name))
        ax.set_xticks(range(len(ris)))
        ax.set_xticklabels(['0', '0.002', '0.005', '0.02'])
        ax.set_xlabel('RI noise, standard deviation per scan (OD)')
        ax.set_title(title, loc='left')
        ax.set_xlim(-0.2, len(ris) + 0.95)
        ax.set_ylim(bottom=0)
        ax.grid(axis='x', visible=False)
    fig.tight_layout(w_pad=1.5)
    fig.canvas.draw()
    for ax, ends in zip(axs, allends):
        # Direct labels at the line ends, pushed apart to at least 8.5 pt
        to_pt = ax.transData.transform
        ends.sort()
        pos = [to_pt((len(ris) - 1, y))[1] * 72.0 / fig.dpi for y, _ in ends]
        for i in range(1, len(pos)):
            pos[i] = max(pos[i], pos[i - 1] + 8.5)
        for (y, name), p in zip(ends, pos):
            y0 = to_pt((len(ris) - 1, y))[1] * 72.0 / fig.dpi
            ax.annotate(name, (len(ris) - 1, y), xytext=(5, p - y0),
                        textcoords='offset points', va='center', fontsize=7.2,
                        color='#0b0b0b')
    fig.savefig(os.path.join(OUT, 'fig_defects.svg'))


if __name__ == '__main__':
    fig_defects()
    if glob.glob(os.path.join(HERE, 'out', 'single_exact', '*.json')):
        fig_single()
