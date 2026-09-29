#import "style.typ": paper, rules-table

#let ff0 = $f\/f_0$
#let pending(x) = text(fill: red)[#x]

#show: paper.with(
  title: [Subgrid coverage in the parallel 2DSA:\ a benchmark in UltraScan],
  subtitle: [Fit outcomes for the proposed partitions and grids,\ and the solver defects they exposed],
  short-title: [Subgrid coverage: a benchmark in UltraScan],
  date: [September 29, 2026],
  abstract: [
    A recent discussion paper @coverage2026 identified a partition defect in custom-grid
    fits of the two-dimensional spectrum analysis (2DSA) and proposed three stages of
    change, from a geometric partition carried by the component order to subgrid lattices
    and grid points chosen in a finite-element (FEM) distance metric. Its evidence consisted
    of coverage measures; the effect on fits was left to future work. We have now fitted
    simulated data with the UltraScan analysis code itself: 21 datasets from seven mixtures,
    nine grid and partition configurations, single-pass and iterated, each compared with
    the exact full-grid optimum. The partition defect is confirmed: the current assignment
    leaves single-pass fits a median 149% above the optimum in residual RMSD (up to 894%).
    Beyond that, the proposal's coverage gains did not become consistent fit gains. The
    FEM-chosen lattice was better than the classic shifted grid on 13 of 21 datasets and
    worse on the rest, and every nondegenerate configuration stayed about 15% above the
    optimum, a floor that iterated fits did not remove. The floor comes from two defects
    in the concentration solve with time- and radially-invariant noise, which affect every
    noise-fitting analysis in UltraScan. With a corrected solver the floor disappears: the
    classic shifted grid comes within 0.1% of the optimum in the median, and the proposal's
    partitions and grids are then consistently but only marginally better, lowering the
    fitted-signal error from 0.14 to 0.08--0.11 $times 10^(-3)$ OD against a noise level of
    $3 times 10^(-3)$, where the solver correction itself lowers it fourteenfold. In four
    further cases in which rectangles fail or the grid is a union of partial grids, the
    general sublattice rule of Stage 1 removed the column subgrids of the current rule,
    improved on its diagonal subgrids with the corrected solver and did no harm elsewhere;
    choosing the sublattice in the FEM metric added nothing. Beyond Stage 1, what the proposal offers is mainly cost: a 1024-point grid placed in the FEM
    metric ran three times faster with two thirds of the memory, and in iterated fits the
    proposal's partitions reached the optimum with about a quarter fewer simulations. The
    corrected solver is itself a substantial change to long-standing numerics: its first
    implementation passed the synthetic benchmark and then failed on experimental data.
    We recommend Stage 1, a solver correction introduced as a versioned option and
    validated independently of this benchmark, and deferring Stages 2 and 3.
  ],
)

= Introduction

The parallel 2DSA fits sedimentation velocity data with a nonnegative combination of
finite-element solutions of the Lamm equation on a grid in sedimentation coefficient $s$ and
frictional ratio #ff0 @demeler2005 @brookes2010 @cao2005. The grid is divided into subgrids
that are fitted independently; their survivors are pooled and fitted again @brookes2006, and
in the iterated form the survivors are added to every subgrid until the set no longer changes
@brookes2026. The discussion paper that motivated this report @coverage2026 showed that, for
custom grids, subgrids are assigned by position in the stored component list, and that with
the row-major order of the current grid editor a $64 times 64$ grid in 64 subgrids yields
subgrids of a single $s$ value each. It proposed a geometric partition encoded in the
component order (Stage 1), subgrid lattices chosen in a metric given by the distance between
finite-element solutions under the experimental conditions (Stage 2), and grid points placed
in that metric (Stage 3).

That paper was careful to state that all its comparisons were coverage measures, and that a
direct validation with fits of simulated and experimental data was "the necessary next
step". This report is that step. Because the proposed changes alter which solutes are fitted
together, and therefore the results of analyses that users have run the same way for years,
we treat them as we would any change to a validated numerical code: the question is not
whether the new construction is more elegant, but whether it changes fitted results for the
better, by how much, at what cost, and with what risk.

The answer turned out to depend less on the partition than on a part of the code that the
proposal did not examine. Fits with time-invariant (TI) and radially invariant (RI) noise,
the default in practice, compute concentrations with a solver that does not minimize the
least-squares objective, and this error dominates every partition effect short of the
degenerate one. We report the benchmark (@sec:design and @sec:production), the solver
defects (@sec:defects), the benchmark repeated with a corrected solver (@sec:corrected),
experience with experimental data (@sec:experimental) and further defects found along the way
(@sec:further). @sec:assessment assesses each proposed stage and the solver correction, and
@sec:limitations states what the
benchmark cannot show.

= Benchmark design <sec:design>

== Data

Datasets were simulated with `us_astfem_sim`, using its `US_Astfem_Sim` class driven as its
command line does, so that the edited data, run definition, TimeState and noise vectors were
written exactly as for a user. The experiment matches the test case of @coverage2026: 50,000
rpm reached at 400 rpm/s, 20 °C, water, $macron(v) = 0.73$ mL/g, meniscus 5.9 cm, bottom
7.2 cm, 60 scans every 300 s from 300 s to 18,000 s, edited to 5.951--7.100 cm at 0.003 cm
(384 points). Noise, in `us_astfem_sim` terms relative to a total loading of 1.0 OD, was
random 0.3% ($sigma = 0.003$), TI 0.05% per point as a random walk, and RI 0.5%. Seven
mixtures (@tab:mixtures) were chosen to probe resolution in $s$ (M1, H1), in #ff0 at equal
$s$ (H3), widely separated shapes (M3), a continuum along a line (H2), a mixture of three
components (M2) and a minor aggregate beside a dominant species (A1). Each mixture was
simulated with three noise realizations and once without noise; the noise-free run is the
truth against which fitted signals are compared.

