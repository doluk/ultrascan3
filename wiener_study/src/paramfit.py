"""Coordinate-descent fit of k_S,red, meniscus and bottom for the mean-field
hindered-settling + Wiener-skew model (UltraScan ASTFVM prototype).
Each evaluation is a warm-started mean-field fit (field of the current best point)."""
import json, os, subprocess, sys, threading, time
import numpy as np

S = os.path.dirname(os.path.abspath(__file__))
EXE = f"{S}/wsim2/b/wsim"
OUT = f"{S}/pfit"
os.makedirs(OUT, exist_ok=True)
SLOTS = threading.Semaphore(4)
LOCK = threading.Lock()
ENV = dict(os.environ, QT_QPA_PLATFORM="offscreen", HOME=f"{S}/home", WTHREADS="1", WQUIET="1")

def read_cfg(fn):
    d = {}
    for l in open(fn):
        l = l.strip()
        if "=" in l and not l.startswith("#"):
            k, v = l.split("=", 1); d[k.strip()] = v.strip()
    return d

def run(base, tag, over):
    """one mean-field fit; returns (rmsd of last iteration, json path)"""
    cfg = dict(base); cfg.update(over); cfg["tag"] = tag
    fn = f"{OUT}/{tag}.cfg"
    with open(fn, "w") as f:
        f.write("\n".join(f"{k}={v}" for k, v in cfg.items()) + "\n")
    with SLOTS:
        t0 = time.time()
        with open(f"{OUT}/{tag}.log", "w") as lg:
            subprocess.run([EXE, OUT, "REAL", fn], env=ENV, stdout=lg, stderr=subprocess.STDOUT)
    J = json.load(open(f"{OUT}/{cfg['id']}_{tag}_real.json"))
    last = max(x["iter"] for x in J["fits"])
    r = [x for x in J["fits"] if x["iter"] == last][0]["rmsd"]
    with LOCK:
        print(f"[{time.strftime('%H:%M:%S')}] {tag}: " + " ".join(f"{k}={over[k]}" for k in ("ksred", "meniscus", "bottom") if k in over)
              + f" -> rmsd {r:.5f} ({(time.time()-t0)/60:.1f} min)", flush=True)
    return r, f"{OUT}/{cfg['id']}_{tag}_field.bin"

def parabola_min(xs, ys, lo, hi):
    a, b, c = np.polyfit(xs, ys, 2)
    if a <= 0:
        return xs[int(np.argmin(ys))]
    return float(np.clip(-b / (2 * a), lo, hi))

def chain(name, cfgfile, steps):
    base = read_cfg(cfgfile)
    base.update({"mfit": "6", "damp": "0.5"})
    cur = {"ksred": float(base["ksred"]), "meniscus": float(base["meniscus"]), "bottom": float(base["bottom"])}
    hist = []
    fmt = lambda p: {k: f"{v:.6g}" for k, v in p.items()}
    best_r, best_field = run(base, f"{name}_base", fmt(cur))
    hist.append(dict(cur, rmsd=best_r, tag="base"))
    warm = {"mfit": "2", "damp": "0.5"}
    for si, (par, deltas, rel) in enumerate(steps):
        pts = []
        res = [None] * len(deltas)
        def one(i, d):
            p = dict(cur); p[par] = cur[par] * (1 + d) if rel else cur[par] + d
            over = fmt(p); over.update(warm); over["field_in"] = best_field
            r, fld = run(base, f"{name}_s{si}_{par}_{i}", over)
            res[i] = (p[par], r, fld)
        th = [threading.Thread(target=one, args=(i, d)) for i, d in enumerate(deltas)]
        [t.start() for t in th]; [t.join() for t in th]
        xs = [cur[par]] + [x for x, _, _ in res]; ys = [best_r] + [y for _, y, _ in res]
        flds = [best_field] + [f for _, _, f in res]
        for x, y in zip(xs[1:], ys[1:]):
            hist.append(dict(cur, **{par: x}, rmsd=y, tag=f"s{si}"))
        lo, hi = min(xs), max(xs)
        xv = parabola_min(np.array(xs), np.array(ys), lo - 0.5 * (hi - lo), hi + 0.5 * (hi - lo))
        p = dict(cur); p[par] = xv
        over = fmt(p); over.update(warm); over["field_in"] = flds[int(np.argmin(ys))]
        rv, fv = run(base, f"{name}_s{si}_{par}_v", over)
        hist.append(dict(p, rmsd=rv, tag=f"s{si}v"))
        cand = list(zip(xs, ys, flds)) + [(xv, rv, fv)]
        x_b, r_b, f_b = min(cand, key=lambda c: c[1])
        cur[par] = x_b; best_r = r_b; best_field = f_b
        with LOCK:
            print(f"== {name}: best after {par} scan: " + ", ".join(f"{k} {v:.5g}" for k, v in cur.items()) + f"; rmsd {best_r:.5f}", flush=True)
    over = fmt(cur); over.update({"mfit": "4", "damp": "0.5", "field_in": best_field})
    r, _ = run(base, f"{name}_final", over)
    hist.append(dict(cur, rmsd=r, tag="final"))
    json.dump(hist, open(f"{OUT}/{name}_history.json", "w"), indent=1)
    with LOCK:
        print(f"#### {name} final: " + ", ".join(f"{k} {v:.5g}" for k, v in cur.items()) + f"; rmsd {r:.5f}", flush=True)

if __name__ == "__main__":
    STEPS = [("ksred", [-0.10, 0.10], True), ("meniscus", [-0.002, 0.002], False),
             ("bottom", [-0.05, 0.05], False), ("ksred", [-0.05, 0.05], True)]
    sets = {"igg10": f"{S}/mf/igg10.cfg", "igg46": f"{S}/mf/v3/igg46.cfg", "bsa140": f"{S}/mf/v3/bsa140.cfg"}
    only = sys.argv[1:] or list(sets)
    th = [threading.Thread(target=chain, args=(n, sets[n], STEPS)) for n in only]
    [t.start() for t in th]; [t.join() for t in th]
