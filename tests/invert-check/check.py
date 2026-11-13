"""Check the Invert filter against the reference in oklab.py, and compare inversions by contrast.

    python3 check.py --plugin obs-invert.so --data DATA [--out OUT]

1. The reference itself, over every colour of a 33 x 33 x 33 grid of sRGB: each colour's tone must
   land on 100 - L* and its hue stay put (where both it and its inverse have colour to speak of).
2. The filter, through OBS's real renderer (the headless harness from obs-shaderfilter's tests),
   against the reference: a 33^3 grid of colours, swatches, and an image with its own transparency
   (anti-aliased shapes, an alpha ramp). The filter looks colours up in a 65^3 table, so it may be
   off by a little between table points: mean under 1/255, every pixel within 4/255.
3. HDR, if the harness has its HDR test source (raw half-float images named NAME_WxH_SPACE.f32):
   linear colour up to 4x SDR white, in the canvas HDR space and in scRGB, against the reference
   followed by OBS's own HDR-to-SDR tonemap. Skipped, saying so, with a harness without it.
4. How well each inversion keeps the contrast between pairs of colours (printed, not checked).

Setup: build the harness and its libobs once with obs-shaderfilter's `tests/run.sh setup`, then
point WORK at its work folder (default ~/.cache/obs-shaderfilter-tests), or set OBS_PREFIX and
HARNESS. Run under X (xvfb-run -a).
"""
import argparse, itertools, json, os, subprocess, sys, tempfile
import numpy as np
from PIL import Image, ImageDraw
from oklab import tone_invert, ok_invert, hsl_invert, to_lin, to_enc, LUM, lstar, lab

WORK = os.environ.get("WORK", os.path.expanduser("~/.cache/obs-shaderfilter-tests"))
OBS_PREFIX = os.environ.get("OBS_PREFIX", f"{WORK}/libobs/prefix")
HARNESS = os.environ.get("HARNESS", f"{WORK}/harness-build/harness")
W, H = 640, 360
PALETTE = [(0.80, 0.10, 0.15), (1, 0, 0), (0.95, 0.55, 0.10), (1, 0.85, 0), (0.10, 0.60, 0.25), (0.10, 0.75, 0.85),
           (0.10, 0.30, 0.80), (0, 0, 1), (0.50, 0.20, 0.60), (0.08, 0.12, 0.35), (0.35, 0.05, 0.08), (0.04, 0.04, 0.04),
           (0.30, 0.30, 0.30), (0.55, 0.55, 0.55), (0.80, 0.80, 0.80), (0.98, 0.80, 0.85), (0.98, 0.92, 0.60),
           (0.80, 0.95, 0.85), (0.85, 0.82, 0.98), (0.75, 0.88, 0.98), (1, 1, 1)]
GRID = np.round(np.stack(np.meshgrid(*[np.arange(33) / 32] * 3, indexing="ij"), -1).reshape(-1, 3) * 255) / 255
MEAN_LIMIT, MAX_LIMIT = 1.0, 4.0  # /255
# OBS's HDR-to-SDR conversion (libobs/data/color.effect: reinhard in Rec. 2020)
REC709_TO_2020 = np.array([[0.62740389593469903, 0.32928303837788370, 0.043313065687417225],
                           [0.069097289358232075, 0.91954039507545871, 0.011362315566309178],
                           [0.016391438875150280, 0.088013307877225749, 0.89559525324762401]])
REC2020_TO_709 = np.array([[1.6604910021084345, -0.58764113878854951, -0.072849863319884883],
                           [-0.12455047452159074, 1.1328998971259603, -0.0083494226043694768],
                           [-0.018150763354905303, -0.10057889800800739, 1.1187296613629127]])
SDR_WHITE = 300.0  # nits; the harness sets OBS's SDR white level to this


def tonemap(rgb):
    x = rgb @ REC709_TO_2020.T
    x = to_lin(np.clip(x / (x + 1), 0, 1) ** (1 / 2.4))
    return x @ REC2020_TO_709.T