#figure(
  rules-table(
    columns: 2,
    align: (left, left),
    header: ([Mixture], [Species: $s$ (S), #ff0, loading concentration (OD)]),
    [M1], [(3.3, 1.25, 0.4), (6.1, 1.55, 0.6)],
    [M2], [(2.2, 1.4, 0.3), (4.7, 1.3, 0.4), (8.4, 2.6, 0.3)],
    [M3], [(2.5, 1.2, 0.5), (5.5, 3.5, 0.5)],
    [H1], [(4.3, 1.3, 0.95), (6.4, 1.45, 0.05)],
    [H2], [12 species on a line from (2.0, 1.2) to (8.0, 2.8), 1/12 each],
    [H3], [(4.0, 1.2, 0.5), (4.0, 2.5, 0.5)],
    [A1], [(6.5, 1.5, 0.90), (9.5, 1.6, 0.07), (3.5, 1.4, 0.03)],
  ),
  caption: [Simulated mixtures.],
) <tab:mixtures>

== Fits

Fits used the `us_2dsa` processing classes (`US_2dsaProcess`, its worker threads, ASTFEM and
the TI+RI noise solve of `US_SolveSim`) from a headless driver, with 4 threads and 64 subgrids.
Partitions were encoded exactly as Stage 1 proposes: as a `CUSTOMGRID` model whose components
are written interleaved, member $m$ of subgrid $i$ at position $64 m + i$, which the unchanged
fit code decodes with its modulo rule. That the programs recover every intended partition
this way is itself a result: Stage 1 needs no change to the analysis programs. Depth-1 and
deeper merges were taken in task order (debug option `2DSA-OrderedMerge`) rather than in the
order in which threads finish, because the latter makes results irreproducible
(@sec:merge). Single-pass fits used one refinement iteration, iterated fits up to ten.

== Configurations

@tab:configs lists the configurations. The first three are what users get today: the modulo
rule over a row-major $64 times 64$ custom grid, the same grid as a classic $8 times 8$ shifted
grid, and the regular-grid path of `us_2dsa` (`init_solutes` with eight repetitions). The
others implement the proposal: the best rectangle and the best sublattice in the FEM metric
(Stage 2), a $128 times 32$ grid with $8 times 8$ offsets or with its best sublattice, and grids
of 4096 and 1024 points placed by farthest-point sampling in the FEM metric with an
interleaved farthest-point partition (Stage 3). The metric was computed from ASTFEM
solutions for a $127 times 127$ candidate set on the same experiment, including rotor
acceleration, with TI and RI components projected out.

#figure(
  rules-table(
    columns: 4,
    align: (left, left, left, right),
    header: ([Name], [Fine grid], [Partition], [Points]),
    [modulo], [$64 times 64$ uniform, $s$ 1--10 S, #ff0 1--4], [current rule, $k$ mod 64], [4096],
    [classic $8 times 8$], [$64 times 64$ uniform], [shifted grid, $8 times 8$ offsets], [4096],
    [regular path], [$64 times 64$ uniform], [`init_solutes`, 8 repetitions], [4096],
    [rect. $4 times 16$], [$64 times 64$ uniform], [best rectangle in the FEM metric], [4096],
    [lattice (8, 3, 8)], [$64 times 64$ uniform], [best sublattice in the FEM metric], [4096],
    [$128 times 32$ $8 times 8$], [$128 times 32$ uniform], [$8 times 8$ offsets], [4096],
    [$128 times 32$ lattice], [$128 times 32$ uniform], [best sublattice, (16, 6, 4)], [4096],
    [FEM 4096], [farthest-point in the FEM metric], [interleaved farthest-point], [4096],
    [FEM 1024], [farthest-point in the FEM metric], [interleaved farthest-point], [1024],
  ),
  caption: [Grid and partition configurations, all with 64 subgrids. Sublattices are given as
    Hermite normal forms $(a, b, c)$ as in @coverage2026.],
) <tab:configs>

== Reference and measures

For each dataset, the exact optimum of the full $64 times 64$ grid was computed outside
UltraScan: the same ASTFEM columns, TI and RI components removed by double centering, and a
single nonnegative least-squares (NNLS) solve @lawson1995 over all 4096 columns. We report
the residual RMSD of each fit and its _excess_ over this optimum; the _fitted-signal error_,
the RMSD between the fitted signal and the noise-free data with TI and RI components removed
(a measure of how well the fit recovers the sample rather than the noise); Wasserstein
distances between fitted and true distributions of $s$ and #ff0\; the largest concentration
error of any true species; and wall time, peak memory and the number of solute simulations.
Grids other than the $64 times 64$ grid contain different points, so their fits can fall
slightly below this reference. Ratios between configurations are reported per dataset,
because the spread between datasets is larger than most of the differences of interest.

== The FEM metric recomputed with ASTFEM

The proposal computed its metric with a compact finite-volume Lamm solver without rotor
acceleration. Recomputed with UltraScan's ASTFEM, including acceleration, its coverage
figures are reproduced closely (@tab:metric): the worst-subgrid covering radii of the modulo
rule, the classic grid and the best lattice agree to within 1%, the same lattice (8, 3, 8)
is selected, and a $3 times 3$ anchor approximation again selects it. The one exception is
the 1024-point placed grid, whose worst subgrid improves from 18.8 to 16.1, a consequence
of the greedy partition rather than of the metric. The geometric part of the proposal is
therefore sound; what follows concerns its consequences for fits.

#figure(
  rules-table(
    columns: 7,
    align: (left, right, right, right, right, right, right),
    header: ([Grid and partition], [union $h$], [], [worst $h$], [], [median $h$], []),
    [], [paper], [ASTFEM], [paper], [ASTFEM], [paper], [ASTFEM],
    table.hline(stroke: 0.3pt),
    [$64 times 64$, modulo], [2.91], [2.89], [46.9], [46.8], [45.1], [45.0],
    [$64 times 64$, classic $8 times 8$], [2.91], [2.89], [19.5], [19.5], [17.4], [17.3],
    [$64 times 64$, rectangle $4 times 16$], [2.91], [2.89], [10.7], [10.6], [10.6], [10.5],
    [$64 times 64$, lattice (8, 3, 8)], [2.91], [2.89], [9.2], [9.1], [7.6], [7.6],
    [$128 times 32$, $8 times 8$ offsets], [0.98], [0.98], [12.0], [12.0], [10.1], [10.0],
    [$128 times 32$, lattice (16, 6, 4)], [0.98], [0.98], [9.5], [9.4], [7.6], [--],
    [FEM-placed 4096], [0.21], [0.21], [8.4], [8.6], [6.9], [6.9],
    [FEM-placed 1024], [0.89], [0.89], [18.8], [16.1], [12.3], [12.3],
  ),
  caption: [Covering radii in the FEM metric (TI and RI noise projected out) as reported in
    @coverage2026 (Tables 3 and 4) and recomputed from ASTFEM solutions with rotor acceleration.],
) <tab:metric>

= Results with the production solver <sec:production>

== The partition defect is real

@fig:single and @tab:single summarize the single-pass fits. The modulo rule, which leaves
every subgrid with a single value of $s$, is by far the worst configuration. Its median
excess over the optimum is 149% (maximum 894%), against 25.5% for the classic shifted grid,
and its fitted-signal error is 5.3 times that of the classic grid in the median of paired
datasets (worse on 20 of 21). Its fits contain a median of four solutes, and the worst
species concentration error, 0.29 OD in the median, is comparable to the loading of a
species (0.3--0.6 OD in most mixtures): a single-pass fit on these subgrids does not identify
the sample. The defect, and therefore
Stage 1, are confirmed by fit outcomes as well as by coverage.

#figure(
  image("fig_single.svg", width: 100%),
  caption: [Excess residual RMSD of single-pass fits over the exact full-grid optimum, per
    dataset (21 per configuration), with the production solver and with the corrected solver
    of @sec:corrected. Bars mark medians. The axis is linear between $-1%$ and $1%$ and
    logarithmic beyond.],
) <fig:single>

