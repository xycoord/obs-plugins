#!/usr/bin/env python3
"""Record a captioned video of Typeset Text in a real OBS, driven by the mouse
and keyboard the way a person would use it: moving and resizing text, editing
it in the preview, the format bar, a text box, and undo. For catching anything
that feels wrong before trying it by hand.

    tests/typeset/demo.py OUT.mp4

Needs what test_typeset.py needs, and ffmpeg (with x11grab and drawtext).
"""

import os
import subprocess
import sys
import tempfile
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture  # noqa: E402
from obs_session import SCREEN, ObsSession  # noqa: E402
from test_typeset import bar_button, popup_field  # noqa: E402

FONT = "/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf"

TEXT_BOX = {"text": "The Grove presents an evening of live music tonight",
            "font": {"face": "Inter", "style": "SemiBold", "size": 64, "flags": 0},
            "box": True, "box_width": 1100, "balance": True}


class Demo:
    def __init__(self, obs, out):
        self.obs = obs
        self.out = out
        self.captions = []  # (start, end, text)
        self.started = None

    # --- Recording -------------------------------------------------------------------------

    def record(self):
        self.raw = tempfile.mktemp(suffix=".mkv")
        self.ffmpeg = subprocess.Popen(
            ["ffmpeg", "-loglevel", "error", "-y", "-f", "x11grab", "-draw_mouse", "1", "-framerate", "30",
             "-video_size", "%dx%d" % SCREEN, "-i", self.obs.display, "-c:v", "libx264", "-preset", "ultrafast",
             "-qp", "18", self.raw], stdin=subprocess.PIPE)
        time.sleep(1)
        self.started = time.time()

    def stop(self):
        self.ffmpeg.communicate(b"q", timeout=60)
        # The captions drawn on, one at a time, in a band at the bottom.
        filters = []
        files = []
        for start, end, text in self.captions:
            f = tempfile.NamedTemporaryFile("w", suffix=".txt", delete=False)
            f.write(text)
            f.close()
            files.append(f.name)
            filters.append(
                "drawtext=fontfile=%s:textfile=%s:fontsize=30:fontcolor=white:box=1:boxcolor=black@0.75:"
                "boxborderw=14:x=(w-text_w)/2:y=h-text_h-40:enable='between(t,%.2f,%.2f)'"
                % (FONT, f.name, start + 1, end + 1))
        subprocess.run(["ffmpeg", "-loglevel", "error", "-y", "-i", self.raw, "-vf", ",".join(filters) or "null",
                        "-c:v", "libx264", "-preset", "slow", "-crf", "24", "-pix_fmt", "yuv420p",
                        "-movflags", "+faststart", self.out], check=True)
        for f in files + [self.raw]:
            os.remove(f)

    def caption(self, text):
        now = time.time() - self.started
        if self.captions:
            s, _, t = self.captions[-1]
            self.captions[-1] = (s, now, t)
        self.captions.append((now, now + 3600, text))

    # --- Hands -----------------------------------------------------------------------------

    def move(self, x, y, steps=20, delay=0.02):
        """The pointer to (x, y), smoothly, as a hand moves it."""
        loc = self.obs.xdo("getmouselocation").split()
        x0, y0 = int(loc[0].split(":")[1]), int(loc[1].split(":")[1])
        for i in range(1, steps + 1):
            t = i / steps
            t = t * t * (3 - 2 * t)
            self.obs.xdo("mousemove", str(int(x0 + (x - x0) * t)), str(int(y0 + (y - y0) * t)))
            time.sleep(delay)

    def drag(self, x0, y0, x1, y1, steps=45, delay=0.03):
        self.move(x0, y0)
        time.sleep(0.2)
        self.obs.xdo("mousedown", "1")
        self.move(x1, y1, steps, delay)
        time.sleep(0.15)
        self.obs.xdo("mouseup", "1")
        time.sleep(0.4)

    def click(self, x, y, repeat=1):
        self.move(x, y)
        time.sleep(0.15)
        self.obs.xdo("click", "--repeat", str(repeat), "--delay", "80", "1")
        time.sleep(0.4)

    def type(self, text, delay=110):
        self.obs.xdo("type", "--delay", str(delay), text)
        time.sleep(0.3)

    def key(self, *keys, pause=0.45):
        for k in keys:
            self.obs.xdo("key", k)
            time.sleep(pause)

    def pause(self, s=1.2):
        time.sleep(s)

    # --- The scene -------------------------------------------------------------------------

    def where(self, name):
        """An item's box on the screen: left, top, right, bottom."""
        items = self.obs.request("GetSceneItemList", sceneName="Scene")["sceneItems"]
        i = next(it["sceneItemId"] for it in items if it["sourceName"] == name)
        t = self.obs.request("GetSceneItemTransform", sceneName="Scene", sceneItemId=i)["sceneItemTransform"]
        x0, y0, sx, sy = self.canvas
        return (x0 + t["positionX"] * sx, y0 + t["positionY"] * sy,
                x0 + (t["positionX"] + t["width"]) * sx, y0 + (t["positionY"] + t["height"]) * sy)