def hdr_image():
    """Linear colour, 1.0 at SDR white: random colours, a grey ramp to 4x white, vivid colours past white."""
    rng = np.random.default_rng(3)
    img = np.zeros((H, W, 4))
    img[..., 3] = 1
    img[:180, :, :3] = np.repeat(np.repeat(rng.random((23, W // 8, 3)) ** 2.2, 8, 0), 8, 1)[:180, :W]
    img[180:270, :, :3] = np.linspace(0, 4, W)[None, :, None]
    img[270:, :, :3] = np.array([1.6, 0.4, 0.2]) * np.linspace(0, 2, W)[:, None]
    return img


def images(out):
    sw = np.ones((H, W, 3))
    for i, c in enumerate(PALETTE):
        x, y = 12 + (i % 7) * 90, 12 + (i // 7) * 115
        sw[y:y + 100, x:x + 78] = c
    Image.fromarray((sw * 255 + 0.5).astype(np.uint8)).save(f"{out}/swatches.png")
    grid = np.ones((H, W, 3))
    grid.reshape(-1, 3)[:len(GRID)] = GRID
    Image.fromarray((grid * 255 + 0.5).astype(np.uint8)).save(f"{out}/grid.png")
    big = Image.new("RGBA", (W * 4, H * 4), (0, 0, 0, 0))
    d = ImageDraw.Draw(big)
    for i, c in enumerate(PALETTE[:12]):
        x, y = 40 + (i % 6) * 400, 80 + (i // 6) * 560
        d.ellipse([x, y, x + 320, y + 320], fill=tuple(int(v * 255) for v in c) + (255,))
    a = np.asarray(big.resize((W, H), Image.LANCZOS)).astype(float)
    a[300:, :, :3] = [40, 90, 200]
    a[300:, :, 3] = np.linspace(0, 255, W)[None, :]
    Image.fromarray(a.astype(np.uint8), "RGBA").save(f"{out}/transparent.png")
    hdr = hdr_image()
    hdr.astype(np.float16).tofile(f"{out}/hdr_{W}x{H}_extended.f32")
    scrgb = hdr.copy()
    scrgb[..., :3] *= SDR_WHITE / 80  # scRGB has 1.0 at 80 nits
    scrgb.astype(np.float16).tofile(f"{out}/hdr_{W}x{H}_scrgb.f32")


SDR_IMAGES = ("grid", "swatches", "transparent")
HDR_IMAGES = ("extended", "scrgb")


def render(plugin, data, out):
    imgs = {i: f"{out}/{i}.png" for i in SDR_IMAGES}
    imgs.update({s: f"{out}/hdr_{W}x{H}_{s}.f32" for s in HDR_IMAGES})
    cases = [(i, i, True) for i in SDR_IMAGES] + [(f"{s}_{m}", s, m == "inverted") for s in HDR_IMAGES for m in ("plain", "inverted")]
    spec = {"width": W, "height": H, "images": imgs,
            "cases": [{"name": n, "source": s, "filters": [{"id": "grove_invert_filter", "settings": {}}] if f else [],
                       "steps": [{"dt": 0.05, "capture": True}]} for n, s, f in cases]}
    json.dump(spec, open(f"{out}/spec.json", "w"))
    env = dict(os.environ, DISPLAY=os.environ.get("DISPLAY", ":99"), LD_LIBRARY_PATH=f"{OBS_PREFIX}/lib")
    r = subprocess.run([HARNESS, "--obs-prefix", OBS_PREFIX, "--plugin", plugin, "--data", data, "--spec", f"{out}/spec.json", "--out", f"{out}/render"],
                       env=env, capture_output=True, text=True)
    if r.returncode != 0 or "Error compiling" in r.stderr:
        sys.exit(f"harness failed:\n{r.stderr[-1500:]}")


def captured(out, name):
    return np.asarray(Image.open(f"{out}/render/{name}_0.png").convert("RGBA")).astype(float) / 255


def report(label, err, ok_mean=MEAN_LIMIT, ok_max=MAX_LIMIT):
    ok = err.mean() <= ok_mean and err.max() <= ok_max
    print(f"  {label:34s} mean {err.mean():5.2f}  max {err.max():5.2f} /255  {'ok' if ok else 'FAIL'}")
    return ok


def check_reference():
    out = tone_invert(GRID)
    t_in, t_out = lstar(to_lin(GRID) @ LUM) * 100, lstar(to_lin(out) @ LUM) * 100
    tone_err = np.abs(t_out - (100 - t_in)).max()
    a, b = lab(GRID), lab(out)
    both = (np.hypot(a[:, 1], a[:, 2]) > 0.03) & (np.hypot(b[:, 1], b[:, 2]) > 0.03)
    hue = np.degrees(np.abs(np.angle(np.exp(1j * (np.arctan2(b[both, 2], b[both, 1]) - np.arctan2(a[both, 2], a[both, 1]))))))
    ok = tone_err < 0.1 and hue.max() < 2
    print(f"  {'reference: tone on 100 - L*':34s} worst {tone_err:.3f} L*;  hue kept: worst {hue.max():.2f} degrees  {'ok' if ok else 'FAIL'}")
    return ok


def check_sdr(out):
    ok = True
    for img in SDR_IMAGES:
        src = np.asarray(Image.open(f"{out}/{img}.png").convert("RGBA")).astype(float) / 255
        a = src[..., 3:]
        got = captured(out, img)
        want = a * tone_invert(src[..., :3])  # OBS premultiplies sRGB-encoded colour
        err = np.abs(got[..., :3] - want).max(-1) * 255
        if img == "grid":
            err = err.reshape(-1)[:len(GRID)]
        ok &= report(img, err)
        alpha = np.abs(got[..., 3:] - a).max() * 255
        if alpha > 0.5:
            print(f"  {img}: alpha changed by {alpha:.1f}/255  FAIL")
            ok = False
    return ok


def check_hdr(out):
    if captured(out, "extended_plain")[..., 3].max() == 0:
        print("  HDR: skipped (this harness has no HDR test source)")
        return True
    lin = hdr_image()[..., :3]
    want = {"plain": to_enc(np.clip(tonemap(lin), 0, 1)),
            "inverted": to_enc(np.clip(tonemap(to_lin(tone_invert(to_enc(np.clip(lin, 0, 1))))), 0, 1))}
    ok = True
    for s in HDR_IMAGES:
        for m in ("plain", "inverted"):
            err = np.abs(captured(out, f"{s}_{m}")[..., :3] - want[m]).max(-1) * 255
            ok &= report(f"HDR {s}, {m}", err)
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
    args = ap.parse_args()
    out = args.out or tempfile.mkdtemp(prefix="invert-check-")
    os.makedirs(out, exist_ok=True)
    images(out)
    render(args.plugin, os.path.abspath(args.data), out)
    print("Invert against the reference:")
    ok = check_reference()
    ok &= check_sdr(out)
    ok &= check_hdr(out)
    contrast()
    sys.exit(0 if ok else 1)
