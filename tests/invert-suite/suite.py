"""Synthetic logo suite with exact ideals for the Invert filter.

Each logo is a stack of layers (colour, exact coverage from 4x supersampling,
kind: solid / thin / soft). The source image composites the layers over white
(or cream paper, or transparency); the ideal is the same stack with each
layer's colour inverted (keep hue and saturation), composited over
transparency. Cut-outs (white text in a shape) are holes in the shape, so the
ideal shows them as background, as the plugin intends.

    python3 suite.py OUTDIR

writes OUTDIR/<logo>_<variant>.png (source), OUTDIR/<logo>_<variant>.npz
(gt_c premultiplied, gt_a, region masks) and OUTDIR/index.json.
"""
import json, sys, os
import numpy as np
from PIL import Image, ImageDraw, ImageFont, ImageFilter

W, H, S = 640, 360, 4
FONTS = {
    "sans_bold": "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
    "sans": "/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
    "serif_bold": "/usr/share/fonts/truetype/dejavu/DejaVuSerif-Bold.ttf",
    "serif": "/usr/share/fonts/truetype/dejavu/DejaVuSerif.ttf",
    "lib_bold": "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
    "lib_serif": "/usr/share/fonts/truetype/liberation/LiberationSerif-Regular.ttf",
}
COLOURS = {
    "saturated": [(0.80, 0.10, 0.15), (0.10, 0.30, 0.80), (0.10, 0.60, 0.25), (0.95, 0.55, 0.10), (0.50, 0.20, 0.60)],
    "dark": [(0.04, 0.04, 0.04), (0.08, 0.12, 0.35), (0.05, 0.25, 0.12), (0.35, 0.05, 0.08)],
    "pastel": [(0.98, 0.80, 0.85), (0.98, 0.92, 0.60), (0.80, 0.95, 0.85), (0.85, 0.82, 0.98), (0.75, 0.88, 0.98)],
    "grey": [(0.80, 0.80, 0.80), (0.55, 0.55, 0.55), (0.30, 0.30, 0.30)],
}
PAPER = (0.98, 0.95, 0.88)


def keephue(c):
    return c + (1 - c.max(-1, keepdims=True) - c.min(-1, keepdims=True))


class Logo:
    def __init__(self, rng):
        self.rng = rng
        self.layers = []  # dicts: cov (H,W), col (H,W,3), kind, name

    def canvas(self):
        return Image.new("L", (W * S, H * S), 0)

    def cov(self, im, blur=0.0):
        a = np.asarray(im.resize((W, H), Image.BOX)).astype(float) / 255
        if blur > 0:
            a = np.asarray(Image.fromarray((a * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur))).astype(float) / 255
        return a

    def add(self, cov, col, kind, name):
        col = np.broadcast_to(np.asarray(col, float), (H, W, 3)).copy() if np.ndim(col) == 1 else col
        self.layers.append({"cov": cov, "col": col, "kind": kind, "name": name})
        return len(self.layers) - 1

    def colour(self, group):
        g = COLOURS[group]
        return g[self.rng.integers(len(g))]

    def font(self, key, size):
        return ImageFont.truetype(FONTS[key], int(size * S))

    def text_cov(self, xy, text, font_key, size, stroke=0):
        im = self.canvas()
        ImageDraw.Draw(im).text((xy[0] * S, xy[1] * S), text, font=self.font(font_key, size), fill=255,
                                stroke_width=int(stroke * S), stroke_fill=255)
        return self.cov(im)

    def shape_cov(self, kind, box, **kw):
        im = self.canvas()
        d = ImageDraw.Draw(im)
        b = [v * S for v in box]
        if kind == "circle":
            d.ellipse(b, fill=255)
        elif kind == "ring":
            d.ellipse(b, fill=255)
            t = kw.get("t", 0.28) * (box[2] - box[0])
            d.ellipse([b[0] + t * S, b[1] + t * S, b[2] - t * S, b[3] - t * S], fill=0)
        elif kind == "rrect":
            d.rounded_rectangle(b, int(kw.get("r", 10) * S), fill=255)
        elif kind == "tri":
            d.polygon([((b[0] + b[2]) / 2, b[1]), (b[2], b[3]), (b[0], b[3])], fill=255)
        elif kind == "star":
            cx, cy, r = (b[0] + b[2]) / 2, (b[1] + b[3]) / 2, (b[2] - b[0]) / 2
            pts = []
            for i in range(10):
                ang = np.pi / 2 + i * np.pi / 5
                rr = r if i % 2 == 0 else r * 0.45
                pts.append((cx + rr * np.cos(ang), cy - rr * np.sin(ang)))
            d.polygon(pts, fill=255)
        elif kind == "line":
            d.line(b, fill=255, width=max(1, int(kw.get("w", 1.0) * S)))
        return self.cov(im)


