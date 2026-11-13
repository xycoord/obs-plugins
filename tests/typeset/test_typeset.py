#!/usr/bin/env python3
"""Tests for Typeset Text in a real OBS: the installed OBS and plugins, on a
virtual display (needs Xvfb, xdotool, ImageMagick's import, and the Python
packages numpy, pillow and websocket-client).

    tests/typeset/test_typeset.py [--out DIR]

--out keeps the frames and screenshots it took, to look at.

1. Scaling is like a vector drawing: text at 24 px scaled 6x in the scene comes
   out the same as text at 144 px unscaled, and the same goes for shrinking,
   stretching one way and a position part-way across a pixel.
2. Editing in the preview: double-clicking the text in OBS's preview selects
   the word clicked on; typing replaces it; the keys move and select as in any
   editor; the caret and selection show in the preview but never in the output.
"""

import argparse
import os
import re
import sys
import time

import numpy as np
from PIL import Image

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from obs_session import CANVAS, ObsSession  # noqa: E402

BG = (40, 20, 60)
BG_ABGR = 0xFF000000 | BG[2] << 16 | BG[1] << 8 | BG[0]
FONT = "DejaVu Sans"

failed = []


def check(ok, what, detail=""):
    print(("ok    " if ok else "FAIL  ") + what + (("  (" + detail + ")") if detail else ""))
    if not ok:
        failed.append(what)


def font(size, style="Book", face=FONT):
    return {"face": face, "style": style, "size": size, "flags": 0}


def text_source(name, text, size, **settings):
    # Plain text: no shadow (on by default), so scaled renders can be compared
    # pixel for pixel.
    return {"id": "typeset_text", "versioned_id": "typeset_text", "name": name,
            "settings": dict({"text": text, "font": font(size), "shadow": False}, **settings)}


def item(name, i, x, y, sx=1.0, sy=None):
    return {"name": name, "id": i, "visible": True, "locked": False, "pos": {"x": x, "y": y},
            "scale": {"x": sx, "y": sy if sy is not None else sx}, "rot": 0.0, "align": 5, "bounds_type": 0}


def collection(sources, items):
    items = [item("Background", 1, 0, 0)] + items
    return {"name": "Test", "current_scene": "Scene", "current_program_scene": "Scene",
            "scene_order": [{"name": "Scene"}],
            "sources": [{"id": "scene", "versioned_id": "scene", "name": "Scene",
                         "settings": {"id_counter": len(items), "items": items}},
                        {"id": "color_source", "versioned_id": "color_source_v3", "name": "Background",
                         "settings": {"color": BG_ABGR, "width": CANVAS[0], "height": CANVAS[1]}}] + sources}


def grey(a):
    return a[..., :3].astype(np.float64).mean(axis=2)


def text_bbox(frame):
    """Where anything other than the background is, in a frame."""
    diff = np.abs(frame[..., :3].astype(int) - np.array(BG)).sum(axis=2) > 6
    ys, xs = np.nonzero(diff)
    return (xs.min(), ys.min(), xs.max() + 1, ys.max() + 1) if len(xs) else None


def region(frame, x, y, w, h):
    return grey(frame[y:y + h, x:x + w])


def sharpness(a):
    """Total contrast across neighbouring pixels: less when blurred."""
    return np.abs(np.diff(a, axis=0)).sum() + np.abs(np.diff(a, axis=1)).sum()


def steepest_edge(a):
    """The largest step between neighbouring pixels, along rows."""
    return np.abs(np.diff(a, axis=1)).max()


def drawing_times(obs):
    """How long each drawing of a text took, in order (TYPESET_DEBUG's log)."""
    return [float(m) for m in re.findall(r"\[obs-typeset\] drew .* in ([0-9.]+) ms", obs.log())]


def drawings(obs):
    return len(drawing_times(obs))


def difference(a, b):
    """Mean absolute difference, in levels of 255, and the largest."""
    d = np.abs(a - b)
    return d.mean(), d.max()


def save(out, name, image):
    if out:
        Image.fromarray(image).save(os.path.join(out, name))


# --- 1. Scaling ------------------------------------------------------------------------------

