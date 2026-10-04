"""Solute systems simulated by the benchmark.

Each system is a list of ideal, non-interacting solutes given by their
sedimentation coefficient s (Svedberg, s20,w), frictional ratio f/f0 and
fraction of the total signal concentration.  All solutes share the partial
specific volume VBAR (2DSA keeps vbar fixed), and the total loading
concentration (OD) is set by the simulation settings of the design.
"""

VBAR = 0.72

SYSTEMS = {
    # --- the six systems of the original request ---------------------------
    "S01_single": {
        "description": "single solute (BSA-like monomer)",
        "solutes": [(4.4, 1.25, 1.0)],
    },
    "S02_far_apart": {
        "description": "two solutes far apart in s",
        "solutes": [(2.0, 1.3, 0.5), (20.0, 1.5, 0.5)],
    },
    "S03_same_s_diff_ff0": {
        "description": "same s, different f/f0",
        "solutes": [(5.0, 1.2, 0.5), (5.0, 2.4, 0.5)],
    },
    "S04_same_ff0_diff_s": {
        "description": "same f/f0, close s",
        "solutes": [(4.0, 1.4, 0.5), (6.0, 1.4, 0.5)],
    },
    "S05_very_small_s": {
        "description": "very small s (peptide)",
        "solutes": [(0.3, 1.2, 1.0)],
    },
    "S06_very_large_s": {
        "description": "very large s (ribosome / small virus)",
        "solutes": [(150.0, 1.1, 1.0)],
    },
    # --- multi-solute systems (3 to 6 solutes) -----------------------------
    "S07_three_ladder": {
        "description": "3 globular solutes, moderate spacing",
        "solutes": [(2.5, 1.2, 0.3), (4.5, 1.3, 0.4), (7.0, 1.4, 0.3)],
    },
    "S08_four_oligomers": {
        "description": "4 oligomers (monomer..tetramer), slowly increasing f/f0",
        "solutes": [(4.3, 1.25, 0.4), (6.4, 1.30, 0.3),
                    (8.4, 1.35, 0.2), (10.2, 1.40, 0.1)],
    },
    "S09_five_wide": {
        "description": "5 solutes spanning 1.6 decades of s",
        "solutes": [(1.0, 1.2, 0.2), (3.0, 1.4, 0.2), (8.0, 1.6, 0.2),
                    (20.0, 1.8, 0.2), (40.0, 2.0, 0.2)],
    },
    "S10_six_mixed_shapes": {
        "description": "6 solutes, pairs of equal s with different shape",
        "solutes": [(2.0, 1.2, 0.2), (2.0, 2.5, 0.15), (6.0, 1.3, 0.2),
                    (6.0, 3.0, 0.15), (15.0, 1.5, 0.15), (40.0, 1.8, 0.15)],
    },
    "S11_three_large": {
        "description": "3 large particles (assemblies / small viruses)",
        "solutes": [(30.0, 1.3, 0.3), (80.0, 1.2, 0.4), (200.0, 1.1, 0.3)],
    },
    "S12_four_small": {
        "description": "4 small solutes (peptides / small proteins)",
        "solutes": [(0.5, 1.2, 0.25), (1.0, 1.3, 0.25), (1.8, 1.4, 0.25),
                    (3.0, 1.5, 0.25)],
    },
    "S13_trace_aggregate": {
        "description": "main species with 3 low-abundance aggregates",
        "solutes": [(4.4, 1.25, 0.85), (6.6, 1.35, 0.08),
                    (9.0, 1.45, 0.04), (12.0, 1.6, 0.03)],
    },
}


def solutes(system_id):
    """Return the solutes of a system as a list of dicts (s in S)."""
    rows = SYSTEMS[system_id]["solutes"]
    total = sum(r[2] for r in rows)
    return [{"s": s, "ff0": k, "fraction": f / total, "vbar": VBAR}
            for s, k, f in rows]