#figure(
  rules-table(
    columns: 9,
    align: (left, right, right, right, right, right, right, right, right),
    header: ([Configuration], [excess], [(max)], [signal err.], [$W_s$], [$W_(#ff0)$],
      [time (s)], [memory (MB)], [simulations]),
    [modulo], [149.3%], [893.9%], [6.75], [0.18], [0.72], [6.8], [162], [4174],
    [classic $8 times 8$], [25.5%], [101.6%], [2.22], [0.13], [0.27], [8.0], [169], [4621],
    [regular path], [29.6%], [122.9%], [2.45], [0.15], [0.33], [7.5], [157], [4563],
    [rect. $4 times 16$], [14.8%], [121.7%], [1.68], [0.13], [0.31], [8.0], [187], [4880],
    [lattice (8, 3, 8)], [14.4%], [120.2%], [1.66], [0.14], [0.32], [8.1], [188], [4875],
    [$128 times 32$ $8 times 8$], [15.3%], [135.5%], [1.72], [0.11], [0.29], [8.1], [187], [4873],
    [$128 times 32$ lattice], [14.4%], [109.1%], [1.67], [0.10], [0.26], [8.1], [188], [4873],
    [FEM 4096], [14.5%], [71.4%], [1.67], [0.12], [0.29], [7.8], [169], [4795],
    [FEM 1024], [14.7%], [50.5%], [1.68], [0.10], [0.22], [2.5], [119], [1541],
    table.hline(stroke: 0.3pt),
    [full-grid optimum], [--], [--], [0.11], [], [], [], [], [],
  ),
  caption: [Single-pass fits with the production solver: medians over 21 datasets. Excess
    RMSD over the full-grid optimum (with its maximum), fitted-signal error ($10^(-3)$ OD),
    Wasserstein distances of the $s$ (S) and #ff0 distributions from the truth, wall time on 4
    threads, peak memory and ASTFEM solute simulations.],
) <tab:single>

== Beyond the defect, the gains are small and inconsistent

Among the nondegenerate configurations the picture is less favorable to the proposal than
its coverage figures suggest. The medians in @tab:single order the configurations as the
covering radii do, the Stage 2 and 3 configurations reaching about 14.5% excess against
25.5% for the classic grid. But the medians hide a spread between datasets that is larger
than the differences between configurations, and paired comparisons are much weaker
(@tab:paired). The FEM-chosen lattice, which halves the worst covering radius of the classic
grid, gave a lower fitted-signal error on 13 of 21 datasets and a higher one on 8, with a
median ratio of 0.98 and individual ratios from 0.17 to 1.61. Per mixture, it was better on
average for M2, M3 and H2 and worse for M1, H1, H3 and A1; for H1 its mean excess was 68%
against 37% for the classic grid. The best rectangle behaved like the lattice. The
placed grids did slightly better (FEM 4096 better on 16 of 21, FEM 1024 on 20 of 21), but
with median ratios of 0.92 and 0.86 and worst cases 1.43 and 1.15 times the classic error.

#figure(
  rules-table(
    columns: 5,
    align: (left, right, right, right, right),
    header: ([Configuration vs classic $8 times 8$], [signal error: better], [median ratio],
      [range], [wall time ratio]),
    [modulo], [1 / 21], [5.27], [0.84--20.4], [0.86],
    [regular path], [6 / 21], [1.10], [0.90--1.87], [0.97],
    [rect. $4 times 16$], [12 / 21], [0.98], [0.27--1.89], [1.04],
    [lattice (8, 3, 8)], [13 / 21], [0.98], [0.17--1.61], [1.08],
    [FEM 4096], [16 / 21], [0.92], [0.12--1.43], [1.03],
    [FEM 1024], [20 / 21], [0.86], [0.12--1.15], [0.33],
  ),
  caption: [Paired comparison with the classic shifted grid, production solver, single pass:
    number of datasets with a lower fitted-signal error, and the median and range of the
    per-dataset ratios of fitted-signal error and of wall time.],
) <tab:paired>

A proposal that claims to halve the worst-case subgrid covering radius at no additional
fitting cost should be held to two further observations. First, the cost is not zero: the
lattice and rectangle partitions produced about 50% more depth-0 survivors than the classic
grid (a median of 636 against 419), which lengthened the merge tree and increased simulations
by 6% and wall time by 8% in the median. Second, the covering radius did not predict fit
quality within a dataset. This is consistent with the caution in @coverage2026 that NNLS
approximates an off-grid solute by neighboring grid points, but it means that coverage
cannot serve as the acceptance criterion for Stage 2.

== A common floor, and iterated fits do not reach the optimum

Every nondegenerate configuration settled at the same median excess of about 15%, and none
came within 1% of the optimum on more than three of 21 datasets. Iterated fits did not remove this floor (@tab:iterated): with the production
solver the modulo rule, the classic grid, the lattice and the FEM 4096 grid all ended at a
median excess of 14.6--14.9%. About half of the runs (10 to 12 of 21 per configuration)
stopped at the limit of ten iterations without converging; the others converged to
solutions just as far from the optimum. The iterated method is exact only if each subproblem is solved exactly
@brookes2026. That iterated fits of every partition converge to the same 15% excess, while
the exact optimum of the same grid is known, locates the floor in the solver rather than in
the partition. @sec:defects identifies the cause.

#figure(
  rules-table(
    columns: 7,
    align: (left, right, right, right, right, right, right),
    header: ([Configuration], [excess], [(max)], [signal err.], [iterations], [time (s)],
      [simulations]),
    [modulo], [14.6%], [35.4%], [1.69], [10], [85.7], [50,291],
    [classic $8 times 8$], [14.9%], [83.2%], [1.71], [10], [86.5], [50,554],
    [lattice (8, 3, 8)], [14.7%], [135.3%], [1.69], [10], [86.3], [50,837],
    [FEM 4096], [14.8%], [85.6%], [1.68], [9], [79.1], [48,340],
  ),
  caption: [Iterated fits (at most ten iterations) with the production solver: medians over
    21 datasets. Units as in @tab:single.],
) <tab:iterated>

== Cost

The one robust benefit in these runs is the cost of the smaller placed grid. FEM 1024 ran
in a third of the time of the classic grid on every dataset (2.5 s against 8.0 s in the
median), with two thirds of the peak memory (119 against 169 MB) and a third of the
simulations, with an equal or lower fitted-signal error on 20 of 21 datasets. This is the saving that
@coverage2026 anticipated for Stage 3, and it grows with grid size. It should be weighed
against what Stage 3 requires before a fit: a candidate set simulated for the specific
experiment (16,129 ASTFEM solutions here) and a selection step, neither of which exists in
UltraScan today.

== Merge order and merge size <sec:merge>

Two properties of the merge tree bear on how precisely any of these configurations can be
compared. In the production code, depth-1 and deeper merges combine results in the order
in which worker threads finish. Six repetitions of the same classic-grid fit of one dataset
(M1) gave residual RMSDs from 0.003154 to 0.003209, excesses of 6.1% to 8.0%; in task order
three repetitions gave the same value to all digits. For the lattice the spread was 0.1%.
Differences of a few percent between single-pass fits are therefore not reproducible in
UltraScan as deployed. Enlarging the merge pool to 512 solutes (debug option
`2DSA-MergePool`), a natural response to a floor that looks like lost survivors, did not help:
over 10--11 datasets the median excess was unchanged for the lattice and FEM grids and
worse for the classic grid (35.8% against 31.1%), at 1.9--3.0 times the wall time and 1.8--2.3
times the memory.

= Two defects in the noise-fitting solve <sec:defects>

== Radially invariant noise is not eliminated from the concentration solve

With TI and RI noise, `US_SolveSim::calc_residuals` eliminates the noise algebraically before
solving for concentrations. For an $n_t times n_r$ array, the exact elimination of both noise
types is double centering: removing the mean over scans at each radius (TI) and the mean over
radii of each scan (RI). The code instead forms the data and simulation vectors as
$b - overline(b)^"col" + overline(b)$, removing the per-radius means and adding back the grand
mean, but not removing the per-scan means; these enter only afterwards, when the RI noise
vector is formed. The concentration solve therefore minimizes the residual plus a penalty of
$n_r$ times the squared per-scan mean residual, so that the concentrations are pulled toward
fitting the scan-to-scan offsets that the RI noise term should absorb, and the retained grand
mean ties the model level to the data level. The bias grows with the RI amplitude.
We refer to this as defect D1.