def test_scaling(out):
    print("-- scaling")
    w, h = 900, 200  # the regions compared (canvas pixels), enough for "Sharp Ag" at 144 px
    sources = [
        text_source("Scaled up", "Sharp Ag", 24),
        text_source("Reference", "Sharp Ag", 144),
        text_source("Scaled down", "Sharp Ag", 576),
        text_source("Scaled 3x", "Sharp Ag", 48),
        text_source("Off-grid", "Sharp Ag", 72),
        {"id": "text_ft2_source", "versioned_id": "text_ft2_source_v2", "name": "OBS text",
         "settings": {"text": "Sharp Ag", "font": font(24)}},
    ]
    items = [item("Scaled up", 2, 0, 0, 6.0), item("Reference", 3, 960, 0),
             item("Scaled down", 4, 0, 220, 0.25), item("Scaled 3x", 5, 960, 220, 3.0),
             item("Off-grid", 6, 0.5, 440.25, 2.0), item("OBS text", 7, 960, 440, 6.0)]
    with ObsSession(collection(sources, items)) as obs:
        time.sleep(2)
        frame = obs.frame()
        save(out, "scaling.png", frame)

        ref = region(frame, 960, 0, w, h)
        for name, x, y in [("scaled up 6x", 0, 0), ("scaled down to a quarter", 0, 220),
                           ("at 48 px scaled 3x", 960, 220)]:
            mean, peak = difference(region(frame, x, y, w, h), ref)
            check(mean < 0.5 and peak < 24, "text %s matches 144 px text" % name,
                  "mean difference %.3f, largest %d of 255" % (mean, peak))

        # Half a pixel across and a quarter down: drawn for where it is, as
        # sharp as on the pixel grid (resampled, it would lose about a fifth
        # of its contrast at edges).
        off = region(frame, 0, 440, w, h)
        ratio = sharpness(off) / sharpness(ref)
        check(ratio > 0.97, "text placed part-way across a pixel is as sharp as text on the pixel grid",
              "edge contrast %.1f%% of the reference" % (100 * ratio))

        # Nothing is drawn again while nothing changes.
        drawn = drawings(obs)
        time.sleep(3)
        check(drawings(obs) == drawn, "no text is drawn again while nothing changes",
              "%d drawings, then %d after 3 s" % (drawn, drawings(obs)))

        obs_text = region(frame, 960, 440, w, h)
        mean_obs, _ = difference(obs_text, ref)
        print("      for comparison, OBS's own text at 24 px scaled 6x differs from 144 px text by %.2f "
              "on average" % mean_obs)

        # Changing the scale in the scene redraws the text for its new size.
        scene_items = obs.request("GetSceneItemList", sceneName="Scene")["sceneItems"]
        ids = {i["sourceName"]: i["sceneItemId"] for i in scene_items}
        obs.request("SetSceneItemTransform", sceneName="Scene", sceneItemId=ids["Scaled up"],
                    sceneItemTransform={"scaleX": 3.0, "scaleY": 3.0})
        obs.request("SetSceneItemTransform", sceneName="Scene", sceneItemId=ids["Reference"],
                    sceneItemTransform={"scaleX": 0.5, "scaleY": 0.5})
        time.sleep(0.5)
        frame = obs.frame()
        save(out, "rescaled.png", frame)
        mean, peak = difference(region(frame, 0, 0, 450, 100), region(frame, 960, 0, 450, 100))
        check(mean < 0.5 and peak < 24, "after rescaling in the scene, 24 px at 3x matches 144 px at 0.5x",
              "mean difference %.3f, largest %d" % (mean, peak))


# --- 2. Editing in the preview -----------------------------------------------------------------

def canvas_on_screen(screen):
    """Where the canvas is drawn in the preview: the background's extent,
    across, and its top. (With no window manager, a part of the screen OBS's
    window no longer covers isn't repainted, so old pixels can linger below.)"""
    match = np.abs(screen.astype(int) - np.array(BG)).sum(axis=2) <= 3
    per_col, per_row = match.sum(axis=0), match.sum(axis=1)
    cols = np.nonzero(per_col > per_col.max() / 2)[0]
    rows = np.nonzero(per_row > per_row.max() / 2)[0]
    scale = (cols.max() + 1 - cols.min()) / CANVAS[0]
    return cols.min(), rows.min(), scale, scale


