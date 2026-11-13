#!/usr/bin/env python3
"""Capture a fixed set of screenshots of Typeset Text in a real OBS, for
reviewing its design: the properties dialog, editing in the preview, and
output frames of typical jobs. Same scenes, same steps, every time.

    tests/typeset/capture.py OUT_DIR

Needs what test_typeset.py needs. Writes:

  props-1-new.png        the properties dialog of a source just added
  props-1-new-more.png   the same, scrolled to the bottom
  props-2-lower-third.png  the dialog of one set up as a lower third
  props-2-lower-third-more.png  the same, scrolled to the bottom
  edit-1-start.png       the preview just after double-clicking a word
  edit-2-caret.png       the caret part-way through the text
  edit-3-selection.png   a selection over two lines
  out-1-jobs.png         an output frame: title, lower third, set times, a message
"""

import os
import subprocess
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from obs_session import CANVAS, ObsSession  # noqa: E402

LOCATOR = (12, 200, 90)

# Settings per scenario: each job set up with what the source offers for it.
# Keys a version doesn't know are ignored by it.
NEW = {}
TITLE = {"text": "The Grove\nLive tonight", "font": {"face": "Inter", "style": "Bold", "size": 140, "flags": 0},
         "align": 1}
LOWER_THIRD = {"text": "Ada Lovelace\nAnalytical Engines",
               "font": {"face": "Inter", "style": "SemiBold", "size": 64, "flags": 0}, "colour": 0xFFFFFFFF,
               "typography": True, "line_spacing": 1.1,
               "plate": True, "plate_colour": 0x281410, "plate_opacity": 85, "plate_padding": 28, "plate_radius": 6}
SET_TIMES = {"text": "19:30  Doors\n20:15  Support\n21:30  Headliner",
             "font": {"face": "Inter", "style": "Medium", "size": 48, "flags": 0}, "colour": 0xFFE0F0FF,
             "typography": True, "tabular": True}
MESSAGE = {"text": "Bar closes in 10 minutes", "font": {"face": "DejaVu Serif", "style": "Italic", "size": 56,
           "flags": 0}, "colour": 0xFFFFD060, "outline": True, "outline_width": 3}


def busy_background(path):
    """A colourful, detailed backdrop like projector visuals."""
    h, w = CANVAS[1], CANVAS[0]
    y, x = np.mgrid[0:h, 0:w] / np.array([h, w])[:, None, None].T.reshape(2, 1, 1)
    r = 0.5 + 0.5 * np.sin(9 * x + 3 * np.sin(5 * y))
    g = 0.5 + 0.5 * np.sin(7 * y + 4 * x * y + 1.3)
    b = 0.5 + 0.5 * np.sin(11 * (x + y) + 2.1)
    img = (np.stack([r * 0.8, g * 0.55, b * 0.9], axis=2) * 255).astype(np.uint8)
    Image.fromarray(img).save(path)


def source(name, settings):
    return {"id": "typeset_text", "versioned_id": "typeset_text", "name": name, "settings": settings}


def item(name, i, x, y, s=1.0):
    return {"name": name, "id": i, "visible": True, "pos": {"x": x, "y": y}, "scale": {"x": s, "y": s},
            "rot": 0.0, "align": 5, "bounds_type": 0}


def collection(backdrop):
    items = [item("Backdrop", 1, 0, 0), item("Title", 2, 560, 120), item("Lower third", 3, 120, 780),
             item("Set times", 4, 1300, 560), item("Message", 5, 1180, 960), item("New text", 6, 140, 520),
             dict(item("Locator", 7, 0, 0), visible=False)]
    return {"name": "Test", "current_scene": "Scene", "current_program_scene": "Scene",
            "scene_order": [{"name": "Scene"}],
            "sources": [{"id": "scene", "versioned_id": "scene", "name": "Scene",
                         "settings": {"id_counter": len(items), "items": items}},
                        {"id": "image_source", "versioned_id": "image_source", "name": "Backdrop",
                         "settings": {"file": backdrop}},
                        source("Title", TITLE), source("Lower third", LOWER_THIRD), source("Set times", SET_TIMES),
                        source("Message", MESSAGE), source("New text", NEW),
                        {"id": "color_source", "versioned_id": "color_source_v3", "name": "Locator",
                         "settings": {"color": 0xFF000000 | LOCATOR[2] << 16 | LOCATOR[1] << 8 | LOCATOR[0],
                                      "width": CANVAS[0], "height": CANVAS[1]}}]}