== NNLS is applied to the normal equations as if they were the design matrix

The concentrations are then computed by Lawson--Hanson NNLS @lawson1995 applied to the
normal equations $G = A^T A$, $g = A^T b$ (`small_a`, `small_b`), passed to `US_Math2::nnls`
as design matrix and right-hand side. This minimizes $norm(G x - g)$ rather than
$norm(A x - b)$. The two have the same unconstrained solution but different solutions once
nonnegativity constraints are active, which in 2DSA is always: most grid points are at zero.
The correct use of the normal equations is to factor $G = L L^T$ and solve the NNLS problem
$(L^T, L^(-1) g)$, which has the least-squares objective; alternatively NNLS can be applied
to the noise-projected design matrix itself, which avoids squaring its condition number
@bjorck1996. We refer to this as defect D2. It affects TI-only and RI-only fits as well.

== A controlled test

To separate the two defects from the merge tree, we solved a single full-grid problem
($32 times 32$ grid, the same ASTFEM columns) for five mixtures, two noise realizations and
four RI amplitudes, with each defect alone, both together (the production calculation) and
neither (@fig:defects, @tab:defects). D2 alone barely changes the residual (0.2--0.6%) but
raises the fitted-signal error by a third and doubles the error in #ff0: it redistributes
concentration among nearly equivalent solutes, which a residual does not detect and a
distribution does. D1 alone grows with the RI amplitude, to +20% in RMSD and six times the
signal error at $sigma_"RI" = 0.02$. Together they are worse than either: without any RI
noise the production solve places 26% of the concentration away from every true species
(7% for the exact solve), and at $sigma_"RI" = 0.02$ it places 74% there, with an RMSD 60%
above the exact solution. These are not rounding effects; they are properties of the
objective being minimized.

#figure(
  image("fig_defects.svg", width: 100%),
  caption: [Single full-grid NNLS solves ($32 times 32$ grid, no merge tree) with TI and RI
    noise, medians over five mixtures and two noise realizations: (a) fitted-signal error and
    (b) fraction of the fitted concentration farther than 0.6 S or 0.3 in #ff0 from every true
    species, for the exact solve, each defect alone, and both (the production calculation).],
) <fig:defects>

#figure(
  rules-table(
    columns: 9,
    align: (left, right, right, right, right, right, right, right, right),
    header: ([$sigma_"RI"$ (OD)], [RMSD], [], [], [], [signal error], [], [], []),
    [], [exact], [D2], [D1], [D1+D2], [exact], [D2], [D1], [D1+D2],
    table.hline(stroke: 0.3pt),
    [0], [2.996], [3.014], [3.047], [3.093], [0.33], [0.42], [0.70], [0.86],
    [0.002], [2.995], [3.007], [3.056], [3.260], [0.34], [0.44], [0.68], [1.35],
    [0.005], [2.995], [3.007], [3.130], [3.708], [0.34], [0.43], [1.00], [2.21],
    [0.02], [2.980], [2.994], [3.561], [4.769], [0.33], [0.44], [1.95], [3.71],
    table.hline(stroke: 0.3pt),
    [TI only], [3.002], [3.020], [--], [--], [0.34], [0.43], [--], [--],
  ),
  caption: [Controlled defect test (@fig:defects): residual RMSD with TI and RI components
    removed, and fitted-signal error, both in $10^(-3)$ OD, medians over ten solves. The
    TI-only row compares the exact TI-only solve with the production TI-only calculation,
    which has only defect D2.],
) <tab:defects>

== Scope

`US_SolveSim` is shared by `us_2dsa`, `us_pcsa` and every method of `us_mpi_analysis`
(2DSA, GA, DMGA and PCSA), so both defects apply to all fits with TI and/or RI noise in
UltraScan, on the desktop and on the clusters, and to every such analysis in the
archives. The size of the effect in a given analysis depends on the RI amplitude and on how
many nearly equivalent solutes compete, which is to say on the data and the grid. It is not
confined to data with strong RI noise: without any RI noise the production calculation
still placed 26% of the concentration away from the true species in the controlled test,
against 7% for the exact solve. We have not assessed how the defects bear on published
results. They are systematic, they bias shape more than residual, and they cannot be
detected by inspecting residuals.

= Results with a corrected solver <sec:corrected>

We repeated the benchmark with the debug option `SolveSim-ExactNoise`, which computes
concentrations with both noise types eliminated by double centering and with NNLS on the
Cholesky factor of the normal equations (branch `claude/keen-maxwell-i5daw2`, Appendix A).
Without the option the code is bit-identical to the production code; with it, merge and
final tasks also drop duplicate solutes, which the exact solve would otherwise keep after
refinement iterations (@sec:further).

== Single pass

The corrected solver changes the picture completely (@fig:single, @tab:exact). The floor
disappears: the classic grid comes within 0.1% of the optimum in the median (at most 0.7%,
within 0.1% on 12 of 21 datasets), and every Stage 2 and Stage 3 configuration is within
0.1% on all 21 datasets; the $128 times 32$ and placed grids fall slightly below the
$64 times 64$ reference (to $-0.33%$) because their points are placed more finely where it
matters. The fitted-signal error of the classic grid falls fourteenfold, from 2.22 to
$0.14 times 10^(-3)$ OD, the Wasserstein distance of the #ff0 distribution from 0.27 to 0.07,
and the largest species concentration error from 0.023 to 0.001 OD. No change to partitions
or grids comes near this. The modulo rule remains degenerate (101% median excess): a correct
solver cannot recover solutes that no subgrid can represent.