def test_editing(out):
    print("-- editing in the preview")
    sources = [text_source("Title", "Hello world", 160), text_source("Other", "Leave me", 100)]
    items = [item("Title", 2, 200, 200), item("Other", 3, 200, 700)]
    with ObsSession(collection(sources, items)) as obs:
        time.sleep(1)
        screen = obs.screen()
        x0, y0, sx, sy = canvas_on_screen(screen)
        to_screen = lambda cx, cy: (x0 + cx * sx, y0 + cy * sy)  # noqa: E731

        # Where the words are, from the output.
        frame = obs.frame()
        bx0, by0, bx1, by1 = text_bbox(frame[150:500, :])
        by0 += 150
        by1 += 150
        mid_y = (by0 + by1) / 2
        hello = to_screen(bx0 + (bx1 - bx0) * 0.2, mid_y)
        world = to_screen(bx0 + (bx1 - bx0) * 0.75, mid_y)

        # Double-click "world": it's selected, and typing replaces it.
        obs.click(*world, repeat=2)
        editing = obs.screen()
        save(out, "edit-1-word-selected.png", editing)
        obs.type("there")
        check(obs.settings("Title")["text"] == "Hello there", "double-click selects the word, typing replaces it",
              repr(obs.settings("Title")["text"]))

        # One key: the text is drawn once more, in a few milliseconds.
        drawn = drawings(obs)
        obs.type("!")
        time.sleep(0.5)
        times = drawing_times(obs)[drawn:]
        check(len(times) == 1 and times[0] < 30, "a key costs one drawing of the text, in a few milliseconds",
              "%s ms" % ", ".join("%.1f" % t for t in times))
        obs.key("BackSpace")

        # Keys: Home, then Shift+Ctrl+Right selects the first word; type over it.
        obs.key("Home", "shift+ctrl+Right")
        obs.type("Goodbye")
        check(obs.settings("Title")["text"] == "Goodbye there", "Home and Shift+Ctrl+Right select a word",
              repr(obs.settings("Title")["text"]))

        # A new line, Backspace, and End.
        obs.key("End", "Return")
        obs.type("Second line")
        obs.key("BackSpace", "BackSpace", "BackSpace", "BackSpace")
        obs.type("row")
        check(obs.settings("Title")["text"] == "Goodbye there\nSecond row", "Return, End and Backspace",
              repr(obs.settings("Title")["text"]))

        save(out, "edit-2-caret.png", obs.screen())

        # Up keeps to the column; Ctrl+Z undoes the last run of typing.
        obs.key("Up")
        obs.type("~")
        text = obs.settings("Title")["text"]
        check(text.startswith("Goodbye th") and "~" in text.split("\n")[0], "Up moves to the line above",
              repr(text))
        obs.key("ctrl+z")
        check(obs.settings("Title")["text"] == "Goodbye there\nSecond row", "Ctrl+Z undoes",
              repr(obs.settings("Title")["text"]))

        # Select all with the caret and selection showing: the preview shows
        # them, the output doesn't.
        obs.key("ctrl+a")
        time.sleep(0.3)
        shown = obs.screen()
        save(out, "edit-3-select-all.png", shown)
        output_while_editing = obs.frame()

        # Keys go to the text, not to OBS: Delete would otherwise remove the
        # source from the scene.
        obs.key("Delete")
        obs.type("Typeset")
        names = [i["sourceName"] for i in obs.request("GetSceneItemList", sceneName="Scene")["sceneItems"]]
        check("Title" in names and obs.settings("Title")["text"] == "Typeset",
              "Delete deletes text, not the source", repr(obs.settings("Title")["text"]))

        # Escape finishes. The output then is what it was while editing
        # (same text): nothing of the editing ever reached it.
        obs.key("ctrl+z", "ctrl+z")  # back to "Goodbye there\nSecond row"
        obs.key("Escape")
        time.sleep(0.3)
        output_after = obs.frame()
        check(obs.settings("Title")["text"] == "Goodbye there\nSecond row", "undoing back through the edits",
              repr(obs.settings("Title")["text"]))
        mean, peak = difference(grey(output_while_editing), grey(output_after))
        check(peak == 0, "no caret, selection or frame in the output while editing",
              "largest difference %d" % peak)
        after_screen = obs.screen()
        save(out, "edit-4-finished.png", after_screen)
        preview_diff = np.abs(shown.astype(int) - after_screen.astype(int)).sum(axis=2) > 30
        check(preview_diff.sum() > 500, "the selection shows in the preview", "%d pixels" % preview_diff.sum())

        # After Escape, typing doesn't change the text.
        obs.type("zzz")
        check(obs.settings("Title")["text"] == "Goodbye there\nSecond row", "after Escape keys don't edit")

        # Clicking on the text once doesn't start editing (it selects the
        # item, as in OBS); a drag on it still moves the item.
        ox0, oy0, ox1, oy1 = text_bbox(obs.frame()[650:900, :])
        other = to_screen((ox0 + ox1) / 2, 650 + (oy0 + oy1) / 2)
        obs.click(*other)
        obs.type("x")
        check(obs.settings("Other")["text"] == "Leave me", "a single click doesn't start editing")
        before = obs.request("GetSceneItemTransform", sceneName="Scene", sceneItemId=3)["sceneItemTransform"]
        obs.drag(other[0], other[1], other[0] + 40, other[1] + 20)
        after = obs.request("GetSceneItemTransform", sceneName="Scene", sceneItemId=3)["sceneItemTransform"]
        check(after["positionX"] > before["positionX"] + 20, "dragging the text still moves it",
              "x %.0f -> %.0f" % (before["positionX"], after["positionX"]))

        # Resizing it by its corner handle in the preview, to about 3x: it's
        # redrawn for its new size, as sharp as ever (scaled as a picture, its
        # edges would be spread over three pixels).
        t = obs.request("GetSceneItemTransform", sceneName="Scene", sceneItemId=3)["sceneItemTransform"]
        corner = to_screen(t["positionX"] + t["width"], t["positionY"] + t["height"])
        target = to_screen(t["positionX"] + t["width"] * 3, t["positionY"] + t["height"] * 3)
        obs.drag(corner[0], corner[1], target[0], target[1])
        time.sleep(0.5)
        t = obs.request("GetSceneItemTransform", sceneName="Scene", sceneItemId=3)["sceneItemTransform"]
        frame = obs.frame()
        save(out, "resized-by-handle.png", frame)
        x, y = int(t["positionX"]), int(t["positionY"])
        edge = steepest_edge(region(frame, max(x, 0), max(y, 0), min(int(t["width"]), CANVAS[0] - x),
                                    min(int(t["height"]), CANVAS[1] - y)))
        check(t["scaleX"] > 2.0 and edge > 150, "resized by its handle to %.2fx, its edges are still sharp"
              % t["scaleX"], "steepest step between pixels %d of 255" % edge)

        # Clicking off the text finishes editing.
        title_now = to_screen(bx0 + 30, mid_y)
        obs.click(*title_now, repeat=2)
        obs.click(*to_screen(1700, 1000))
        obs.type("q")
        check("q" not in obs.settings("Title")["text"], "a click outside the text finishes editing")

        warnings = [line for line in obs.log().splitlines() if "[obs-typeset]" in line and "editing" not in line and "] drew " not in line]
        check(not warnings, "no warnings from obs-typeset", "; ".join(warnings))


