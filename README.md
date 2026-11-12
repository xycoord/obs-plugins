# OBS plugins for The Grove

The OBS Studio plugins used for the projector visuals at The Grove, built and installed together on
Linux.

| Plugin | What it is |
|---|---|
| [obs-invert](plugins/obs-invert) | **Invert**: swaps light and dark. By default it keeps each colour's hue and saturation and flips only lightness, so a logo supplied on white goes onto black and keeps its colours: the white background turns black, black text turns white, a red stays red and a dark navy becomes a light blue. **JPEG clean-up** clears the specks a JPEG leaves around text, repairs the colour JPEG smears along edges (which inverting would turn into dark bands) and takes cream or off-white paper as background too. **Transparent background** lets the logo go over visuals: edges blend into what's behind, shadows and glows fade to transparent, solid parts stay solid however dark, and over black it looks exactly the same as without. |
| [obs-typeset](plugins/obs-typeset) | **Typeset Text**: a text source using any installed font in any weight or style. It **scales like a vector drawing**: resize it in the scene and it's redrawn at the size it's shown, never blurred or pixelated, and never drawn at more pixels than it covers. And it's **edited right in the preview**: double-click the text and type, as in a slide editor. |
| [obs-shaderfilter](plugins/obs-shaderfilter) | A filter that applies a shader to a source, with a library of 150+ effects (blur, bloom, film grain, glitch, distortion, colour). A submodule: [xycoord/obs-shaderfilter](https://github.com/xycoord/obs-shaderfilter). |

### Using Invert on a logo

Add it to the logo's image source (Filters, then +, then Invert). The defaults suit a logo on white
going onto black. For a logo over visuals, tick **Transparent background**; if a dark box is left where
the background was, raise **JPEG clean-up** until the background is fully black.

- Anything that ends up black becomes see-through, including white text cut out of a shape.
- Shadows, glows and other soft parts fade to transparent; solid parts stay solid however dark.
  **Fade shadows and glows** sets how far (100% by default). If a textured or grainy part of a logo turns
  see-through, lower it; 0% keeps every part solid.

The defaults are automatic and suit most logos. If one doesn't come out right, look at it over the
visuals and adjust for what you see:

| You see | Change |
|---|---|
| Part of the background is left as a dark box, or the logo goes too | **Background**: pick the paper's colour (the logo may touch the image's edges) |
| Black specks or blotches around text (JPEG noise) | Raise **JPEG clean-up** (12–18%). Above 8% it clears only specks, not pale parts of the logo |
| Dark rims or grey halos round the edges of a soft or enlarged logo | Raise **Edge softness** (1.5–2.5 px) |
| Thin lines or small text too faint, or see-through | **Thin lines and small text**: Solid |
| Thin lines or small text too strong, with a hard outline, on a blurred logo | **Thin lines and small text**: Faint |
| Light grey shapes or text in the logo come out see-through | Untick **Dark greys can be shadows** |
| A textured or grainy part turns see-through | Lower **Fade shadows and glows** |

- How each pixel's transparency is worked out is described at the top of
  [invert.effect](plugins/obs-invert/data/invert.effect); it was tuned on a suite of synthetic logos with
  known ideals (crisp, JPEG, blurred, cream paper, already transparent).
- The filter works on the image at its own size every frame: scale very large logo files (print
  artwork thousands of pixels across) down to about projector size first.

## Installing

On the machine that runs OBS (it needs `git`, `cmake`, a C and C++ compiler, Pango and Qt 6's
development files and, for OBS 32 and later, SIMDe:
`sudo apt install git cmake build-essential libpango1.0-dev qt6-base-dev libsimde-dev`):

```bash
git clone --recursive https://github.com/xycoord/obs-plugins
cd obs-plugins
./install.sh
```

Then restart OBS. The plugins are built for the version of OBS installed on that machine and go into
OBS's per-user plugins folder, `~/.config/obs-studio/plugins`.

To update after changes here, or after OBS itself has been updated:

```bash
git pull --recurse-submodules
./install.sh
```

`./install.sh --uninstall` removes them again. `./install.sh <name>` builds and installs just one.

### How it builds

Each plugin is compiled against the libobs headers of exactly the OBS version installed (fetched from
obsproject's git the first time and kept in `~/.cache/obs-plugins`) and linked to the installed
libobs, so it always matches the OBS that loads it. No OBS development packages are needed.

OBS installed as a Flatpak is detected and the plugins are put in its folder instead, but that is
untested: a plugin built on the machine may need a newer C library than the Flatpak has.

## Adding a plugin

Put it in `plugins/<name>/`, as a folder or, if it has its own repository, as a submodule. It needs:

- a `CMakeLists.txt` that builds a module called `<name>` and finds libobs with
  `find_package(libobs REQUIRED)` (target `OBS::libobs`) and, if it adds to OBS's window, the
  frontend API with `find_package(obs-frontend-api REQUIRED)` (target `OBS::obs-frontend-api`);
- its data files, if any, in `plugins/<name>/data/`.

`install.sh` picks it up from there. A test scene for the smoke test can go in
`tests/scenes/<name>.json` (see below).

## Tests

`tests/smoke.sh` starts the installed OBS with a throwaway configuration and checks that it loads
every plugin, then loads each plugin's test scene and checks OBS reports no errors for it (for
example a shader that doesn't compile). `@DATA@` in a scene file stands for the plugin's installed
data folder. On a machine without a display, run it as `xvfb-run -a tests/smoke.sh`.

[tests/typeset](tests/typeset) runs OBS on a virtual display and checks Typeset Text's scaling and
editing in the preview, using OBS's window with the mouse and keyboard (it needs `xvfb`, `xdotool`,
`imagemagick` and Python's numpy, pillow and websocket-client).

[tests/invert-suite](tests/invert-suite) measures the Invert filter's transparent background against
synthetic logos with exact ideals.

CI builds, installs and smoke-tests everything against Ubuntu 24.04's OBS and against the latest OBS
from the official PPA.

Plugins can have deeper tests of their own; obs-shaderfilter's compare its output with release 2.6.1
frame by frame and run in its own repository.
