"""Tuned score: for each image, the best combination of the per-image controls.

    python3 tuned.py --plugin obs-invert.so --data DATA --out OUT

What a tech could reach by adjusting the controls for each logo. Renders the
suite once for every combination in GRID (a JSON object in the GRID environment
variable replaces it; renders are kept in OUT and reused), then for each image
takes the combination with the lowest error (mean over its region kinds) and
scores those choices as measure.py scores one setting. Prints the default and
tuned scores and writes OUT/tuned.json with each image's choice. About 20 s per
combination.
"""
import argparse, itertools, json, os
import numpy as np
from PIL import Image
import measure

# The first combination must be the defaults: it gives the default score.
GRID = {"clean_up": [8.0, 12.0, 18.0], "thin_details": ["auto", "solid", "faint"],
        "edge_softness": [1.0, 1.5, 2.5], "grey_shadows": [True, False]}
if os.environ.get("GRID"):
    GRID = json.loads(os.environ["GRID"])


def combos():
    keys = list(GRID)
    for vals in itertools.product(*GRID.values()):
        yield dict(zip(keys, vals))


def errmap(out, e, bds):
    g = np.load(f"{measure.SUITE}/{e['name']}.npz")
    o = np.asarray(Image.open(f"{out}/render/{e['name']}_0.png").convert("RGBA")).astype(np.float32) / 255
    errs = [np.abs(o[..., :3] + b * (1 - o[..., 3:]) - (g["gt_c"] + b * (1 - g["gt_a"][..., None]))).max(-1) * 255 for b in bds.values()]
    return g, np.mean(errs, 0)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True); ap.add_argument("--data", required=True); ap.add_argument("--out", required=True)
    a = ap.parse_args()
    index = json.load(open(f"{measure.SUITE}/index.json"))
    runs = []
    for i, s in enumerate(combos()):
        d = f"{a.out}/c{i:02d}"
        if not os.path.exists(f"{d}/render/results.jsonl"):
            measure.run(a.plugin, a.data, d, dict(s, transparent=True))
        runs.append((s, d))
    bds = measure.backdrops()
    acc, acc0, choice = {}, {}, {}
    for e in index:
        best = None
        for s, d in runs:
            g, err = errmap(d, e, bds)
            cats = {c: (float(err[g[c]].sum()), int(g[c].sum())) for c in measure.CATS if g[c].sum()}
            h = np.mean([v[0] / v[1] for v in cats.values()])
            if best is None or h < best[0] - 1e-9:
                best = (h, s, cats)
            if s == runs[0][0]:
                default = (h, cats)
        st = "holdout" if e["holdout"] else "train"
        choice[e["name"]] = {"settings": best[1], "tuned": best[0], "default": default[0]}
        for tgt, cats in ((acc, best[2]), (acc0, default[1])):
            for c, (sm, n) in cats.items():
                v = tgt.setdefault((st, c), [0.0, 0]); v[0] += sm; v[1] += n
    res = {}
    for st in ["train", "holdout"]:
        for name, tgt in (("default", acc0), ("tuned", acc)):
            cats = {c: tgt[(st, c)][0] / tgt[(st, c)][1] for c in measure.CATS if (st, c) in tgt}
            res[f"{st}_{name}"] = {"headline": float(np.mean(list(cats.values()))), "cats": cats}
            print(f"{st:8s} {name:8s} {res[f'{st}_{name}']['headline']:6.2f} | " + " ".join(f"{c} {v:5.1f}" for c, v in cats.items()))
    res["choice"] = choice
    json.dump(res, open(f"{a.out}/tuned.json", "w"), indent=1)