def find_window(obs, title_pattern):
    """A window, found by its title, placed and sized the same every time."""
    for _ in range(40):
        ids = obs.xdo("search", "--onlyvisible", "--name", title_pattern, check=False).split()
        if ids:
            break
        time.sleep(0.25)
    else:
        raise RuntimeError("no window like %r" % title_pattern)
    wid = ids[-1]
    obs.xdo("windowsize", wid, "900", "990")
    obs.xdo("windowmove", wid, "300", "5")
    time.sleep(1.5)
    return wid


def preview_crop(obs, path, box):
    Image.fromarray(obs.screen()).crop(box).save(path)


def main():
    out = os.path.abspath(sys.argv[1])
    os.makedirs(out, exist_ok=True)
    backdrop = os.path.join(out, ".backdrop.png")
    busy_background(backdrop)
    with ObsSession(collection(backdrop)) as obs:
        time.sleep(2)
        Image.fromarray(obs.frame()).save(os.path.join(out, "out-1-jobs.png"))

        # Editing the title in the preview. Where the canvas is drawn: a
        # plain layer over it shows its extent, then it's removed (hidden, it
        # would stay selected and change what OBS's first click selects).
        obs.request("SetSceneItemEnabled", sceneName="Scene", sceneItemId=7, sceneItemEnabled=True)
        time.sleep(0.5)
        x0, y0, scale = canvas_on_screen(obs.screen())
        obs.request("RemoveSceneItem", sceneName="Scene", sceneItemId=7)
        time.sleep(0.5)
        to_screen = lambda cx, cy: (x0 + cx * scale, y0 + cy * scale)  # noqa: E731
        crop = (int(x0), int(y0), int(round(x0 + CANVAS[0] * scale)), int(round(y0 + CANVAS[1] * scale)))

        t = obs.request("GetSceneItemTransform", sceneName="Scene", sceneItemId=2)["sceneItemTransform"]
        tx, ty, tw, th = t["positionX"], t["positionY"], t["width"], t["height"]
        obs.click(*to_screen(tx + tw * 0.3, ty + th * 0.75), repeat=2)  # "Live"
        preview_crop(obs, os.path.join(out, "edit-1-start.png"), crop)
        obs.key("Right", "Left", "Left")
        time.sleep(0.1)
        preview_crop(obs, os.path.join(out, "edit-2-caret.png"), crop)
        obs.key("ctrl+Home", "Right", "Right", "shift+Down", "shift+Right", "shift+Right")
        time.sleep(0.1)
        preview_crop(obs, os.path.join(out, "edit-3-selection.png"), crop)
        obs.key("Escape")
        time.sleep(0.5)

        # The properties dialogs.
        # Each in two parts: the top, and scrolled to the bottom (with the
        # pointer over the labels, where the wheel can't change a value).
        for name, file in (("New text", "props-1-new"), ("Lower third", "props-2-lower-third")):
            obs.request("OpenInputPropertiesDialog", inputName=name)
            wid = find_window(obs, "Properties for '%s'" % name)
            geometry = obs.xdo("getwindowgeometry", wid)
            x, y = [int(v) for v in geometry.split("Position: ")[1].split(" ")[0].split(",")]
            # A smaller preview in the dialog (its splitter dragged up), so
            # more of the settings show.
            obs.drag(x + 450, y + 472, x + 450, y + 200)
            time.sleep(0.5)
            subprocess.run(["import", "-window", wid, os.path.join(out, file + ".png")], env=obs.env, check=True)
            obs.xdo("mousemove", str(x + 30), str(y + 800))
            for _ in range(40):
                obs.xdo("click", "5")
            time.sleep(0.5)
            subprocess.run(["import", "-window", wid, os.path.join(out, file + "-more.png")], env=obs.env, check=True)
            obs.xdo("windowclose", wid, check=False)
            time.sleep(1)

    os.remove(backdrop)


def canvas_on_screen(screen):
    """Where the canvas is drawn in the preview: the locator's extent."""
    match = np.abs(screen.astype(int) - np.array(LOCATOR)).sum(axis=2) <= 3
    rows = np.nonzero(match.sum(axis=1) > 50)[0]
    cols = np.nonzero(match.sum(axis=0) > 50)[0]
    return cols.min(), rows.min(), (cols.max() + 1 - cols.min()) / CANVAS[0]


if __name__ == "__main__":
    main()
