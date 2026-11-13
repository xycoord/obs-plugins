"""Check the Invert filter against the reference in oklab.py, and compare inversions by contrast.

    python3 check.py --plugin obs-invert.so --data DATA [--out OUT]

Renders a sheet of colour swatches and an image with its own transparency (anti-aliased shapes,
an alpha ramp) through OBS's real renderer (the headless harness from obs-shaderfilter's tests),
and compares with the reference: it fails if any pixel is off by more
than 2/255. Then prints how well each inversion keeps the contrast between pairs of colours.

Setup: build the harness and its libobs once with obs-shaderfilter's `tests/run.sh setup`, then
point WORK at its work folder (default ~/.cache/obs-shaderfilter-tests), or set OBS_PREFIX and
HARNESS. Run under X (xvfb-run -a).
"""
import argparse, itertools, json, os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageDraw
from oklab import tone_invert, ok_invert, hsl_invert, to_lin, LUM, lstar

WORK = os.environ.get("WORK", os.path.expanduser("~/.cache/obs-shaderfilter-tests"))
OBS_PREFIX = os.environ.get("OBS_PREFIX", f"{WORK}/libobs/prefix")
HARNESS = os.environ.get("HARNESS", f"{WORK}/harness-build/harness")
W, H = 640, 360
PALETTE = [(0.80, 0.10, 0.15), (1, 0, 0), (0.95, 0.55, 0.10), (1, 0.85, 0), (0.10, 0.60, 0.25), (0.10, 0.75, 0.85),
           (0.10, 0.30, 0.80), (0, 0, 1), (0.50, 0.20, 0.60), (0.08, 0.12, 0.35), (0.35, 0.05, 0.08), (0.04, 0.04, 0.04),
           (0.30, 0.30, 0.30), (0.55, 0.55, 0.55), (0.80, 0.80, 0.80), (0.98, 0.80, 0.85), (0.98, 0.92, 0.60),
           (0.80, 0.95, 0.85), (0.85, 0.82, 0.98), (0.75, 0.88, 0.98), (1, 1, 1)]


def images(out):
    sw = np.ones((H, W, 3))
    for i, c in enumerate(PALETTE):
        x, y = 12 + (i % 7) * 90, 12 + (i // 7) * 115
        sw[y:y + 100, x:x + 78] = c
    Image.fromarray((sw * 255 + 0.5).astype(np.uint8)).save(f"{out}/swatches.png")
    big = Image.new("RGBA", (W * 4, H * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    for i, c in enumerate(PALETTE[:12]):
        x, y = 40 + (i % 6) * 400, 80 + (i // 6) * 560
        d.ellipse([x, y, x + 320, y + 320], fill=tuple(int(v * 255) for v in c) + (255,))
    im = big.resize((W, H), Image.LANCZOS)
    a = np.asarray(im).astype(float)
    a[300:, :, :3] = [40, 90, 200]
    a[300:, :, 3] = np.linspace(0, 255, W)[None, :]
    Image.fromarray(a.astype(np.uint8), "RGBA").save(f"{out}/transparent.png")


def render(plugin, data, out):
    cases = [(img, img, {}) for img in ("swatches", "transparent")]
    spec = {"width": W, "height": H, "images": {i: f"{out}/{i}.png" for i in ("swatches", "transparent")},
            "cases": [{"name": n, "source": s, "filters": [{"id": "grove_invert_filter", "settings": st}], "steps": [{"dt": 0.05, "capture": True}]} for n, s, st in cases]}
    json.dump(spec, open(f"{out}/spec.json", "w"))
    env = dict(os.environ, DISPLAY=os.environ.get("DISPLAY", ":99"), LD_LIBRARY_PATH=f"{OBS_PREFIX}/lib")
    r = subprocess.run([HARNESS, "--obs-prefix", OBS_PREFIX, "--plugin", plugin, "--data", data, "--spec", f"{out}/spec.json", "--out", f"{out}/render"],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0 or "Error compiling" in r.stderr:
        sys.exit(f"harness failed:\n{r.stderr[-1500:]}")


def compare(out):
    ok = True
    for img in ("swatches", "transparent"):
        src = np.asarray(Image.open(f"{out}/{img}.png").convert("RGBA")).astype(float) / 255
        a = src[..., 3:]
        got = np.asarray(Image.open(f"{out}/render/{img}_0.png").convert("RGBA")).astype(float) / 255
        want = a * tone_invert(src[..., :3])  # the harness captures premultiplied colour
        err = max(np.abs(got[..., :3] - want).max(), np.abs(got[..., 3:] - a).max()) * 255
        ok &= err <= 2.0
        print(f"  {img:12s} largest difference {err:4.1f}/255  {'ok' if err <= 2.0 else 'FAIL'}")
    return ok


def contrast():
    Y = lambda c: float(to_lin(np.asarray(c, float)) @ LUM)
    T = lambda c: float(lstar(Y(c))) * 100
    ratio = lambda x, y: (max(Y(x), Y(y)) + 0.05) / (min(Y(x), Y(y)) + 0.05)
    pairs = list(itertools.combinations(PALETTE, 2))
    clear = [(x, y) for x, y in pairs if abs(T(x) - T(y)) > 25]
    print(f"\n  Contrast kept, over {len(pairs)} pairs of the palette (0 is perfect):")
    print(f"  {'':16s}{'tone change (L*)':>18s}{'WCAG ratio change (log)':>25s}{'clear pairs losing half':>25s}")
    for name, f in (("HSL lightness", hsl_invert), ("OKLab lightness", ok_invert), ("tone (the filter)", tone_invert)):
        g = lambda c: f(np.asarray(c, float))
        dt = np.mean([abs(abs(T(g(x)) - T(g(y))) - abs(T(x) - T(y))) for x, y in pairs])
        dr = np.mean([abs(np.log(ratio(g(x), g(y)) / ratio(x, y))) for x, y in pairs])
        lose = sum(abs(T(g(x)) - T(g(y))) < 0.5 * abs(T(x) - T(y)) for x, y in clear)
        print(f"  {name:16s}{dt:18.1f}{dr:25.3f}{f'{lose} of {len(clear)}':>25s}")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--plugin", required=True); ap.add_argument("--data", required=True); ap.add_argument("--out")
    a = ap.parse_args()
    out = a.out or tempfile.mkdtemp(prefix="invert-check-")
    os.makedirs(out, exist_ok=True)
    images(out)
    render(a.plugin, a.data, out)
    print("Invert against the reference:")
    ok = compare(out)
    contrast()
    sys.exit(0 if ok else 1)