# --- 3. Features: text box, F2, hotkeys, Studio Mode ------------------------------------------

def test_features(out):
    print("-- text box, F2, hotkeys and Studio Mode")
    sources = [text_source("Title", "Hello world", 160), text_source("Boxed", "Short", 60)]
    items = [item("Title", 2, 200, 200), item("Boxed", 3, 200, 700)]
    scene = collection(sources, items)
    # A second scene, switched to by the key 1.
    scene["scene_order"].append({"name": "Other scene"})
    scene["sources"].append({"id": "scene", "versioned_id": "scene", "name": "Other scene",
                             "settings": {"id_counter": 0, "items": []},
                             "hotkeys": {"OBSBasic.SelectScene": [{"key": "OBS_KEY_1"}]}})
    with ObsSession(scene) as obs:
        time.sleep(1)
        ids = {i["sourceName"]: i["sceneItemId"]
               for i in obs.request("GetSceneItemList", sceneName="Scene")["sceneItems"]}

        def size(name):
            t = obs.request("GetSceneItemTransform", sceneName="Scene", sceneItemId=ids[name])["sceneItemTransform"]
            return t["sourceWidth"], t["sourceHeight"]

        # A text box: a fixed width the text wraps in, or shrinks to fit.
        long_text = "A rather long band name that will not fit"
        obs.request("SetInputSettings", inputName="Boxed", inputSettings={
            "text": long_text, "box": True, "box_width": 400, "overflow": 0})
        time.sleep(0.5)
        w, h = size("Boxed")
        check(w == 400 and h > 60 * 2.5, "a text box wraps at its width", "%dx%d" % (w, h))
        obs.request("SetInputSettings", inputName="Boxed", inputSettings={"overflow": 1})
        time.sleep(0.5)
        w, h = size("Boxed")
        check(w == 400 and h < 60 * 1.2, "shrink to fit keeps it to one line in the box", "%dx%d" % (w, h))
        obs.request("SetInputSettings", inputName="Boxed", inputSettings={"text": "Short"})
        time.sleep(0.5)
        w2, _ = size("Boxed")
        check(w2 == 400, "the box keeps its width when the text gets shorter", "%d" % w2)

        x0, y0, sx, sy = canvas_on_screen(obs.screen())
        to_screen = lambda cx, cy: (x0 + cx * sx, y0 + cy * sy)  # noqa: E731

        # While editing a text box, its width grip (on the editing frame's
        # right edge) sets the box's width: dragged right by 50 preview
        # pixels, it's that much wider in the text's own pixels.
        bw, bh = size("Boxed")
        obs.click(*to_screen(210, 700 + bh / 2), repeat=2)
        grip = to_screen(200 + bw, 700 + bh / 2)
        obs.drag(grip[0] + 7, grip[1], grip[0] + 57, grip[1])
        save(out, "features-grip.png", obs.screen())
        obs.key("Escape")
        new_width = obs.settings("Boxed").get("box_width")
        expected = 400 + 50 / sx
        check(new_width is not None and abs(new_width - expected) < 4 / sx,
              "dragging the text box's grip sets its width", "%s (about %.0f expected)" % (new_width, expected))

        tw, th = size("Title")
        title = to_screen(200 + tw * 0.7, 200 + th / 2)

        # F2 on the selected text edits all of it.
        obs.click(*title)
        obs.key("F2")
        obs.type("Fresh")
        check(obs.settings("Title")["text"] == "Fresh", "F2 edits the selected text, all of it selected",
              repr(obs.settings("Title")["text"]))
        save(out, "features-f2.png", obs.screen())

        # Typing a key bound to an OBS hotkey types it, and doesn't switch scenes.
        # Held down as long as a person holds a key, so OBS's own reading of
        # the keyboard would see it.
        obs.key("End")
        obs.xdo("keydown", "1")
        time.sleep(0.2)
        obs.xdo("keyup", "1")
        time.sleep(0.5)
        current = obs.request("GetCurrentProgramScene")["currentProgramSceneName"]
        check(current == "Scene" and obs.settings("Title")["text"] == "Fresh1",
              "while editing, a hotkey's key is typed, not acted on",
              "scene %r, text %r" % (current, obs.settings("Title")["text"]))
        obs.key("Escape")
        # ... which it would have been otherwise (or the check above proves nothing).
        obs.click(*title)
        obs.key("1")
        time.sleep(0.5)
        current = obs.request("GetCurrentProgramScene")["currentProgramSceneName"]
        check(current == "Other scene", "(the same key switches scenes when not editing)", "scene %r" % current)

        # Studio Mode: the preview shows a different scene from the output,
        # and editing works on the one in the preview.
        obs.request("SetStudioModeEnabled", studioModeEnabled=True)
        time.sleep(1)
        obs.request("SetCurrentPreviewScene", sceneName="Scene")
        time.sleep(2)
        x0, y0, sx, sy = canvas_on_screen(obs.screen())
        to_screen = lambda cx, cy: (x0 + cx * sx, y0 + cy * sy)  # noqa: E731
        tw, th = size("Title")
        # The title is still selected from before: a double-click on
        # selected text edits it too (OBS's first click selects what's
        # below it).
        obs.click(*to_screen(200 + tw * 0.5, 200 + th / 2), repeat=2)
        obs.key("ctrl+a")
        obs.type("Studio")
        check(obs.settings("Title")["text"] == "Studio", "in Studio Mode, text in the preview's scene is edited",
              repr(obs.settings("Title")["text"]))
        save(out, "features-studio.png", obs.screen())
        obs.key("Escape")

        warnings = [line for line in obs.log().splitlines() if "[obs-typeset]" in line and "editing" not in line and "] drew " not in line]
        check(not warnings, "no warnings from obs-typeset", "; ".join(warnings))


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--out", help="keep the frames and screenshots in this folder")
    parser.add_argument("tests", nargs="*", default=["scaling", "editing", "features"])
    args = parser.parse_args()
    if args.out:
        os.makedirs(args.out, exist_ok=True)
    for name in args.tests:
        globals()["test_" + name](args.out)
    if failed:
        print("%d failed" % len(failed))
        sys.exit(1)


if __name__ == "__main__":
    main()
