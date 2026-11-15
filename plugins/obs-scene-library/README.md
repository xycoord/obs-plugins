# Scene Library

Keeps scenes you've perfected, so you can add them back months later. A saved
scene keeps everything that makes it look the way it does:

- every source in it, and in the scenes and groups nested in it;
- each source's filters, in order, with their settings;
- each item's position, size, crop, rotation, blending, visibility and show and
  hide transitions;
- audio settings (volume, mute, balance, sync, monitoring, tracks);
- **copies of the files it uses** (images, videos, shader files, slideshow
  folders), so it still works after the originals have been moved, changed or
  deleted;
- a thumbnail of how it looked.

## Installing

It's built and installed with the other plugins in this repository (see
[Installing](../../README.md#installing)), or on its own, from the
repository's folder:

```bash
./install.sh obs-scene-library
```

Then restart OBS. The **Scene Library** dock appears beside Scenes the first
time OBS runs with the plugin. After that it's under **Docks → Scene Library**
like any other dock, if you've closed it.

## Quick start

1. Get a scene looking the way you want it.
2. In the Scene Library dock, click **Save Scene…**, give it a name you'll
   recognise in months' time ("Interval, blue", "Headliner logo loop") and
   click **OK**. It appears in the dock with a thumbnail.
3. Months later, in whatever scene collection you're using, double-click it
   in the dock. It's added to the Scenes list (and in Studio Mode, to the
   preview); nothing goes live.
4. Click it in the Scenes list (in Studio Mode, transition to it) when you
   want it showing.

## Using it

- **Save Scene…** saves the scene you're editing: the one showing or, in
  Studio Mode, the one in the preview. You name it (its scene's name to start
  with). Saving under a name that's already in the library asks before
  replacing it. A scene with big files (a long video) takes a moment to copy:
  a progress bar shows how far it's got, and **Cancel** stops it, leaving the
  library as it was.
- **Add to Scenes** (or double-click it) adds the selected saved scene to OBS's
  scene list. Nothing goes live: in Studio Mode it's put in the preview;
  otherwise it's added to the list and what's showing stays as it was.
- Right-click a saved scene to replace it with the scene you're editing as it
  is now, rename it, delete it, or show its folder. **Delete** and **F2** work
  too.
- **Search** finds saved scenes by their name or the name of the scene they
  were saved from.
- **⋯** sorts by name or by date saved, shows the library folder, or moves the
  library to another folder.

Hover over a saved scene to see when it was saved, from which scene, and
anything it uses that wasn't copied.

### Adding a scene back never changes what's already in OBS

The scene and everything in it are added as copies of their own, which you can
change without affecting anything else. If a name is already taken, the copy
gets a number (`Logo` becomes `Logo 2`). A setting that refers to a renamed
source by name, as some plugins' do, is pointed at the copy. Text that just
reads the same as a source's name is left alone.

Capture sources are the exception: video capture devices, audio inputs and
outputs, and screen, window and game capture. A second copy of one would fight
the first for the device or double its sound. So if OBS already has a capture
source with the same name and type, the scene uses that one as it's set up
here, and says so.

Files: each one the scene uses is taken from where it was if it's still there
unchanged (same size and time). Otherwise the library's copy is used.

Hotkeys aren't carried over, because a copy shouldn't answer to its original's
keys. Set them again on the copy if you want them.

## The library

By default the library is in OBS's settings folder
(`~/.config/obs-studio/plugin_config/obs-scene-library/library`). Use **⋯ →
Change Library Folder…** to keep it somewhere that's backed up or shared.
Saved scenes already in the old folder aren't moved: move their folders across
yourself.

### Backing it up, or taking it to another computer

The library is just that folder: copy it to back it up, or to another
computer and point the dock at it there with **Change Library Folder…**. Its
scenes find their files in the library's copies wherever the originals aren't.
What isn't in the library has to be on the other computer already: the plugins
the scene's sources and filters come from (`./install.sh` puts this
repository's there), and the fonts its text uses.

### How it's stored

Each saved scene is a folder:

```
Interval/
  scene.json      the scene and everything in it, as OBS saves sources
  thumbnail.png   how it looked
  files/          copies of the files it uses
```

A file that's already in the library, from another saved scene, is linked
rather than copied again, so it takes no more space. Replacing a saved scene
keeps its copies of files that haven't changed where they were, so scenes
already added from it still find them. Renaming a saved scene changes its name
but not its folder's, for the same reason. Deleting a saved scene deletes its
folder. Any scenes already added from it stay in OBS, but if they use the
library's copies of files, those copies go too.

## Automating it

With obs-websocket (built into OBS), these requests are available as vendor
requests of the vendor `scene-library` (`CallVendorRequest`). That's for a
Stream Deck, Bitfocus Companion or a script:

| Request | Data | Answers |
|---|---|---|
| `ListScenes` | | `scenes`: `[{name, scene, saved, sources, files, folder, notCopied}]` |
| `SaveScene` | `sceneName` (default: the scene being edited), `name` (default: the scene's name), `replace` (default: false) | `name` |
| `LoadScene` | `name` | `sceneName` (its name in OBS), `missingTypes`, `missingFiles`, `reused`, `otherCanvas` |
| `RenameScene` | `name`, `newName` | |
| `DeleteScene` | `name` | |

Each answers `ok: true`, or `ok: false` with an `error`. For example, to add
the saved scene "Interval, blue" from a script or a Companion button, send
obs-websocket's `CallVendorRequest` with:

```json
{
  "vendorName": "scene-library",
  "requestType": "LoadScene",
  "requestData": { "name": "Interval, blue" }
}
```

The answer's `responseData.sceneName` is the scene's name in OBS (`Interval,
blue`, or `Interval, blue 2` if that's taken), ready for `SetCurrentProgramScene`.

## What isn't kept

- Desktop Audio and Mic/Aux, OBS's global audio sources, aren't part of any
  scene.
- A browser source showing a local HTML file has that file copied, but not the
  files it uses.
- A folder bigger than 4 GB or 5,000 files isn't copied, and the dock says so
  when you save. Single files are always copied, however big.
- Positions are in canvas pixels. A scene saved from a canvas of another size
  is added anyway, with a warning that things may not sit where they did.
- A source whose type isn't installed (its plugin is missing) is still added,
  and kept, but shows nothing. You're told which types are missing.

## Tests

[tests/scene-library](../../tests/scene-library) checks it in a real OBS
through obs-websocket: saving, adding back beside the original and comparing
the frames, changed and missing files, linking files, renaming, deleting and
Studio Mode. Its unit tests check the changes made to a saved scene to add it
back (names, references, uuids, files).
