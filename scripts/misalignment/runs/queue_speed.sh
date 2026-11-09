#!/bin/bash
# speed / column-length series, sector geometry, BSA 0.1 mg/mL; equal omega^2 t
cd "$(dirname "$0")/.."
run() { python3 misalign2d_sector.py "$@" > runs/$N.log 2>&1; }
for rpm in 20000 35000 60000; do
  te=$(python3 -c "print(round(4000*(50000/$rpm)**2))"); dt=$(python3 -c "print(min(20, 5*(50000/$rpm)**2))")
  for d in 0 1; do N=v_r${rpm}_d$d; run --delta $d --c0 0.1 --rpm $rpm --tend $te --dt $dt --out runs/$N.npz & done
  wait
done
for L in 0.45 1.2; do
  for d in 0 1; do N=v_L${L}_d$d; run --delta $d --c0 0.1 --L $L --out runs/$N.npz & done
  wait
done
echo SPEED DONE
