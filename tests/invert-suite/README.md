# Invert test suite

Synthetic logos with exact ideals for measuring the [Invert](../../plugins/obs-invert) filter's
transparent background, and the scripts that score the real shader on them.

- **`suite.py OUTDIR`** builds the suite: 18 logos (9 for tuning, 9 held out) made of layers with exactly
  known coverage (4× supersampled). They include bold, regular, serif and small text, hairlines, solid
  shapes in saturated, dark, pastel and grey colours, gradient fills, badges with dark or knocked-out
  text, outlined text, black drop shadows at 30–75% opacity and occasional coloured glows. Each logo
  comes in 7 renderings: crisp PNG, JPEG q75, JPEG q50, soft (σ 1.0) + JPEG, blurred (σ 1.8) + JPEG,
  cream paper + JPEG, and PNG with its own transparency. The ideal is the layer stack with each
  layer's colour inverted, over transparency. Takes about 30 s.
- **`measure.py --plugin obs-invert.so --data DATA --out OUT`** runs the real shader through OBS's
  renderer (the headless harness from obs-shaderfilter's tests) on every image, transparent
  background on. It composites the result over black, a colourful backdrop and white, and reports the
  error against the ideal (largest channel difference, 0–255, averaged over the backdrops) for each
  region kind: background, edge, interior, dark interior, seam, shadow, thin. The headline is the mean
  over region kinds. It's deterministic.
- **`tuned.py --plugin obs-invert.so --data DATA --out OUT`** gives the **tuned** score: what a tech
  could reach by adjusting the per-logo controls (JPEG clean-up, edge softness, thin lines, dark greys
  as shadows). It renders the suite with every combination, takes each logo's best and scores those.
  About 20 minutes.
- **`oracle.py OUT`** gives the ceiling for a run: the error left if every pixel had the best alpha for
  the colour the filter produced. The difference from the run's own score is what better alpha
  estimation could still win.
- **`compare.py BEFORE AFTER OUT.png NAMES...`** shows source, before, after and ideal side by side.

Setup: build the harness and its libobs once with obs-shaderfilter's `tests/run.sh setup`, then point
`WORK` at its work folder (default `~/.cache/obs-shaderfilter-tests`), or set `OBS_PREFIX` and
`HARNESS`. Generate the suite into `suite/` (or set `SUITE`). Run under X (`xvfb-run -a`).

## The climb so far

Headline on the tuning logos / the held-out logos (lower is better):

| Version | Tuning | Held out | Notes |
|---|---|---|---|
| Single-pass Invert (7123650) | 17.28 | 17.49* | grey parts fade, coloured stay solid |
| Multi-pass (da4d2a6) | **12.70** | **11.71** | paper colour from the border; alpha from same-colour plateaus |
| Ceiling for da4d2a6 | 5.02 | 6.41 | best possible alpha for its colours |
| Per-logo controls, defaults | 12.70 | 11.71 | the defaults reproduce da4d2a6 exactly |
| Per-logo controls, tuned | **10.55** | **10.82** | each logo with its best controls |
| Two-colour seams, pale hairlines | **12.14** | **11.34** | defaults; seams blended from both colours' inversions |

\* measured on the first 3 held-out logos; on all 9, the multi-pass version's 11.71 compares with
13.19 on those 3.

Tried after da4d2a6 and not kept, because they moved the score by no more than noise or made it
worse:
- Letting a dark-grey plateau count when background is within 4 px and the drop is steep. This fixes
  narrow light-grey strokes, but blurred grey hairlines go solid.
- Ramps instead of yes/no decisions for "background within 2" and "solid part near".
- Tightening "a solid part about as dark as the pixel". Halos improve but seams get much worse.
- An unmix pass that reads 2.5 px up and down the gradient and blends the alpha of the two sides.
  Edges improve by about 2.5 and seams get worse by about 3. Dark outlines in a hue close to their
  text can't be told from a partial mix.
- Using each part's plateau or crisp sides in the soft branch, or counting the radius-4 ring's steps
  as crisp evidence. Shadows near crisp shapes go solid.

With the controls, edges gain most (19.2 to 13.0 on the tuning logos: edge softness on blurred
logos), then thin features and dark interiors. JPEG clean-up above 8% used to crush pale parts of a
logo along with the specks (dark interior 9 to 72 at 16%); it now leaves anything in an area of 8 or
more similar pixels as at 8%, so raising it clears specks round text at little cost (12.74 at 12%,
13.28 at 18%). The score barely shows the specks going, because they are few pixels, but to the eye
it is the largest improvement for JPEG logos.

Then two faults that were plain to see on one logo (Paper Wolves):
- Where two of the logo's colours meet (dark text on a coloured badge), the pixels in between were
  inverted as colours of their own, and keeping hue isn't linear for a blend of two colours: the text
  got outlined in the wrong colour. Now such a pixel is placed between the two colours by luma (which
  JPEG keeps sharp, unlike colour) and gets the blend of their inversions. Seam error on the crisp PNG
  went from 20.6 to 3.0.
- A pale grey hairline taken as solid turned into an opaque near-black line over the visuals. A thin
  feature that is dark once inverted now has to be crisper to count as solid, since being wrongly
  faint costs far less than being wrongly solid.

What the review (Opus, Fable, GPT) thinks would take it further. All of it needs evidence from beyond
a pixel's neighbourhood, and they estimate a headline of about 9–11:
- Pool or smooth each part's full-strength brightness G across the part, keyed on colour. Alpha is
  then the pixel's brightness over a G that is constant within the part.
- Spread "crisp rim" and "soft rim" evidence inwards through same-colour pixels, a few passes at
  growing strides. A solid shape has a crisp edge against background somewhere; a shadow doesn't.
- Estimate the image's blur once, and judge "crisp" and "flat" against it.
- Build a palette pass with the brightest plateau of each hue in the logo, for thin coloured text that
  never reaches full strength.

Limits nothing local can get past:
- A pixel of grey at full strength and one of white at partial coverage look the same.
- Below the blur width, only stroke width × strength is visible.
- JPEG noise is magnified by 1/G on dark parts.
