#!/usr/bin/env python3
"""Tests for the Scene Library in a real OBS: the installed OBS and plugins, on a
virtual display (needs Xvfb, and the Python packages numpy, pillow and
websocket-client). The library is driven through its obs-websocket requests.

    tests/scene-library/test_scene_library.py [--out DIR]

--out keeps the frames, thumbnail and screenshot it took, to look at.

1. Saving a scene keeps all of it (nested scenes, a group, filters, audio
   settings) and nothing else, copies the files it uses, and has a thumbnail of
   it.
2. Adding it back beside the original makes a copy of its own that looks the
   same, renaming what clashes (and settings that refer to those by name, but
   not text that happens to read the same), and leaves the original alone.
3. When a file it used has changed or gone, the library's copy is used.
4. Files already in the library aren't stored twice.
5. Replacing, renaming and deleting saved scenes; a source type that isn't
   installed is reported, not fatal.
6. Adding never changes what's live: in Studio Mode the scene goes into the
   preview, which is also what's saved when no scene is named.
"""

import argparse
import json
import os
import shutil
import sys
import tempfile
import time

import numpy as np
from PIL import Image

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(TESTS, "typeset"))
from obs_session import CANVAS, ObsSession  # noqa: E402

failed = []


def check(ok, what, detail=""):
    print(("ok    " if ok else "FAIL  ") + what + (("  (" + str(detail) + ")") if detail else ""))
    if not ok:
        failed.append(what)
    return ok


def item(name, i, x=0, y=0, sx=1.0, rot=0.0, **extra):
    return dict({"name": name, "id": i, "visible": True, "locked": False, "pos": {"x": x, "y": y},
                 "scale": {"x": sx, "y": sx}, "rot": rot, "align": 5, "bounds_type": 0}, **extra)


def colour(name, abgr, w, h):
    return {"id": "color_source", "versioned_id": "color_source_v3", "name": name,
            "settings": {"color": abgr, "width": w, "height": h}}


def collection(logo):
    fade = {"id": "fade_transition", "versioned_id": "fade_transition", "name": "Fade", "transition": {},
            "duration": 300}
    look_items = [
        item("Background", 1),
        item("Logo", 2, 300, 200, 2.0, 10.0, show_transition=fade),
        item("Caption", 3, 100, 900),
        item("Ref", 4, 1600, 100),
        item("Inner", 5, 1200, 500, 0.25),
        item("Group", 6, 200, 600),
        item("Sound", 7),
    ]
    return {
        "name": "Test", "current_scene": "Look", "current_program_scene": "Look",
        "scene_order": [{"name": "Look"}, {"name": "Inner"}, {"name": "Other"}],
        "sources": [
            {"id": "scene", "versioned_id": "scene", "name": "Look",
             "settings": {"id_counter": len(look_items), "items": look_items}},
            {"id": "scene", "versioned_id": "scene", "name": "Inner",
             "settings": {"id_counter": 1, "items": [item("Inner square", 1, 400, 200)]}},
            {"id": "scene", "versioned_id": "scene", "name": "Other",
             "settings": {"id_counter": 1, "items": [item("Other source", 1)]}},
            {"id": "group", "versioned_id": "group", "name": "Group",
             "settings": {"id_counter": 1, "custom_size": True, "cx": 300, "cy": 200,
                          "items": [item("Group box", 1)]}},
            colour("Background", 0xFF3C1428, CANVAS[0], CANVAS[1]),
            colour("Inner square", 0xFF20C0F0, 800, 500),
            colour("Group box", 0xFF40E040, 300, 200),
            colour("Other source", 0xFF0000FF, 64, 64),
            # A setting naming another source (as some plugins' do): follows a rename.
            dict(colour("Ref", 0xFFFFFFFF, 100, 100), settings={
                "color": 0xFFFFFFFF, "width": 100, "height": 100, "linked_source": "Logo"}),
            {"id": "image_source", "versioned_id": "image_source", "name": "Logo",
             "settings": {"file": logo},
             "filters": [
                 {"id": "grove_invert_filter", "versioned_id": "grove_invert_filter", "name": "Invert",
                  "enabled": True, "settings": {}},
                 {"id": "color_filter", "versioned_id": "color_filter_v2", "name": "Colour Correction",
                  "enabled": True, "settings": {"gamma": 0.4, "saturation": -0.5}}]},
            # Text that reads the same as a source's name stays as it is.
            {"id": "typeset_text", "versioned_id": "typeset_text", "name": "Caption",
             "settings": {"text": "Logo", "font": {"face": "DejaVu Sans", "style": "Book", "size": 72, "flags": 0}}},
            {"id": "ffmpeg_source", "versioned_id": "ffmpeg_source", "name": "Sound", "settings": {},
             "muted": True, "volume": 0.5},
        ],
    }