#figure(
  rules-table(
    columns: 9,
    align: (left, right, right, right, right, right, right, right, right),
    header: ([Configuration], [excess], [(max)], [signal err.], [$W_s$], [$W_(#ff0)$],
      [vs classic], [time (s)], [memory (MB)]),
    [modulo], [101.5%], [243.4%], [5.18], [0.12], [0.67], [3 / 21], [8.1], [213],
    [classic $8 times 8$], [0.1%], [0.7%], [0.14], [0.05], [0.07], [--], [9.3], [216],
    [regular path], [0.6%], [2.7%], [0.28], [0.08], [0.12], [2 / 21], [9.1], [214],
    [rect. $4 times 16$], [0.0%], [0.1%], [0.12], [0.05], [0.07], [18 / 21], [10.0], [231],
    [lattice (8, 3, 8)], [0.0%], [0.0%], [0.11], [0.05], [0.05], [16 / 21], [9.8], [236],
    [$128 times 32$ $8 times 8$], [0.0%], [0.1%], [0.10], [0.03], [0.06], [17 / 21], [9.9], [219],
    [$128 times 32$ lattice], [0.0%], [0.1%], [0.10], [0.03], [0.05], [17 / 21], [9.6], [223],
    [FEM 4096], [0.0%], [0.0%], [0.08], [0.03], [0.05], [17 / 21], [9.3], [231],
    [FEM 1024], [0.0%], [0.0%], [0.10], [0.03], [0.09], [16 / 21], [3.1], [139],
    table.hline(stroke: 0.3pt),
    [full-grid optimum], [--], [--], [0.11], [], [], [], [], [],
  ),
  caption: [Single-pass fits with the corrected solver (`SolveSim-ExactNoise`): medians over
    21 datasets, units as in @tab:single. "vs classic" is the number of datasets with a lower
    fitted-signal error than the classic grid.],
) <tab:exact>

With the floor removed, the proposal's partitions are now consistently better than the
classic grid, where under the production solver they were not: the FEM-chosen lattice gave
a lower fitted-signal error on 16 of 21 datasets (median ratio 0.89), the best rectangle on
18 (0.88), and the $128 times 32$ lattice and the 4096-point placed grid on 17 (0.67 and
0.68). The gains are real but small in absolute terms. The fitted-signal error falls from
0.14 to 0.11 (lattice) or 0.08 (FEM 4096) $times 10^(-3)$ OD, against a noise level of
$3 times 10^(-3)$ OD; the excess RMSD falls from 0.1% to 0.0%. In the #ff0 distribution the
lattice reduces the Wasserstein distance from 0.07 to 0.05. The measures do not all agree:
the largest species concentration error, about 0.001 OD in the median for every
nondegenerate configuration, was higher for the lattice and the placed grids than for the
classic grid on 12 to 15 of 21 datasets, and the 1024-point grid gave a coarser #ff0
distribution than the classic grid on 12 of 21 (median ratio 1.7). Against the corrected
solver's own effect, a factor of 14 in signal error, these are second-order differences,
and which configuration is better depends on the measure.

The regular-grid path of `us_2dsa` deserves a note. It builds the same fine grid and the
same $8 times 8$ subgrids as the classic custom grid, but numbers them with the #ff0 offset
varying fastest, where our custom grid varies the $s$ offset fastest; merge tasks therefore
pool different subgrids. Its fits were 0.6% above the optimum in the median (2.7% at most)
against 0.1% for the classic custom grid; its fitted-signal error was higher on 18 of 21
datasets and within 0.1% on the other three. Which subgrids are
merged together evidently matters at this level, and pooling across $s$ offsets, the
direction the FEM metric identifies as the costly one, did better. This is a single
observation and we do not build a recommendation on it, but it is the kind of effect a
production change would have to control for.

The corrected solve costs more: 19% more wall time and 17% more CPU time than the production
calculation in the same configurations, and 30% more peak memory, mostly for a copy of the
simulation matrix with the per-scan means removed. The projected-matrix implementation of
the review branch avoids the normal equations entirely and was faster than the production
code on the experimental dataset of @sec:experimental (48--61 s against 64--68 s); it was not run on
the full simulated matrix.

== Iterated fits

With the corrected solver, iterated fits behave as the theory says they should
@brookes2026 (@tab:iter-exact). Of 84 runs, 83 ended within 0.01% of the optimum, including
all 21 with the modulo rule; the exception, a classic-grid fit of H1, was still 2.1% above the
optimum when it reached the limit of ten iterations. The partition no longer decides where
the iteration ends, but it decides how fast it gets there. The lattice and the placed grid
needed a median of four iterations, against five for the classic grid and seven for the modulo
rule, and in paired comparison 28% (lattice) and 24% (FEM 4096) fewer simulations than the
classic grid; the modulo rule needed 29% more. This is an argument for Stage 2 that the
single-pass results do not provide: for fits that are iterated, a better partition saves about
a quarter of the computation without changing the result. The corrected iterated fits were
also cheaper than those of the production solver, which ran to the limit in about half of the
runs (a median of 40--75 s against 79--87 s, and 22,000--40,000 simulations against about
50,000). The wall times of this set were measured while lighter work ran on the same machine;
the simulation counts, which do not depend on it, give the same ratios.

#figure(
  rules-table(
    columns: 7,
    align: (left, right, right, right, right, right, right),
    header: ([Configuration], [excess], [(max)], [signal err.], [iterations], [time (s)],
      [simulations]),
    [modulo], [0.0%], [0.0%], [0.11], [7 (4--10)], [74.8], [39,579],
    [classic $8 times 8$], [0.0%], [2.1%], [0.13], [5 (3--10)], [54.0], [31,454],
    [lattice (8, 3, 8)], [0.0%], [0.0%], [0.11], [4 (3--5)], [39.9], [22,449],
    [FEM 4096], [$-0.0%$], [$-0.0%$], [0.09], [4 (3--6)], [38.5], [22,842],
  ),
  caption: [Iterated fits (at most ten iterations) with the corrected solver: medians over 21
    datasets, with the range of iterations. Units as in @tab:single.],
) <tab:iter-exact>

= Where the general rule differs from the classic grid <sec:general>

With 64 subgrids on the grids of @tab:configs, good rectangular partitions exist, and in
index units the rule of Stage 1 simply selects the classic $8 times 8$ offsets. The general
sublattice rule of @coverage2026 (Hermite normal forms, with Algorithm 1 for unions of
partial grids) matters where rectangles fail. We therefore fitted four further cases
(@tab:gen-cover): a $100 times 100$ grid with 101 subgrids, the count that the odd-count
override produces for such a grid, for which the modulo rule gives diagonal subgrids and no
rectangle leaves every subgrid populated; a $64 times 64$ grid with a prime count of 41, for
which the modulo generator (23) is benign; a $60 times 60$ grid with 60 subgrids, for which
the modulo rule gives columns; and a union of two partial grids with 32 subgrids, a fine band
($64 times 21$ points, #ff0 1--1.95) stored before a coarse band ($22 times 15$ points, #ff0
2--4), in which the modulo rule turns the fine band into pairs of columns. Each was
partitioned by the modulo rule, by the best sublattice in index units (Stage 1) and by the
best sublattice in the FEM metric (Stage 2); sublattice cosets of unequal size on the finite
grid were balanced as Algorithm 1 prescribes, so that the unchanged programs decode them.
All 21 datasets were fitted once with each partition and each solver, and compared with the
exact optimum of the same fine grid.

#figure(
  rules-table(
    columns: 5,
    align: (left, left, left, right, right),
    header: ([Case], [Partition], [Choice], [worst $h$], [median $h$]),
    [$100 times 100$, 101], [modulo (diagonals)], [--], [14.1], [12.0],
    [], [Stage 1, index units], [(101, 10, 1)], [11.1], [8.4],
    [], [Stage 2, FEM metric], [(101, 15, 1)], [8.0], [6.7],
    table.hline(stroke: 0.3pt),
    [$64 times 64$, 41], [modulo ($g = 23$)], [--], [8.4], [6.5],
    [], [Stage 1, index units], [(41, 6, 1), 18 moves], [8.4], [7.5],
    [], [Stage 2, FEM metric], [(41, 11, 1), 11 moves], [8.6], [6.5],
    table.hline(stroke: 0.3pt),
    [$60 times 60$, 60], [modulo (columns)], [--], [46.8], [45.0],
    [], [best rectangle, index units], [$6 times 10$], [16.4], [14.9],
    [], [Stage 1, index units], [(30, 7, 2)], [11.7], [8.3],
    [], [Stage 2, FEM metric], [(15, 11, 4)], [9.6], [8.0],
    table.hline(stroke: 0.3pt),
    [union, 32], [modulo], [--], [25.3], [15.6],
    [], [Algorithm 1, index units], [(32, 5, 1) twice, 6 moves], [10.0], [9.3],
    [], [Algorithm 1, FEM metric], [(32, 7, 1), (32, 17, 1), 7 moves], [10.2], [9.6],
    [], [interleaved farthest-point], [--], [11.0], [9.2],
  ),
  caption: [Partitions of the further cases and their worst and median subgrid covering radii
    in the FEM metric (against the $127 times 127$ proxy). "Moves" are the points moved by the
    size balancing of Algorithm 1; sublattices are Hermite normal forms $(a, b, c)$.],
) <tab:gen-cover>

