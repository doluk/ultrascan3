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

## Example sweep (shell)

```sh
for N in 50 100 200 400 800; do
  ./astfvm_convergence_driver --outdir out --tag "N_$N" \
     --N "$N" --refine 0 --uniform 1 --steps-per-transit 200
done
for K in 50 100 200 400 800; do
  ./astfvm_convergence_driver --outdir out --tag "dt_$K" \
     --N 400 --refine 0 --uniform 1 --steps-per-transit "$K"
done
```
