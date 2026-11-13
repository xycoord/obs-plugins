# Invert check

`check.py` checks the [Invert](../../plugins/obs-invert) filter against a numpy reference of its
inversion (`oklab.py`), through OBS's real renderer, and shows how well it keeps contrast.

```bash
xvfb-run -a python3 check.py --plugin path/to/obs-invert.so --data plugins/obs-invert/data
```

It renders a sheet of colour swatches and an image with its own transparency (anti-aliased shapes
and an alpha ramp). It fails if any pixel differs from the reference by
more than 2/255; the filter is within 1.6 (8-bit rounding at partial alpha).

Setup: build the harness and its libobs once with obs-shaderfilter's `tests/run.sh setup`, then point
`WORK` at its work folder (default `~/.cache/obs-shaderfilter-tests`), or set `OBS_PREFIX` and
`HARNESS`.

## Why tone

Invert flips each colour's tone (CIELAB L\*, perceived lightness from luminance alone) and keeps
its OKLab hue and chroma, the way Google's HCT makes dark themes. Contrast between two colours is a
matter of their luminance, and L\* is luminance on a perceptually even scale, so every pair keeps its
contrast. The filter first flipped HSL lightness, which leaves vivid colours where they are and
moves pale and dark ones: orange text on navy loses almost all its contrast. Flipping OKLab
lightness was tried next; OKLab L isn't a function of luminance alone, so it keeps contrast less
well and crushes pale colours to near black. Over the 210 pairs of the check's 21 colours (122 of them
clearly different in tone, by more than 25 L\*):

| Inversion | Mean change in tone contrast (L\*) | Mean change in WCAG ratio (log) | Clear pairs losing over half |
|---|---|---|---|
| HSL lightness | 19.0 | 0.634 | 28 of 122 |
| OKLab lightness | 6.1 | 0.244 | 1 of 122 |
| Tone (the filter) | 0.0 | 0.046 | 0 of 122 |

## History

The filter once also removed the paper background (with JPEG clean-up, edge, shadow and thin-line
handling) to put logos over visuals; that work, its synthetic test suite with exact ideals and the
record of tuning it, are in the repository at commit 9cbcfc6 (`tests/invert-suite`). Colour
Correction does that job well enough in practice, so Invert now only inverts.
