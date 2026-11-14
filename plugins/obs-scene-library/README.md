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

## Using it

The **Scene Library** dock appears beside Scenes the first time OBS runs with
the plugin. After that it's under **Docks** like any other dock.

- **Save Scene…** saves the scene you're editing: the one showing or, in
  Studio Mode, the one in the preview. You name it (its scene's name to start
  with). Saving under a name that's already in the library asks before
  replacing it.
- **Add to Scenes** (or double-click it) adds the selected saved scene to OBS's
  scene list. Nothing goes live: in Studio Mode it's put in the preview;
  otherwise it's added to the list and what's showing stays as it was.
- Right-click a saved scene to replace it with the scene you're editing as it
  is now, rename it, delete it, or show its folder. **Delete** and **F2** work
  too.
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

Each saved scene is a folder:

```
Interval/
  scene.json      the scene and everything in it, as OBS saves sources
  thumbnail.png   how it looked
  files/          copies of the files it uses
```

A file that's already in the library, from another saved scene, is linked
rather than copied again, so it takes no more space. Deleting a saved scene
deletes its folder. Any scenes already added from it stay in OBS, but if they
use the library's copies of files, those copies go too.

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

Each answers `ok: true`, or `ok: false` with an `error`.

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