The index-unit choices are those of @coverage2026 (Table 1: (101, 10, 1), (41, 6, 1),
(30, 7, 2) and the $6 times 10$ rectangle). Two properties of the table bear on the
proposal. First, degeneracy in index units need not be degeneracy in the FEM metric: the
diagonal subgrids of the $100 times 100$ grid, 5.6 times worse than the best sublattice in
index units according to @coverage2026, are only 1.3 times worse in the FEM metric, because
every diagonal contains every value of $s$ and steps in #ff0 are cheap. Columns, which
contain a single $s$, are degenerate in both. Second, the size balancing that the modulo
decoding requires can erase the advantage of a choice made in the FEM metric: for the
$64 times 64$ grid with 41 subgrids and for the union, the FEM-chosen partitions were
slightly worse after balancing than the index-unit ones.

#figure(
  rules-table(
    columns: 8,
    align: (left, left, right, right, right, right, right, right),
    groups: (
      [], [],
      table.cell(colspan: 2, align: center)[production solver],
      table.cell(colspan: 4, align: center)[corrected solver],
      table.hline(start: 2, end: 4, stroke: 0.4pt),
      table.hline(start: 4, end: 8, stroke: 0.4pt),
    ),
    header: ([Case], [Partition], [excess], [vs modulo], [excess], [(max)], [signal err.],
      [vs modulo]),
    [$100 times 100$, 101], [modulo (diagonals)], [17.6%], [--], [0.1%], [4.1%], [0.18], [--],
    [], [Stage 1], [14.4%], [12 (0.94)], [0.0%], [0.1%], [0.13], [18 (0.71)],
    [], [Stage 2], [14.9%], [13 (0.93)], [0.0%], [0.0%], [0.10], [18 (0.54)],
    table.hline(stroke: 0.3pt),
    [$64 times 64$, 41], [modulo ($g = 23$)], [14.9%], [--], [0.0%], [0.0%], [0.11], [--],
    [], [Stage 1], [14.8%], [8 (1.00)], [0.0%], [0.1%], [0.13], [10 (1.00)],
    [], [Stage 2], [15.4%], [9 (1.01)], [0.0%], [0.0%], [0.12], [13 (1.00)],
    table.hline(stroke: 0.3pt),
    [$60 times 60$, 60], [modulo (columns)], [170.1%], [--], [101.7%], [243.5%], [5.19], [--],
    [], [rectangle $6 times 10$], [23.1%], [19 (0.24)], [0.0%], [0.7%], [0.19], [18 (0.04)],
    [], [Stage 1], [14.5%], [20 (0.22)], [0.0%], [0.0%], [0.14], [21 (0.03)],
    [], [Stage 2], [14.5%], [20 (0.24)], [0.0%], [0.1%], [0.14], [21 (0.03)],
    table.hline(stroke: 0.3pt),
    [union, 32], [modulo], [24.8%], [--], [0.5%], [6.7%], [0.34], [--],
    [], [Algorithm 1, index], [15.0%], [15 (0.91)], [0.0%], [0.0%], [0.13], [20 (0.43)],
    [], [Algorithm 1, FEM], [14.9%], [17 (0.79)], [0.0%], [0.0%], [0.13], [19 (0.43)],
    [], [farthest-point], [12.8%], [16 (0.86)], [0.0%], [0.0%], [0.13], [18 (0.43)],
  ),
  caption: [Single-pass fits of the further cases: median excess RMSD over the exact optimum
    of the same fine grid with the production and the corrected solver, the maximum and the
    median fitted-signal error ($10^(-3)$ OD) with the corrected solver, and the number of the
    21 datasets on which the partition gave a lower fitted-signal error than the modulo rule
    (median ratio in parentheses).],
) <tab:gen-fits>

The fits (@tab:gen-fits) separate the cases more sharply than the covering radii. Where the
modulo rule produces columns, the general rule is indispensable. For the $60 times 60$ grid
the modulo fits were 170% above the optimum with the production solver and 102% with the
corrected one, and every geometric partition removed the defect, with a thirtieth of the
signal error under the corrected solver
(ratio 0.03); the sublattice also beat the best rectangle under
the production solver (14.5% against 23.1%, better on 15 of 21 datasets) and matched it
under the corrected one. The union behaved alike on a smaller scale, since only its fine
band degenerates: 24.8% against 15.0% with the production solver, 0.5% (up to 6.7%)
against 0.0% with the corrected one. With the corrected solver Algorithm 1 lowered the
fitted-signal error 2.3-fold and was better on 20 of 21 datasets; with the production
solver the gain was small (15 of 21, ratio 0.91). The interleaved farthest-point partition
did as well as Algorithm 1.

For the diagonal subgrids of the $100 times 100$
grid the answer depends on the solver. With the production solver they were barely worse
than the sublattice (17.6% against 14.4%, the sublattice better on only 12 of 21 datasets
and with the larger worst case, 123% against 53%), as the FEM metric, but not index units,
would predict. With the corrected solver the sublattice was better on 18 of 21 datasets,
lowering the signal error by 29% and the Wasserstein distance of the #ff0 distribution
from 0.11 to 0.04.

For the benign prime count, $64 times 64$ with 41 subgrids, all
partitions gave the same results with either solver: the general rule, including its 18
balancing moves, did no harm. Choosing the sublattice in the FEM metric rather than in
index units made no material difference in any case (median ratios of the fitted-signal
error between 0.98 and 1.02).

