# ASTFVM convergence driver

A minimal headless (no GUI, no database) console program that runs a single
`US_LammAstfvm` simulation at a controlled mesh resolution / time step and
exports its per-step solution trace via the new `setSolutionTrace()` API
(see the Chapter 4 ASTFVM instrumentation spec). Intended to be invoked
once per point on an `(N, dt)` sweep by an external script; the companion
`us3utils` Python package (`../us3utils`) parses the resulting CSVs and
renders the convergence and mesh-tracking figures.

## Build

Not built by default. Opt in with:

```sh
cmake -S . -B build -DBUILD_ASTFVM_CONVERGENCE_DRIVER=ON
cmake --build build --target astfvm_convergence_driver
```

## Usage

```sh
./astfvm_convergence_driver \
   --outdir /tmp/astfvm_traces --tag myo_N100_k100 \
   --N 100 --steps-per-transit 100 --refine 0 --uniform 1
```

Run `--help` for the full option list (mesh speed, error tolerance,
band-forming vs. full-column loading, rpm/meniscus/bottom, s/D/vbar, etc.).
Each invocation writes `<outdir>/<tag>_c0_trace_steps.csv` and
`<outdir>/<tag>_c0_trace_nodes.csv`.

## Open items (confirm before using for publication figures)

The default myoglobin parameters (`s=2.13e-13`, `D=1.08e-6 cm^2/s`,
`vbar=0.741`, `f/f0=1.17`) come from the paper's Table 1. The rotor speed
(50000 rpm) and cell geometry (meniscus 5.8 cm, bottom 7.2 cm) are
reasonable SV defaults but are **not** taken from the paper -- pass
`--rpm`, `--meniscus`, `--bottom` explicitly once the real run metadata is
confirmed (see spec Sec. 9).

## Matching a published Fig-2-style figure (mesh density / error / grids vs. time)

Two knobs needed for this weren't exposed until recently, and are easy to
miss:

- `--err-tol` (already existed) and `--mon-cutoff` (new) together set the
  adaptive-refinement recipe: `--err-tol 1e-4 --mon-cutoff 500` matches a
  `Tol=1e-4`, mesh-density-level-off-at-500 recipe (the solver's internal
  default if `--mon-cutoff` is never passed is 1000, not 500).
- `--band-forming 1 --band-volume <mL>` loads a narrow band instead of a
  full column. **The band-forming initial condition only tests mesh
  *vertices* against the band bounds** (element midpoints are just
  vertex averages) -- if the band is narrower than the *initial* coarse
  mesh's element spacing, it can fall entirely between two vertices and
  silently load **zero concentration everywhere** (mass stays 0 for the
  whole run). The default `--band-volume 0.06` gives a band comfortably
  wider than the ~0.14cm initial spacing at `--N 10` over the default
  meniscus/bottom; widen it further (or raise `--N`) if you go coarser or
  change `--meniscus`/`--bottom`.

Even with both of those set, don't expect an exact numeric match to a
paper figure without also matching its physical setup: this driver's
defaults are a full myoglobin-scale AUC cell (meniscus 5.8 - bottom 7.2
cm), and a **full-column load keeps sharpening a boundary layer against
the cell bottom for as long as the run continues** -- the number of grids
grows essentially without bound over a multi-hour run, unlike a bounded
mesh-count trace. If your reference figure shows the element count
leveling off (not growing indefinitely), it's very likely evaluated over
a much shorter time window, a smaller domain, or a band-forming load
where the band reaches a quasi-steady width -- match `--run-hours`,
`--meniscus`/`--bottom`, and `--band-forming`/`--band-volume` to that
setup, not just `--err-tol`/`--mon-cutoff`/the three dt values. Also note
`us3utils`'s error figures compare against another run from the *same*
tool (self-convergence, see `../us3utils/README.md`), not against an
independent method -- if the reference figure compares to a different
solver entirely, exact numeric agreement isn't expected, only the same
qualitative trends (error drops sharply for the first dt reduction, then
levels off; mesh clusters where the gradient is sharp and follows it).

## Tag convention for sweeps

`us3utils` groups runs by tag: `N_<value>[_<series>]` for the spatial
sweep, `dt_<value>[_<series>]` for the temporal sweep. The optional
`_<series>` suffix (e.g. `R0_U1` for refine=0/uniform=1) lets you vary a
mesh configuration alongside N or dt and have each configuration plotted
as its own line instead of being mixed together -- see
`../us3utils/README.md` for how the plotting side groups these.

**Prefer `--fixed-dt` over `--steps-per-transit` for a dt sweep.** The
solver has a pre-existing safety clamp that caps `dt` to the output scan
spacing; with few scans it can silently override every
`--steps-per-transit` value to the *same* clamped `dt`, making the sweep
a no-op. `--fixed-dt` bypasses that clamp and is honored exactly (raise
`--nscans` if you want to use `--steps-per-transit` instead, so the
clamp doesn't engage).

## Example sweep (bash)

```sh
for N in 50 100 200 400 800; do
  for series in R0_U1 R1_U1 R1_U0; do
    IFS=_ read -r r refine u uniform <<< "$series"
    ./astfvm_convergence_driver --outdir out --tag "N_${N}_${series}" \
       --N "$N" --refine "${refine#R}" --uniform "${uniform#U}" --fixed-dt 60
  done
done
for dt in 1 2 3 5 7 10 15 20 30 40 50 75 100 125 150; do
  ./astfvm_convergence_driver --outdir out --tag "dt_${dt}_R0_U1" \
     --N 200 --refine 0 --uniform 1 --fixed-dt "$dt"
  ./astfvm_convergence_driver --outdir out --tag "dt_${dt}_R1_U0" \
     --N 200 --refine 1 --uniform 0 --fixed-dt "$dt"
done
```

## Example sweep (PowerShell)

```powershell
foreach ($N in 20..1000 | Where-Object { $_ % 20 -eq 0 }) {
  .\astfvm_convergence_driver.exe --outdir sweep --tag "N_${N}_R0_U1" --N "$N" --refine 0 --uniform 1 --fixed-dt 60 --nscans 50 --npoints 200 --run-hours 1.0
  .\astfvm_convergence_driver.exe --outdir sweep --tag "N_${N}_R1_U1" --N "$N" --refine 1 --uniform 1 --fixed-dt 60 --nscans 50 --npoints 200 --run-hours 1.0
  .\astfvm_convergence_driver.exe --outdir sweep --tag "N_${N}_R1_U0" --N "$N" --refine 1 --uniform 0 --fixed-dt 60 --nscans 50 --npoints 200 --run-hours 1.0
}
foreach ($dt in "1","2","3","5","7","10","15","20","30","40","50","75","100","125","150") {
  .\astfvm_convergence_driver.exe --outdir sweep --tag "dt_${dt}_R0_U1" --N 200 --refine 0 --uniform 1 --fixed-dt "$dt" --nscans 50 --npoints 200 --run-hours 1.0
  .\astfvm_convergence_driver.exe --outdir sweep --tag "dt_${dt}_R1_U0" --N 200 --refine 1 --uniform 0 --fixed-dt "$dt" --nscans 50 --npoints 200 --run-hours 1.0
}
```