def main():
    out = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else "typeset-demo.mp4")
    tmp = tempfile.mkdtemp()
    backdrop = os.path.join(tmp, "backdrop.png")
    capture.busy_background(backdrop)
    scene = capture.collection(backdrop)
    scene["sources"].append(capture.source("Text box", TEXT_BOX))
    scene["sources"][0]["settings"]["items"].append(dict(capture.item("Text box", 8, 400, 380), visible=False))
    scene["sources"][0]["settings"]["id_counter"] = 8
    with ObsSession(scene) as obs:
        # Where the canvas is on the screen, from a solid layer shown for a
        # moment over everything (the backdrop is too busy to find it by).
        obs.request("SetSceneItemIndex", sceneName="Scene", sceneItemId=7, sceneItemIndex=20)
        obs.request("SetSceneItemEnabled", sceneName="Scene", sceneItemId=7, sceneItemEnabled=True)
        time.sleep(1.5)
        x0, y0, scale = capture.canvas_on_screen(obs.screen())
        obs.request("RemoveSceneItem", sceneName="Scene", sceneItemId=7)
        time.sleep(1)
        d = Demo(obs, out)
        d.canvas = (x0, y0, scale, scale)
        d.record()
        try:

            # 1. Moving.
            d.caption("1. Moving a text: drag it (OBS's own handles)")
            l, t, r, b = d.where("Title")
            d.click((l + r) / 2, (t + b) / 2)
            d.drag((l + r) / 2, (t + b) / 2, (l + r) / 2 - 220, (t + b) / 2 + 120)
            l, t, r, b = d.where("Title")
            d.drag((l + r) / 2, (t + b) / 2, (l + r) / 2 + 220, (t + b) / 2 - 120)
            d.pause()

            # 2. Resizing by a corner.
            d.caption("2. Resizing by a corner: redrawn sharp at every size")
            l, t, r, b = d.where("Title")
            d.drag(r, b, r + 260, b + 150, steps=60)
            d.pause(1.5)
            l, t, r, b = d.where("Title")
            d.drag(r, b, r - 420, b - 240, steps=60)
            d.pause(1.5)
            l, t, r, b = d.where("Title")
            d.drag(r, b, r + 160, b + 90, steps=40)
            d.pause()

            # 3. Side handles: the text box.
            d.caption("3. Side handles change the text box: the text rewraps at the same size")
            l, t, r, b = d.where("Title")
            d.drag(r, (t + b) / 2, r - 260, (t + b) / 2, steps=70, delay=0.035)
            d.pause(1.2)
            l, t, r, b = d.where("Title")
            d.drag(r, (t + b) / 2, r + 330, (t + b) / 2, steps=70, delay=0.035)
            d.pause(1.2)
            d.caption("3. The left handle too: the right edge stays put")
            l, t, r, b = d.where("Title")
            d.drag(l, (t + b) / 2, l + 200, (t + b) / 2, steps=60, delay=0.035)
            d.pause(1.2)
            l, t, r, b = d.where("Title")
            d.drag(l, (t + b) / 2, l - 200, (t + b) / 2, steps=60, delay=0.035)
            d.pause(1.2)
            d.caption("3. ...and corners still scale the whole text")
            l, t, r, b = d.where("Title")
            d.drag(r, t, r - 160, t + 80, steps=50)  # the top right: on the canvas, however tall it's grown
            d.pause(1.2)

            # 4. Editing.
            d.caption("4. Double-click to edit: the word is selected, typing replaces it")
            l, t, r, b = d.where("Message")
            d.click(l + (r - l) * 0.6, (t + b) / 2)
            d.click(l + (r - l) * 0.6, (t + b) / 2, repeat=2)
            d.pause()
            d.type("5")
            d.pause(0.8)
            d.caption("4. Caret: arrows, Ctrl+arrows by word, Home/End; Shift selects")
            d.key("Left", "Left", "ctrl+Left", "ctrl+Left", "Home", "End", pause=0.5)
            d.key("shift+ctrl+Left", "shift+ctrl+Left", pause=0.6)
            d.pause()
            d.caption("4. Selecting by dragging over the text")
            l, t, r, b = d.where("Message")
            d.drag(l + 10, (t + b) / 2, l + (r - l) * 0.45, (t + b) / 2, steps=40)
            d.pause()
            d.key("Escape")
            d.pause()

            # 5. The format bar.
            d.caption("5. The format bar: italic, weight, size")
            l, t, r, b = d.where("Title")
            d.click(l + (r - l) * 0.62, t + (b - t) * 0.25)  # on "Grove"
            d.click(l + (r - l) * 0.62, t + (b - t) * 0.25, repeat=2)
            d.pause()
            d.click(*bar_button(obs, "italic"))
            d.pause()
            d.click(*bar_button(obs, "weight"))
            d.pause(0.8)
            d.key("Up", "Up", pause=0.5)
            d.key("Return")
            d.pause()
            d.click(*bar_button(obs, "size"))
            d.type("200")
            d.key("Return")
            d.pause(1.5)
            d.caption("5. Spacing: drag the letter-spacing slider (live)")
            d.click(*bar_button(obs, "tracking"))
            d.pause(0.6)
            fx, fy = popup_field(obs, "tracking")
            d.drag(fx - 160, fy, fx - 80, fy, steps=50)
            d.pause()
            d.key("Escape")
            d.caption("5. Paragraph: alignment; colour (opens beside the text); clear formatting")
            d.click(*bar_button(obs, "paragraph"))
            d.pause(0.6)
            d.click(*popup_field(obs, "centre"))
            d.pause(0.8)
            d.key("Escape")
            d.click(*bar_button(obs, "colour"))
            d.pause(2)
            d.key("Escape")
            d.pause(0.6)
            d.key("ctrl+a")
            d.click(*bar_button(obs, "clear"))
            d.pause(1.5)
            d.key("Escape")
            d.pause()

            # 6. A text box while editing.
            d.caption("6. While editing: a side handle rewraps the text live (balanced lines)")
            items = obs.request("GetSceneItemList", sceneName="Scene")["sceneItems"]
            box_id = next(it["sceneItemId"] for it in items if it["sourceName"] == "Text box")
            obs.request("SetSceneItemEnabled", sceneName="Scene", sceneItemId=box_id, sceneItemEnabled=True)
            d.pause(0.8)
            l, t, r, b = d.where("Text box")
            d.click(l + 40, (t + b) / 2)
            d.click(l + 40, (t + b) / 2, repeat=2)
            d.pause()
            d.drag(r, (t + b) / 2, r - 380, (t + b) / 2, steps=70, delay=0.035)
            d.pause()
            l, t, r, b = d.where("Text box")
            d.drag(r, (t + b) / 2, r + 420, (t + b) / 2, steps=60, delay=0.035)
            d.pause()
            d.key("Escape")
            d.pause(0.6)

            # 7. Undo.
            d.caption("7. OBS's own Undo and Redo take back a whole edit")
            d.key("ctrl+z", pause=1.5)
            d.key("ctrl+z", pause=1.5)
            d.key("ctrl+shift+z", pause=1.5)
            d.pause()

            # 8. Resizing after editing.
            d.caption("8. Resizing the edited text: still sharp")
            obs.request("SetSceneItemEnabled", sceneName="Scene", sceneItemId=box_id, sceneItemEnabled=False)
            l, t, r, b = d.where("Title")
            d.click((l + r) / 2, (t + b) / 2)
            d.drag(r, b, r - 250, b - 140, steps=50)
            d.pause(1.5)
            d.caption("")
            d.pause(0.5)
        finally:
            d.stop()
    os.remove(backdrop)
    print(out)


if __name__ == "__main__":
    main()