Two consequences for the proposal follow. The index-unit analysis of @coverage2026 (its
Fig. 3) treats line-like subgrids, columns and diagonals alike, as degenerate; the fits show that columns,
which occur when the subgrid count divides the row length (#box($g = 0$)), are the dangerous case,
and diagonals (#box($g = plus.minus 1$)) a mild one. And the general rule of Stage 1, in index
units and including Algorithm 1 for unions, did what it is meant to do in every case
tested: it removed the degenerate subgrids and did no harm where there were none.

= Experimental data <sec:experimental>

== Dataset and workflow

We also followed a workflow reported by a user on an experimental dataset: pseudo-absorbance
from intensity data at 294 nm, 98 scans by 935 radial points (6.04--6.98 cm), fitted on a
grid of $s$ 0.5--18 S by #ff0 1--4 ($96 times 64$ points, eight repetitions, 64 subgrids).
The workflow first fits TI noise alone, then loads the data with that TI noise subtracted
and fits TI and RI noise together, or RI noise alone. On TI-corrected data, a TI+RI fit
has strictly more freedom than an RI-only fit, so its optimum cannot have a higher
residual; with the production solver it did (@tab:real): 0.002233 against 0.002118.

#figure(
  rules-table(
    columns: 4,
    align: (left, right, right, right),
    header: ([Solver, merge order], [TI only], [TI-corrected, TI+RI], [TI-corrected, RI only]),
    [production, task order], [2.978 (0.76)], [2.233 (0.57)], [2.118 (0.52)],
    [`ExactNoise` (corrected), task order], [2.970 (0.76)], [2.061 (0.50)], [2.030 (0.48)],
    [projected NNLS (review branch), task order], [2.970 (0.76)], [2.061 (0.50)], [2.030 (0.48)],
    table.hline(stroke: 0.3pt),
    [`ExactNoise`, first implementation, + cache], [2.935 (0.75)], [*10.10 (0.98)*], [--],
    [`ExactNoise` (corrected), + cache], [2.935 (0.75)], [2.026 (0.48)], [2.060 (0.50)],
    [projected NNLS, + cache], [2.935 (0.75)], [2.042 (0.49)], [2.031 (0.48)],
  ),
  caption: [Experimental dataset: residual RMSD ($10^(-3)$ OD) of single-pass fits, with the
    lag-1 autocorrelation of the residuals along the radius in parentheses. Rows below the
    rule used thread-arrival merge order with 16 threads and the simulation cache
    (`2DSA-SimCache`), as in the user's desktop runs.],
) <tab:real>

With either corrected solver the inversion shrinks to 0.002061 against 0.002030 in task
order; in arrival order the TI+RI fit was the better of the two with one corrected solver
and the worse with the other. To test whether
the remaining difference is a solver error, we took the 44 input solutes of the final
RI-only fit and solved them with each elimination: 0.002030 with RI elimination, 0.002029
with TI and RI. The remaining difference is therefore the single-pass merge path retaining a
different solute set, which is what refinement iterations are designed to correct.

Two further observations limit what this dataset can show. All fits leave residuals with a
lag-1 autocorrelation along the radius of 0.48--0.76: the model does not describe these data
to the noise level, and differences of a few percent in RMSD sit on top of a systematic
misfit that no partition or solver addresses. And fits in arrival order with the same
solver differed by up to 3% between runs (0.002042 and 0.002103 for the projected NNLS with
and without the cache), more than most of the differences discussed.

== The first corrected implementation failed

The row marked in bold in @tab:real is the fit that the user reported: with the first
implementation of `SolveSim-ExactNoise` and the simulation cache enabled, the TI+RI fit of
TI-corrected data reached an RMSD of 0.0101 with strongly structured residuals, while TI-only
and RI-only fits looked normal. The cause was in the new code. With both noise types
eliminated, the normal-equation matrix of a subgrid has condition numbers of $10^15$ to
$10^17$, and rounding makes it slightly indefinite (smallest computed eigenvalues near
$-1.4 times 10^(-9)$ against a ridge of $1.2 times 10^(-9)$). The factorization clamped a
negative pivot to the square root of the ridge and continued; every later row divided
rounding-sized remainders by that pivot, the errors grew from row to row (to $-0.43$,
$-2 times 10^7$, $-5 times 10^23$ in successive pivots) and overflowed, and the subgrid returned
no solutes. In the failing fit, 43 of 74 solves ended this way. The cache stores simulations
in single precision, which shifted the rounding enough to trigger the failure on this
dataset; of the 76 systems of a fit without the cache, 47 also overflowed when replayed with
the original factorization in double precision outside UltraScan. This is the textbook failure of Cholesky factorization on a
semidefinite matrix @higham2002, and it was not caught by the synthetic benchmark, in
which it did not occur.

The corrected factorization (commit `8a8e0b219`) uses a ridge of $10^(-9)$ of each diagonal
element, treats a column whose remaining pivot does not exceed its ridge as dependent on the
preceding ones, and falls back to the production calculation if the result is not finite.
On the 76 systems above it matches an eigen-decomposition reference to within $10^(-11)$
(absolute) in the objective. An independent implementation on a different branch
(`claude/nnls-us-solve-sim-review-eufblk`), which applies NNLS directly to the
noise-projected design matrix and never forms the normal equations, gave the same results
to all printed digits in task order and ran 23--32% faster (@tab:real). That two
independent implementations agree is the strongest evidence we have that the corrected
results are right; that the first one failed is the strongest evidence that this code
should not be changed casually.

= Further defects found along the way <sec:further>

The benchmark exercised parts of the code in combinations that ordinary use rarely does,
and several further defects surfaced. We list them because together they describe the
state of the code into which the proposal would be introduced.

- *Merge order.* The production merge tree depends on the order in which threads finish, so
  single-pass results are not reproducible (@sec:merge). A task-ordered merge exists as a
  debug option on `claude/keen-maxwell-i5daw2`.
- *Duplicate solutes.* After refinement iterations every subgrid returns the solutes added
  from the previous iteration, so merge tasks and the final fit receive several copies of each.
  With an exact solve, identical columns can share a concentration: a first version of the
  corrected solver let the final fit grow to 832 inputs with 13 distinct solutes, taking more
  than 1000 s and 2 GB. Duplicates are now dropped in merge and final tasks on both branches.
- *A scheduler hang in* `us_2dsa`. When the last subgrid of an iteration finishes, the
  maximum task depth is reset to 1 even if merges at depth 2 are already queued; their
  results then satisfy neither the condition for the final fit nor the one for further
  merges, and the fit stops without finishing. The hang is present on `main`; smaller merge
  tasks make it likely (in one of five iterated fits with duplicate removal). It is fixed
  on the review branch (commit `8b24aad1f`); `us_mpi_analysis` does not have it.
- *NNLS round-off branch.* In `US_Math2::nnls`, the feasibility re-check after a round-off
  step never resets its flag, so once entered it does not terminate and indexes before the
  start of its work array; a tie of two coefficients reaching zero crashed it. Fixed on the
  review branch (commit `b63bfd079`).
- *Tikhonov rows with noise.* With regularization and TI or RI noise, the noise helpers read
  the design matrix with the wrong column stride. Fixed on the review branch.
- *Debug option matching.* `us_2dsa` matched debug options case-insensitively and the solver
  case-sensitively, so a differently capitalized entry could enable half of
  `SolveSim-ExactNoise`. Fixed in `8a8e0b219`.

None of these is exotic, and several date from 2010 to 2013. They are what one should
expect to find when long-used code is exercised systematically for the first time, and
they argue for changing it one reviewed step at a time.

= Assessment <sec:assessment>

== Stage 1: a geometric partition carried by component order

Stage 1 is supported by fit outcomes and carries little risk. The modulo rule over a
row-major grid produces single-pass fits that are wrong by any measure, the correction needs
no change to the programs deployed on clusters (the benchmark encoded every partition this
way), and it affects only grids saved after the change. It should be introduced together with
the proposed report of the worst-subgrid covering radius in the editor and in the run log,
and the odd-count override should call the same routine. For the $64 times 64$ grid, Stage 1
selects the classic $8 times 8$ offsets (the best sublattice in index units), so its effect
here is the difference between the modulo and classic rows of @tab:single and @tab:exact: a
median factor of 5.3 in fitted-signal error with the production solver and of 27 with the
corrected one. Where the general rule differs from the classic grid (@sec:general), it
removed the column subgrids of a $60 times 60$ grid with 60 subgrids and of the fine band of
a union of partial grids under both solvers, improved on the diagonal subgrids of a
$100 times 100$ grid with 101 subgrids under the corrected solver, and did no harm for a
benign prime count; Algorithm 1, with its rotated offsets and size balancing, worked as
intended for the union. Stage 1 is therefore supported as a general rule, not only where it
reproduces the classic grid. Users who re-save an existing grid will obtain different
single-pass results, and this should be documented.

== Stage 2: subgrids shaped by the FEM metric

We do not recommend Stage 2 as a default for single-pass fits. Its coverage advantage is
real (@tab:metric), but its effect on fits depends on the solver. Under the production solver
it helped on 13 of 21 datasets and hurt on the others. Under the corrected solver it helps
consistently (16 of 21) but by an amount, $0.03 times 10^(-3)$ OD in fitted-signal error and
0.1% in RMSD, that is two orders of magnitude below the noise and small beside the solver
correction itself; and in a single pass it costs 6--9% more simulations and time, not "no
additional cost". It also changes which solutes are fitted together and therefore changes
results: a cost in reproducibility for users and, in regulated work, in revalidation. The
one benefit that would pay for this is in iterated fits with the corrected solver, which
reached the same optimum with about a quarter fewer simulations. In the four further cases
of @sec:general, the FEM-chosen sublattice was no better than the index-unit one with either
solver. If pursued, Stage 2 should
therefore follow the solver correction, as an option aimed at iterated fits, and be
evaluated on experimental data.

== Stage 3: grids placed in the FEM metric

Stage 3 has the one robust benefit found here: a quarter of the grid points, a third of the
time and two thirds of the memory, at equal residual and fitted-signal error. The saving is
not entirely free: with the corrected solver the 1024-point grid gave a coarser #ff0
distribution than the classic grid on 12 of 21 datasets. It is also the largest change:
a new kind of grid, generated per experiment from thousands of simulations, with no
counterpart in the editor, the LIMS or the cluster submission path, and with known boundary
over-sampling. We would pursue it as a cost-reduction project after the solver question is
settled, validated on experimental data with model error, before any user sees it.

== The solver correction

The solver correction matters more than any stage of the proposal, and it is also the most
dangerous change discussed here. It alters the concentrations of every TI or RI fit in
UltraScan, including all cluster analyses; it makes results incomparable with the archive,
which matters most under GMP; and its first implementation passed the synthetic benchmark and
then failed on the first experimental dataset it met. We therefore recommend:

+ one implementation, not two; the projected-matrix NNLS of the review branch is simpler,
  faster and does not depend on factoring a numerically singular matrix;
+ a versioned option recorded with every model and noise file, off by default, rather than
  a debug flag;
+ validation independent of this benchmark: an external reference solver on archived
  experimental datasets of each optical system, TI-only, RI-only and TI+RI fits, Monte Carlo
  iterations, meniscus and bottom fits, custom grids with a $macron(v)$ axis, PCSA, GA and
  DMGA, and `us_mpi_analysis` on a cluster; comparisons of distributions, not only of RMSD;
+ regression tests for ill-conditioned inputs: identical and nearly identical columns,
  single-precision columns, columns that vanish after noise elimination;
+ separate, individually reviewed changes for the scheduler hang, the NNLS round-off loop,
  the Tikhonov stride and the merge order, rather than one combined change.

Only once the corrected solver is the default does it make sense to decide whether Stages 2
and 3 are worth their cost, because the benchmark shows that the answer depends on the solver.

= Limitations <sec:limitations>

The simulated data were generated and fitted with the same ASTFEM code, with white Gaussian
random noise and noise models that the fit represents exactly; there is no model error. The
experimental dataset shows that real data are not like this, and effects of a few percent
seen here need not survive on such data. One experimental design, one noise level, seven
mixtures, one grid size with 64 subgrids for the main comparison and one thread count were
examined, and the merge tree
depends on the thread count. The general sublattice rule was tested where it replaces
rectangles in four cases only, one of each kind (@sec:general); other grid shapes, subgrid
counts and unions, and custom grids with other axes, were not tested. The accuracy reference is the optimum of the $64 times 64$ grid,
which other grids can undercut. The fits used the `us_2dsa` processing classes from a
headless driver written for this study; the driver is itself new, unreviewed code, the
graphical program was exercised only through the experimental-data runs, and
`us_mpi_analysis` was compiled but not run. Only one experimental dataset was examined.
GA, PCSA and DMGA, which share the solver, were not benchmarked.

= Conclusions

The degenerate subgrids of custom-grid analyses are as harmful in fits as their coverage
suggested when they are columns, less so when they are diagonals, and Stage 1 should be
implemented: its general rule removed them in every case tested and did no harm elsewhere. Beyond that, the benchmark does not support the
expectation that subgrids and grids shaped by the FEM metric will make single-pass 2DSA results
materially more accurate. Under the production solver their gains were inconsistent and
dominated by a solver error common to all configurations; under a corrected solver the
classic construction is already close to exact, and the proposal's constructions improve on
it consistently but marginally in a single pass, and save about a quarter of the
computation when fits are iterated. The reduction in cost from smaller placed
grids is the part of the proposal that survives, and it is a larger undertaking than the
proposal suggests. The most consequential finding is not about subgrids at all: fits with TI
and RI noise have been computing concentrations with an objective other than least squares.
Correcting that is necessary and should be done with the care that a change to every noise
fit in UltraScan warrants; the failure of its first implementation on experimental data is a
measure of how much care that is.

#heading(numbering: none)[Notes]

*Use of AI tools.* The benchmark driver, code changes, numerical experiments, figures and
draft text of this report were prepared with the assistance of Claude (Anthropic).

*Code and data.* The benchmark driver, its scripts and this report are on branch
`claude/proposal-review-y6ue50`; the solver changes are on the branches listed in Appendix A.
Simulated datasets and fit results are available on request.

#heading(numbering: none)[Appendix A: Branches and code locations]

Line numbers refer to the main branch of UltraScan III at commit `186d57120`
(September 24, 2026), as in @coverage2026.

#figure(
  rules-table(
    columns: 2,
    align: (left, left),
    header: ([Location], [Role]),
    [`utils/us_solve_sim.cpp:1248--1265`], [TI+RI elimination and NNLS on the normal equations (D1, D2)],
    [`utils/us_solve_sim.cpp:1318`], [NNLS on the normal equations, RI only (D2)],
    [`utils/us_solve_sim.cpp:601, 874, 1067`], [Tikhonov rows appended to the design matrix],
    [`utils/us_math2.cpp:1155`], [NNLS round-off feasibility loop],
    [`programs/us_2dsa/us_2dsa_process.cpp:1185`], [maximum depth reset when subgrids complete],
    [`programs/us_2dsa/us_2dsa_process.cpp:440`], [final fit input (duplicates)],
    [`programs/us_mpi_analysis/2dsa_master.cpp:1116, 1179, 1218`], [merge jobs (duplicates)],
  ),
  kind: table,
  caption: [Code locations of the defects discussed.],
)

#figure(
  rules-table(
    columns: 3,
    align: (left, left, left),
    header: ([Branch], [Commit], [Change]),
    [`claude/keen-maxwell-i5daw2`], [`4f1feaab3`], [debug options `2DSA-OrderedMerge`, `2DSA-MergePool`],
    [], [`0859b9037`], [debug option `2DSA-SimCache`],
    [], [`2e2c6cde9`], [debug option `SolveSim-ExactNoise`, duplicate removal],
    [], [`8a8e0b219`], [robust factorization; option matching],
    [`claude/nnls-us-solve-sim-review-eufblk`], [`b63bfd079`], [projected-matrix NNLS; NNLS round-off loop; Tikhonov rows],
    [], [`8b24aad1f`], [scheduler: maximum depth not lowered],
    [], [`f3bf57d95`], [duplicate removal (`us_2dsa`, `us_mpi_analysis`)],
    [`claude/proposal-review-y6ue50`], [`661f25c13`], [benchmark driver `us_2dsa_bench` and scripts],
  ),
  kind: table,
  caption: [Branches and commits referred to in this report. None has been merged into `main`.],
)

#bibliography("refs.yml", style: "ieee", title: [References])
