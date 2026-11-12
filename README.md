# OBS plugins for The Grove

The OBS Studio plugins used for the projector visuals at The Grove, built and installed together on
Linux.

| Plugin | What it is |
|---|---|
| [obs-shaderfilter](plugins/obs-shaderfilter) | A filter that applies a shader to a source, with a library of 150+ effects (blur, bloom, film grain, glitch, distortion, colour). A submodule: [xycoord/obs-shaderfilter](https://github.com/xycoord/obs-shaderfilter). |

## Installing

On the machine that runs OBS (it needs `git`, `cmake` and a C compiler:
`sudo apt install git cmake build-essential`):

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

CI builds, installs and smoke-tests everything against Ubuntu 24.04's OBS and against the latest OBS
from the official PPA.

Plugins can have deeper tests of their own; obs-shaderfilter's compare its output with release 2.6.1
frame by frame and run in its own repository.