def build(seed):
    rng = np.random.default_rng(seed)
    L = Logo(rng)
    r = rng.random
    # 1. emblem: a shape, sometimes with a soft grey drop shadow, sometimes a ring
    ex, ey, es = 30 + r() * 40, 60 + r() * 60, 140 + r() * 60
    box = [ex, ey, ex + es, ey + es]
    shape = rng.choice(["circle", "ring", "rrect", "star"])
    group = rng.choice(["saturated", "dark", "pastel", "grey"], p=[0.4, 0.25, 0.25, 0.1])
    col = L.colour(group)
    cov = L.shape_cov(shape, box, r=16)
    if r() < 0.6:  # soft grey drop shadow / glow behind the emblem
        blur = 3 + r() * 8
        off = (r() * 8, r() * 8)
        sh = L.shape_cov(shape, [box[0] + off[0], box[1] + off[1], box[2] + off[0], box[3] + off[1]], r=16)
        sh = np.asarray(Image.fromarray((sh * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur))).astype(float) / 255
        grey = r() * 0.15          # drop shadows are black (or nearly) at partial opacity
        L.add(sh * (0.3 + r() * 0.45), (grey, grey, grey), "soft", "shadow")
    if r() < 0.12:  # rare coloured glow
        gl = np.asarray(Image.fromarray((cov * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(10))).astype(float) / 255
        L.add(gl * 0.6, L.colour("saturated"), "soft", "coloured glow")
    if r() < 0.25:  # gradient fill
        c2 = np.asarray(L.colour(rng.choice(["saturated", "pastel"])))
        t = np.clip((np.arange(W) - box[0]) / max(1, box[2] - box[0]), 0, 1)[None, :, None]
        colg = np.asarray(col)[None, None, :] * (1 - t) + c2[None, None, :] * t
        L.add(cov, np.broadcast_to(colg, (H, W, 3)).copy(), "solid", "gradient emblem")
    else:
        L.add(cov, col, "solid", f"emblem {group}")
    if r() < 0.6:  # something inside the emblem: a smaller shape
        inner = [box[0] + es * 0.33, box[1] + es * 0.33, box[2] - es * 0.33, box[3] - es * 0.33]
        L.add(L.shape_cov(rng.choice(["tri", "circle", "star"]), inner), L.colour(rng.choice(["saturated", "pastel", "dark"])), "solid", "inner shape")

    # 2. title text, maybe with a drop shadow or an outline
    tx, ty = ex + es + 25, 50 + r() * 40
    title = rng.choice(["NIGHT ORCHARD", "VELVET HUM", "the lantern", "SLOW TIDES", "Paper Wolves", "KITE & CO"])
    fk = rng.choice(["sans_bold", "serif_bold", "lib_bold"])
    size = 44 + r() * 20
    tgroup = rng.choice(["dark", "saturated", "grey"], p=[0.6, 0.3, 0.1])
    tcol = L.colour(tgroup)
    words = title.split(" ", 1)
    covs = [L.text_cov((tx, ty), words[0], fk, size)]
    if len(words) > 1:
        covs.append(L.text_cov((tx, ty + size * 1.05), words[1], fk, size))
    tcov = np.maximum.reduce(covs)
    if r() < 0.35:
        sh = np.asarray(Image.fromarray((tcov * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(2 + r() * 3))).astype(float) / 255
        sh = np.roll(np.roll(sh, 3, 0), 3, 1)
        g = r() * 0.15
        L.add(sh * (0.3 + r() * 0.4), (g, g, g), "soft", "text shadow")
    if r() < 0.25:  # outlined text: outline layer under the text
        oc = [L.text_cov((tx, ty), words[0], fk, size, stroke=2.5)]
        if len(words) > 1:
            oc.append(L.text_cov((tx, ty + size * 1.05), words[1], fk, size, stroke=2.5))
        L.add(np.maximum.reduce(oc), L.colour(rng.choice(["saturated", "pastel"])), "solid", "text outline")
    L.add(tcov, tcol, "solid", f"title {tgroup}")

    # 3. a rule (hairline) and small text
    ry = ty + size * 2.3
    if r() < 0.7:
        w = rng.choice([0.7, 1.0, 1.5, 3.0])
        L.add(L.shape_cov("line", [tx, ry, tx + 300, ry + r() * 2], w=w), L.colour(rng.choice(["grey", "dark", "saturated"])),
              "thin" if w < 2 else "solid", f"rule {w}px")
    small_group = rng.choice(["dark", "saturated", "grey", "pastel"], p=[0.35, 0.35, 0.2, 0.1])
    L.add(L.text_cov((tx, ry + 10), rng.choice(["live and unplugged", "since 2019", "nottingham", "the grove tour"]),
                     rng.choice(["serif", "sans", "lib_serif"]), 14 + r() * 12), L.colour(small_group), "thin", f"small text {small_group}")

    # 4. a badge: dark text on a pastel/saturated badge, or white text knocked out of it
    if r() < 0.75:
        bx, by = 30 + r() * 30, 280 + r() * 20
        bcov = L.shape_cov("rrect", [bx, by, bx + 190, by + 54], r=12)
        btext = L.text_cov((bx + 14, by + 9), rng.choice(["TONIGHT", "SOLD OUT", "LIVE"]), "sans_bold", 30)
        if r() < 0.5:
            L.add(bcov * (1 - btext), L.colour(rng.choice(["saturated", "dark"])), "solid", "badge with knocked-out text")
        else:
            L.add(bcov, L.colour(rng.choice(["pastel", "saturated"])), "solid", "badge")
            L.add(btext, L.colour("dark"), "solid", "badge text")
    # 5. a pastel or light-grey block (dark once inverted)
    if r() < 0.5:
        px, py = 400 + r() * 60, 280 + r() * 20
        L.add(L.shape_cov("rrect", [px, py, px + 150, py + 44], r=10), L.colour(rng.choice(["pastel", "grey"])), "solid", "pale block")
    return L


def render(L, variant):
    blur = {"soft": 1.0, "blur": 1.8}.get(variant.split("_")[0], 0.0)
    layers = []
    for l in L.layers:
        cov = l["cov"]
        if blur:
            cov = np.asarray(Image.fromarray((np.clip(cov, 0, 1) * 255).astype(np.uint8)).filter(ImageFilter.GaussianBlur(blur))).astype(float) / 255
        layers.append(dict(l, cov=cov))
    alpha_bg = variant.startswith("alpha")
    paper = np.asarray(PAPER if variant.startswith("paper") else (1.0, 1.0, 1.0))
    img_c = np.zeros((H, W, 3)) if alpha_bg else np.broadcast_to(paper, (H, W, 3)).copy()
    img_a = np.zeros((H, W)) if alpha_bg else np.ones((H, W))
    gt_c = np.zeros((H, W, 3)); gt_a = np.zeros((H, W))
    vis = []  # visible weight of each layer
    for l in layers:
        a = l["cov"][..., None]
        img_c = img_c * (1 - a) + l["col"] * a  # premultiplied when alpha_bg
        img_a = img_a * (1 - a[..., 0]) + a[..., 0]
        gt_c = gt_c * (1 - a) + keephue(l["col"]) * a
        gt_a = gt_a * (1 - a[..., 0]) + a[..., 0]
    for i, l in enumerate(layers):
        w = l["cov"].copy()
        for m in layers[i + 1:]:
            w = w * (1 - m["cov"])
        vis.append(w)
    vis = np.stack(vis)
    kinds = np.array([l["kind"] for l in layers])
    soft_w = vis[kinds == "soft"].sum(0) if (kinds == "soft").any() else np.zeros((H, W))
    hard_w = vis[kinds != "soft"].sum(0)
    thin_w = vis[kinds == "thin"].sum(0) if (kinds == "thin").any() else np.zeros((H, W))
    top = vis.max(0)
    regions = {
        "background": gt_a < 0.005,
        "edge": (gt_a > 0.02) & (gt_a < 0.98) & (soft_w < 0.02) & (thin_w < 0.02),
        "interior": (top >= 0.98) & (np.isin(np.argmax(vis, 0), np.nonzero(kinds == "solid")[0])),
        "seam": (gt_a >= 0.98) & (top < 0.95) & (soft_w < 0.02),
        "shadow": (soft_w > 0.02) & (hard_w < 0.02),
        "thin": thin_w > 0.02,
    }
    # dark parts: solid interiors whose inverted colour is dark (pale or grey originals)
    inv_v = np.where(gt_a > 0, (gt_c / np.maximum(gt_a, 1e-9)[..., None]).max(-1), 0)
    regions["dark interior"] = regions["interior"] & (inv_v < 0.45)
    if alpha_bg:
        src = np.concatenate([np.where(img_a[..., None] > 0, img_c / np.maximum(img_a, 1e-9)[..., None], 0), img_a[..., None]], -1)
        im = Image.fromarray((np.clip(src, 0, 1) * 255 + 0.5).astype(np.uint8), "RGBA")
    else:
        im = Image.fromarray((np.clip(img_c, 0, 1) * 255 + 0.5).astype(np.uint8), "RGB")
    return im, gt_c, gt_a, regions


VARIANTS = ["png", "jpg75", "jpg50", "soft_jpg75", "blur_jpg75", "paper_jpg75", "alpha_png"]
TRAIN = list(range(1, 10))
HOLDOUT = [101, 102, 103, 104, 105, 106, 107, 108, 109]

if __name__ == "__main__":
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    index = []
    for seed in TRAIN + HOLDOUT:
        L = build(seed)
        for v in VARIANTS:
            im, gt_c, gt_a, regions = render(L, v)
            name = f"logo{seed}_{v}"
            q = {"jpg75": 75, "jpg50": 50}.get(v.split("_")[-1])
            if q:
                im.convert("RGB").save(f"{out}/{name}.jpg", quality=q)
                im = Image.open(f"{out}/{name}.jpg").convert("RGBA")
            im.convert("RGBA").save(f"{out}/{name}.png")
            np.savez_compressed(f"{out}/{name}.npz", gt_c=gt_c.astype(np.float32), gt_a=gt_a.astype(np.float32),
                                **{k.replace(" ", "_"): m for k, m in regions.items()})
            index.append({"name": name, "seed": seed, "variant": v, "holdout": seed in HOLDOUT,
                          "layers": [l["name"] for l in L.layers]})
    json.dump(index, open(f"{out}/index.json", "w"), indent=1)
    print(len(index), "images")
