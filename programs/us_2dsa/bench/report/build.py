"""Compile subgrid_benchmark.typ to PDF (Typst 0.15, via the typst Python package).

usage: python3 build.py [output.pdf]   (default: subgrid_benchmark.pdf beside this script)
"""
import os, sys, typst

here = os.path.dirname(os.path.abspath(__file__))
out = os.path.abspath(sys.argv[1]) if len(sys.argv) > 1 else \
      os.path.join(here, 'subgrid_benchmark.pdf')
pdf, warnings = typst.compile_with_warnings(os.path.join(here, 'subgrid_benchmark.typ'),
                                            root=here, format='pdf')
open(out, 'wb').write(pdf)
for w in warnings:
    print('warning:', w)
print(out, len(pdf), 'bytes')