class Library:
    def __init__(self, obs):
        self.obs = obs
        self.dir = os.path.join(obs.dir, ".config", "obs-studio", "plugin_config", "obs-scene-library", "library")

    def __call__(self, kind, **data):
        r = self.obs.request("CallVendorRequest", vendorName="scene-library", requestType=kind, requestData=data)
        return r["responseData"]

    def entries(self):
        return {e["name"]: e for e in self("ListScenes")["scenes"]}

    def json(self, name):
        with open(os.path.join(self.entries()[name]["folder"], "scene.json")) as f:
            return json.load(f)


def item_sources(obs, scene, group=False):
    kind = "GetGroupSceneItemList" if group else "GetSceneItemList"
    return [i["sourceName"] for i in obs.request(kind, sceneName=scene)["sceneItems"]]


def item_id(obs, scene, source):
    return obs.request("GetSceneItemId", sceneName=scene, sourceName=source)["sceneItemId"]


def transform(obs, scene, source):
    t = obs.request("GetSceneItemTransform", sceneName=scene, sceneItemId=item_id(obs, scene, source))
    t = t["sceneItemTransform"]
    return {k: t[k] for k in ("positionX", "positionY", "scaleX", "scaleY", "rotation")}


def filters(obs, source):
    return [(f["filterName"], f["filterKind"], f["filterEnabled"], f["filterSettings"])
            for f in obs.request("GetSourceFilterList", sourceName=source)["filters"]]


def difference(a, b):
    d = np.abs(a.astype(int) - b.astype(int))[..., :3]
    return d.mean(), d.max()


def save(out, name, image):
    if out:
        Image.fromarray(image).save(os.path.join(out, name))


def wait_for_frame(obs, scene, settle=1.0):
    time.sleep(settle)
    return obs.frame(scene)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out")
    args = ap.parse_args()
    if args.out:
        os.makedirs(args.out, exist_ok=True)

    assets = tempfile.mkdtemp(prefix="scene-library-assets-")
    logo = os.path.join(assets, "logo.png")
    shutil.copy(os.path.join(TESTS, "fixtures", "logo-alpha.png"), logo)

    try:
        with ObsSession(collection(logo)) as obs:
            run(obs, Library(obs), logo, args.out)
            log = obs.log()
            if args.out:
                with open(os.path.join(args.out, "obs.log"), "w") as f:
                    f.write(log)
    finally:
        shutil.rmtree(assets, ignore_errors=True)

    print()
    if failed:
        print("%d failed:" % len(failed))
        for f in failed:
            print("  " + f)
        sys.exit(1)
    print("all passed")


