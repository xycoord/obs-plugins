# OBS plugins for The Grove

The OBS Studio plugins used for the projector visuals at The Grove, built and installed together on
Linux.

| Plugin | What it is |
|---|---|
| [obs-invert](plugins/obs-invert) | **Invert**: swaps light and dark. By default it keeps each colour's hue and saturation and flips only lightness, so a logo supplied on white goes onto black and keeps its colours: the white background turns black, black text turns white, a red stays red and a dark navy becomes a light blue. **JPEG clean-up** clears the specks a JPEG leaves around text, repairs the colour JPEG smears along edges (which inverting would turn into dark bands) and takes cream or off-white paper as background too. **Transparent background** lets the logo go over visuals: edges blend into what's behind, shadows and glows fade to transparent, solid parts stay solid however dark, and over black it looks exactly the same as without. |
| [obs-shaderfilter](plugins/obs-shaderfilter) | A filter that applies a shader to a source, with a library of 150+ effects (blur, bloom, film grain, glitch, distortion, colour). A submodule: [xycoord/obs-shaderfilter](https://github.com/xycoord/obs-shaderfilter). |

### Using Invert on a logo

Add it to the logo's image source (Filters, then +, then Invert). The defaults suit a logo on white
going onto black. For a logo over visuals, tick **Transparent background**; if a dark box is left where
the background was, raise **JPEG clean-up** until the background is fully black.

- Anything that ends up black becomes see-through, including white text cut out of a shape.
- Shadows, glows and other soft parts fade to transparent; solid parts stay solid however dark.
  **Fade shadows and glows** sets how far (100% by default). If a textured or grainy part of a logo turns
  see-through, lower it; 0% keeps every part solid.
- How each pixel's transparency is worked out is described at the top of
  [invert.effect](plugins/obs-invert/data/invert.effect); it was tuned on a suite of synthetic logos with
  known ideals (crisp, JPEG, blurred, cream paper, already transparent).
- The filter works on the image at its own size every frame: scale very large logo files (print
  artwork thousands of pixels across) down to about projector size first.

## Installing

On the machine that runs OBS (it needs `git`, `cmake`, a C compiler and, for OBS 32 and later, SIMDe:
`sudo apt install git cmake build-essential libsimde-dev`):

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
  `find_package(libobs REQUIRED)` (target `OBS::libobs`);
- its data files, if any, in `plugins/<name>/data/`.

`install.sh` picks it up from there. A test scene for the smoke test can go in
`tests/scenes/<name>.json` (see below).

## Tests

`tests/smoke.sh` starts the installed OBS with a throwaway configuration and checks that it loads
every plugin, then loads each plugin's test scene and checks OBS reports no errors for it (for
example a shader that doesn't compile). `@DATA@` in a scene file stands for the plugin's installed
data folder. On a machine without a display, run it as `xvfb-run -a tests/smoke.sh`.

[tests/invert-suite](tests/invert-suite) measures the Invert filter's transparent background against
synthetic logos with exact ideals.

CI builds, installs and smoke-tests everything against Ubuntu 24.04's OBS and against the latest OBS
from the official PPA.

Plugins can have deeper tests of their own; obs-shaderfilter's compare its output with release 2.6.1
frame by frame and run in its own repository.
