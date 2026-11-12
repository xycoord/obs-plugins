"""Score the Invert filter on the suite.

    python3 measure.py --plugin obs-invert.so --data DATA_DIR --out OUT [--settings JSON] [--label NAME]

Runs the real shader through the headless libobs harness on every suite
image (default settings + transparent background unless --settings), then
composites the output over black, over a colourful 'visuals' backdrop and
over white, and compares with the ideal. Error per pixel: largest channel
difference, 0..255. Per region category: mean over that category's pixels in
all images of a set, averaged over the three backdrops. Headline: mean over
categories. Writes OUT/score.json and prints a table.
"""
import argparse, json, os, subprocess, sys
import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
# The headless harness and the libobs it runs on come from obs-shaderfilter's
# tests (plugins/obs-shaderfilter/tests/run.sh setup builds both).
WORK = os.environ.get("WORK", os.path.expanduser("~/.cache/obs-shaderfilter-tests"))
OBS_PREFIX = os.environ.get("OBS_PREFIX", f"{WORK}/libobs/prefix")
HARNESS = os.environ.get("HARNESS", f"{WORK}/harness-build/harness")
SUITE = os.environ.get("SUITE", f"{HERE}/suite")
CATS = ["background", "edge", "interior", "dark_interior", "seam", "shadow", "thin"]
W, H = 640, 360


def backdrops():
    y, x = np.mgrid[0:H, 0:W] / np.array([H, W])[:, None, None]
    visuals = np.stack([0.25 + 0.35 * np.sin(6 * x + 2 * y) ** 2, 0.1 + 0.3 * y, 0.35 + 0.4 * np.cos(5 * y - 3 * x) ** 2], -1)
    return {"black": np.zeros((H, W, 3)), "visuals": visuals, "white": np.ones((H, W, 3))}


def run(plugin, data, out, settings):
    index = json.load(open(f"{SUITE}/index.json"))
    spec = {"width": W, "height": H, "images": {e["name"]: f"{SUITE}/{e['name']}.png" for e in index},
            "cases": [{"name": e["name"], "source": e["name"], "filters": [{"id": "grove_invert_filter", "settings": settings}],
                       "steps": [{"dt": 0.05, "capture": True}]} for e in index]}
    os.makedirs(out, exist_ok=True)
    json.dump(spec, open(f"{out}/spec.json", "w"))
    env = dict(os.environ, DISPLAY=os.environ.get("DISPLAY", ":99"), LD_LIBRARY_PATH=f"{OBS_PREFIX}/lib")
    r = subprocess.run([HARNESS, "--obs-prefix", OBS_PREFIX, "--plugin", plugin, "--data", data,
                        "--spec", f"{out}/spec.json", "--out", f"{out}/render"], env=env, capture_output=True, text=True)
    errs = [json.loads(l) for l in open(f"{out}/render/results.jsonl")] if os.path.exists(f"{out}/render/results.jsonl") else []
    bad = [e for e in errs if e.get("error")]
    if "Error compiling" in r.stderr or "failed to compile" in r.stderr:
        sys.exit("shader failed to compile:\n" + "\n".join(l for l in r.stderr.splitlines() if "error" in l.lower())[:2000])
    if r.returncode != 0 or bad or len(errs) != len(index):
        sys.exit(f"harness failed: rc={r.returncode} {bad[:2]} {r.stderr[-800:]}")
    return index


def score(out, index):
    bds = backdrops()
    acc = {}  # (set, cat) -> [sum, count] per backdrop
    per_variant = {}
    per_image = {}
    for e in index:
        g = np.load(f"{SUITE}/{e['name']}.npz")
        o = np.asarray(Image.open(f"{out}/render/{e['name']}_0.png").convert("RGBA")).astype(np.float32) / 255
        gt_c, gt_a = g["gt_c"], g["gt_a"]
        errs = []
        for bn, b in bds.items():
            ideal = gt_c + b * (1 - gt_a[..., None])
            got = o[..., :3] + b * (1 - o[..., 3:])
            errs.append(np.abs(got - ideal).max(-1) * 255)
        err = np.mean(errs, 0)
        s = "holdout" if e["holdout"] else "train"
        img_cats = {}
        for c in CATS:
            m = g[c]
            if m.sum() == 0:
                continue
            a = acc.setdefault((s, c), [0.0, 0])
            a[0] += float(err[m].sum()); a[1] += int(m.sum())
            v = per_variant.setdefault((s, e["variant"], c), [0.0, 0])
            v[0] += float(err[m].sum()); v[1] += int(m.sum())
            img_cats[c] = float(err[m].mean())
        per_image[e["name"]] = img_cats
    res = {}
    for s in ["train", "holdout"]:
        cats = {c: acc[(s, c)][0] / acc[(s, c)][1] for c in CATS if (s, c) in acc}
        res[s] = {"headline": float(np.mean(list(cats.values()))), "cats": cats}
    variants = sorted({k[1] for k in per_variant})
    res["variants"] = {v: {c: per_variant[("train", v, c)][0] / per_variant[("train", v, c)][1] for c in CATS if ("train", v, c) in per_variant} for v in variants}
    res["images"] = per_image
    return res


def show(res, label):
    print(f"== {label}")
    for s in ["train", "holdout"]:
        print(f"  {s:8s} headline {res[s]['headline']:6.2f} | " + " ".join(f"{c} {v:5.1f}" for c, v in res[s]["cats"].items()))
    for v, cats in res["variants"].items():
        print(f"    {v:12s} " + " ".join(f"{c} {x:5.1f}" for c, x in cats.items()))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True); ap.add_argument("--data", required=True); ap.add_argument("--out", required=True)
    ap.add_argument("--settings", default='{"transparent": true}'); ap.add_argument("--label", default="")
    a = ap.parse_args()
    index = run(a.plugin, a.data, a.out, json.loads(a.settings))
    res = score(a.out, index)
    json.dump(res, open(f"{a.out}/score.json", "w"), indent=1)
    show(res, a.label or a.out)