def run(obs, lib, logo, out):
    look = wait_for_frame(obs, "Look")
    save(out, "look.png", look)

    # --- 1. Saving ---------------------------------------------------------------------------
    print("-- saving")
    r = lib("SaveScene", sceneName="Look", name="Interval")
    check(r.get("ok"), "SaveScene answers ok", r.get("error"))
    entries = lib.entries()
    check("Interval" in entries, "the saved scene is listed", list(entries))
    saved = lib.json("Interval")
    names = sorted(s["name"] for s in saved["sources"])
    want = sorted(["Look", "Background", "Logo", "Caption", "Ref", "Inner", "Inner square", "Group", "Group box",
                   "Sound"])
    check(names == want, "it has the scene, its nested scene and group, and everything in them, and nothing else",
          names)
    check(saved["scene"] == "Look" and saved["canvas"] == {"width": CANVAS[0], "height": CANVAS[1]},
          "it records the scene's name and the canvas", (saved["scene"], saved["canvas"]))
    logo_saved = next(s for s in saved["sources"] if s["name"] == "Logo")
    check([f["name"] for f in logo_saved.get("filters", [])] == ["Invert", "Colour Correction"],
          "the filters are saved, in order", logo_saved.get("filters"))
    files = saved.get("files", [])
    check(len(files) == 1 and files[0]["path"] == logo, "the logo file is recorded", files)
    folder = entries["Interval"]["folder"]
    if files:
        copy = os.path.join(folder, files[0]["stored"])
        check(os.path.isfile(copy) and open(copy, "rb").read() == open(logo, "rb").read(),
              "and copied into the library", copy)
    thumb_path = os.path.join(folder, "thumbnail.png")
    if check(os.path.isfile(thumb_path), "there's a thumbnail"):
        thumb = np.asarray(Image.open(thumb_path).convert("RGB"))
        expect = np.asarray(Image.fromarray(look).convert("RGB").resize((thumb.shape[1], thumb.shape[0]),
                                                                         Image.LANCZOS))
        mean, _ = difference(thumb, expect)
        check(thumb.shape[1] == 480 and mean < 6, "the thumbnail is the scene, 480 wide",
              "%dx%d, mean difference %.1f" % (thumb.shape[1], thumb.shape[0], mean))
        save(out, "thumbnail.png", thumb)

    # --- 2. Adding it back beside the original ---------------------------------------------------
    print("-- adding it back beside the original")
    before = {s: obs.settings(s) for s in ("Logo", "Ref", "Caption")}
    look_filters = filters(obs, "Logo")
    r = lib("LoadScene", name="Interval")
    check(r.get("ok") and r.get("sceneName") == "Interval", "LoadScene adds it as 'Interval'", r)
    check(not r.get("missingTypes") and not r.get("missingFiles"), "nothing is missing", r)
    program = obs.request("GetCurrentProgramScene")["currentProgramSceneName"]
    check(program == "Look", "what's live isn't changed", program)
    scenes = [s["sceneName"] for s in obs.request("GetSceneList")["scenes"]]
    check("Interval" in scenes and "Inner 2" in scenes, "it and its nested scene are in the scene list", scenes)
    got = item_sources(obs, "Interval")
    check(got == ["Background 2", "Logo 2", "Caption 2", "Ref 2", "Inner 2", "Group 2", "Sound 2"],
          "its sources are copies, renamed where the names were taken", got)
    check(item_sources(obs, "Group 2", group=True) == ["Group box 2"], "the group's copy has its own copy inside",
          item_sources(obs, "Group 2", group=True))
    check(filters(obs, "Logo 2") == look_filters, "the copy has the same filters, with the same settings",
          filters(obs, "Logo 2"))
    check(obs.settings("Ref 2").get("linked_source") == "Logo 2", "a setting naming a source follows its rename",
          obs.settings("Ref 2").get("linked_source"))
    check(obs.settings("Caption 2").get("text") == "Logo", "text that reads like a source's name is left alone",
          obs.settings("Caption 2").get("text"))
    check(obs.settings("Logo 2").get("file") == logo, "a file still as it was is used where it is",
          obs.settings("Logo 2").get("file"))
    check(transform(obs, "Interval", "Logo 2") == transform(obs, "Look", "Logo"), "the item's transform is the same",
          transform(obs, "Interval", "Logo 2"))
    mute = obs.request("GetInputMute", inputName="Sound 2")["inputMuted"]
    vol = obs.request("GetInputVolume", inputName="Sound 2")["inputVolumeMul"]
    check(mute and abs(vol - 0.5) < 1e-4, "audio settings come back (muted, volume)", (mute, vol))
    interval = wait_for_frame(obs, "Interval")
    save(out, "interval.png", interval)
    mean, worst = difference(interval, look)
    check(worst <= 2, "it looks the same as the original", "mean %.2f, largest %d" % (mean, worst))
    check({s: obs.settings(s) for s in ("Logo", "Ref", "Caption")} == before and filters(obs, "Logo") == look_filters,
          "the original's sources are left as they were")
    check(item_sources(obs, "Look") == ["Background", "Logo", "Caption", "Ref", "Inner", "Group", "Sound"],
          "the original scene is left as it was", item_sources(obs, "Look"))

    # The item's show transition came back too: save the copy and look.
    r = lib("SaveScene", sceneName="Interval", name="Interval copy")
    saved_copy = lib.json("Interval copy")
    copy_scene = next(s for s in saved_copy["sources"] if s["name"] == "Interval")
    logo_item = next(i for i in copy_scene["settings"]["items"] if i["name"] == "Logo 2")
    show = logo_item.get("show_transition", {})
    check(show.get("id") == "fade_transition" and show.get("duration") == 300,
          "the item's show transition comes back", show)
    check(all(s.get("uuid") not in {t.get("uuid") for t in saved["sources"]} for s in saved_copy["sources"]),
          "the copies have uuids of their own")

    # --- 3. A file that's changed --------------------------------------------------------------
    print("-- a file that's changed since")
    Image.new("RGBA", (320, 180), (255, 0, 0, 255)).save(logo)
    r = lib("LoadScene", name="Interval")
    check(r.get("ok") and r.get("sceneName") == "Interval 2", "it's added again, as 'Interval 2'", r)
    path = obs.settings("Logo 3").get("file", "")
    check(path.startswith(lib.dir), "the changed file is replaced by the library's copy", path)
    again = wait_for_frame(obs, "Interval 2")
    mean, worst = difference(again, look)
    check(worst <= 2, "so it looks as it did when it was saved", "mean %.2f, largest %d" % (mean, worst))
    os.remove(logo)
    r = lib("LoadScene", name="Interval")
    check(r.get("ok") and obs.settings("Logo 4").get("file", "").startswith(lib.dir),
          "and so is one that's gone", r)

    # Replacing the saved scene with that scene (which uses the library's copy)
    # keeps the copy where it is, and records it as the file it's a copy of.
    used = obs.settings("Logo 4").get("file", "")
    r = lib("SaveScene", sceneName=r.get("sceneName"), name="Interval", replace=True)
    check(r.get("ok") and os.path.isfile(used),
          "replacing a saved scene keeps its copies where scenes added from it find them", used)
    replaced = lib.json("Interval")
    logo4 = next(s for s in replaced["sources"] if s["name"] == "Logo 4")
    check([f["path"] for f in replaced["files"]] == [logo] and logo4["settings"]["file"] == logo,
          "and a library copy a scene uses is saved as the file it's a copy of",
          ([f["path"] for f in replaced["files"]], logo4["settings"]["file"]))

    # --- 4. Files already in the library -------------------------------------------------------
    print("-- files already in the library")
    r = lib("SaveScene", sceneName="Interval 2", name="From the copy")
    a = lib.json("From the copy")["files"][0]
    b = lib.json("Interval")["files"][0]
    ino_a = os.stat(os.path.join(lib.entries()["From the copy"]["folder"], a["stored"])).st_ino
    ino_b = os.stat(os.path.join(lib.entries()["Interval"]["folder"], b["stored"])).st_ino
    check(r.get("ok") and ino_a == ino_b, "a file the library has already is linked, not stored twice")
    r = lib("DeleteScene", name="Interval")
    check(r.get("ok") and os.path.isfile(os.path.join(lib.entries()["From the copy"]["folder"], a["stored"])),
          "and deleting the other saved scene leaves it")

    # --- 5. Replacing, renaming, deleting ------------------------------------------------------
    print("-- replacing, renaming, deleting")
    r = lib("SaveScene", sceneName="Other", name="From the copy")
    check(not r.get("ok") and "already" in r.get("error", ""), "saving over a name needs replace: true", r)
    r = lib("SaveScene", sceneName="Other", name="From the copy", replace=True)
    check(r.get("ok") and lib.json("From the copy")["scene"] == "Other", "with it, the saved scene is replaced", r)
    check(sorted(lib.entries()) == ["From the copy", "Interval copy"], "there's one of each name",
          sorted(lib.entries()))
    r = lib("RenameScene", name="From the copy", newName="Blue square")
    check(r.get("ok") and "Blue square" in lib.entries() and
          os.path.basename(lib.entries()["Blue square"]["folder"]) == "From the copy",
          "renaming changes its name, not its folder's (scenes added from it may use files there)", r)
    r = lib("RenameScene", name="Blue square", newName="Interval copy")
    check(not r.get("ok"), "it can't take another's name", r)
    r = lib("DeleteScene", name="Interval copy")
    check(r.get("ok") and sorted(lib.entries()) == ["Blue square"], "deleting removes it", r)
    check(not os.path.exists(os.path.join(lib.dir, "Interval copy")), "and its folder")
    check(not [d for d in os.listdir(lib.dir) if d.startswith(".")], "no half-saved folders are left",
          os.listdir(lib.dir))

    # A source type that isn't installed here: reported, and the rest still added.
    path = os.path.join(lib.entries()["Blue square"]["folder"], "scene.json")
    data = json.load(open(path))
    for s in data["sources"]:
        if s["name"] == "Other source":
            s["id"] = s["versioned_id"] = "no_such_source"
    json.dump(data, open(path, "w"))
    r = lib("LoadScene", name="Blue square")
    check(r.get("ok") and [t["name"] for t in r.get("missingTypes", [])] == ["no_such_source"],
          "a source type that isn't installed is reported", r)
    r = lib("LoadScene", name="Nothing")
    check(not r.get("ok") and "no saved scene" in r.get("error", ""), "loading what isn't there says so", r)

    # --- 6. Studio Mode --------------------------------------------------------------------------
    print("-- Studio Mode")
    obs.request("SetStudioModeEnabled", studioModeEnabled=True)
    time.sleep(0.5)
    r = lib("LoadScene", name="Blue square")
    preview = obs.request("GetCurrentPreviewScene")["currentPreviewSceneName"]
    program = obs.request("GetCurrentProgramScene")["currentProgramSceneName"]
    check(r.get("ok") and preview == r.get("sceneName") and program == "Look",
          "in Studio Mode it's put in the preview, and the program is left", (preview, program))
    r = lib("SaveScene", name="Saved from the preview")
    check(r.get("ok") and lib.json("Saved from the preview")["scene"] == preview,
          "and with no scene named, the preview's scene is saved", r)

    log = obs.log()
    problems = [line for line in log.splitlines()
                if ("[scene-library]" in line and ("warning" in line.lower() or "couldn't" in line)) or
                "scene_load_item" in line]
    problems = [p for p in problems if "no_such_source" not in p]
    check(not problems, "OBS reported nothing wrong", "\n".join(problems[:5]))


if __name__ == "__main__":
    main()
