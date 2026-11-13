#!/usr/bin/env python3
"""Typeset Text against OBS's own text source (FreeType 2), in a real OBS:
the same scenes with each, and with no text at all for a baseline, measuring
OBS's CPU time (by thread), its memory, and OBS's own frame timings.

    tests/typeset/bench.py [--runs N] [--json OUT.json] [scenario ...]

Needs what test_typeset.py needs. Numbers are only comparable within a run
on one machine; with software rendering (no GPU), drawing costs CPU that a
GPU would take.
"""

import argparse
import json
import os
import statistics
import subprocess
import sys
import threading
import time

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import capture  # noqa: E402
from obs_session import ObsSession  # noqa: E402

FONT = {"face": "DejaVu Sans", "style": "Bold", "size": 64, "flags": 1}
LINE = "Doors 19:30 · Support 20:15"
PARAGRAPH = ("The Grove presents an evening of live music tonight, with doors at half past seven, the support "
             "act at quarter past eight and the headliner at half past nine. ") * 4
TICK = 0.1  # seconds between updates, where a scenario updates

# What each scenario sets up and does: texts (how many, their settings, scale),
# then what happens in the measured window.
SCENARIOS = {
    "idle": dict(n=20, text=LINE, scale=1.0, action=None,
                 about="20 short texts, still"),
    "updating": dict(n=20, text=LINE, scale=1.0, action="update",
                     about="20 short texts, each changed 10 times a second (200 changes/s)"),
    "scaled": dict(n=20, text=LINE, scale=2.5, action=None,
                   about="20 short texts scaled 2.5x in the scene, still"),
    "zooming": dict(n=5, text=LINE, scale=1.0, action="zoom",
                    about="5 short texts, their scale animated every frame"),
    "paragraph": dict(n=2, text=PARAGRAPH, scale=1.0, action="update", wrap=900,
                      about="2 wrapped paragraphs (about 150 words), each changed 10 times a second"),
}


def settings(kind, text, wrap=None):
    if kind.startswith("typeset"):
        s = {"text": text, "font": FONT, "colour": 0xFFFFFFFF}
        if kind == "typeset-plain":  # as OBS's own: no shadow (Typeset Text's default has a soft one)
            s["shadow"] = False
        if wrap:
            s.update(box=True, box_width=wrap)
        return s
    s = {"text": text, "font": FONT, "color1": 0xFFFFFFFF, "color2": 0xFFFFFFFF}
    if wrap:
        s.update(word_wrap=True, custom_width=wrap)
    return s


def collection(kind, sc):
    items = [{"name": "Back", "id": 1, "visible": True, "pos": {"x": 0, "y": 0}, "scale": {"x": 1, "y": 1},
              "rot": 0.0, "align": 5, "bounds_type": 0}]
    sources = [{"id": "color_source", "versioned_id": "color_source_v3", "name": "Back",
                "settings": {"color": 0xFF202020, "width": 1920, "height": 1080}}]
    if kind != "none":
        sid = "typeset_text" if kind.startswith("typeset") else "text_ft2_source"
        vid = "typeset_text" if kind.startswith("typeset") else "text_ft2_source_v2"
        for i in range(sc["n"]):
            name = "T%d" % i
            sources.append({"id": sid, "versioned_id": vid, "name": name,
                            "settings": settings(kind, sc["text"], sc.get("wrap"))})
            x, y = 40 + (i % 2) * 940, 30 + (i // 2) * 100
            items.append({"name": name, "id": i + 2, "visible": True, "pos": {"x": x, "y": y},
                          "scale": {"x": sc["scale"], "y": sc["scale"]}, "rot": 0.0, "align": 5,
                          "bounds_type": 0})
    return {"name": "Bench", "current_scene": "Scene", "current_program_scene": "Scene",
            "scene_order": [{"name": "Scene"}],
            "sources": [{"id": "scene", "versioned_id": "scene", "name": "Scene",
                         "settings": {"id_counter": len(items), "items": items}}] + sources}


def threads(pid):
    """CPU seconds per thread name, and in all."""
    tick = os.sysconf("SC_CLK_TCK")
    by_name = {}
    for tid in os.listdir("/proc/%d/task" % pid):
        try:
            with open("/proc/%d/task/%s/stat" % (pid, tid)) as f:
                stat = f.read()
        except OSError:
            continue
        name = stat[stat.index("(") + 1:stat.rindex(")")]
        fields = stat[stat.rindex(")") + 2:].split()
        by_name[name] = by_name.get(name, 0) + (int(fields[11]) + int(fields[12])) / tick
    return by_name


def rss_mb(pid):
    with open("/proc/%d/status" % pid) as f:
        for line in f:
            if line.startswith("VmRSS:"):
                return int(line.split()[1]) / 1024


def batch(obs, requests):
    """Several requests in one message (op 8), waiting for the replies."""
    rid = "b%d" % time.monotonic_ns()
    obs.ws.send(json.dumps({"op": 8, "d": {"requestId": rid, "haltOnFailure": False, "executionType": 0,
                                           "requests": requests}}))
    while True:
        msg = json.loads(obs.ws.recv())
        if msg["op"] == 9 and msg["d"]["requestId"] == rid:
            return msg["d"]["results"]


def run(kind, name, seconds=10.0):
    sc = SCENARIOS[name]
    with ObsSession(collection(kind, sc)) as obs:
        pid = obs.obs.pid
        time.sleep(4)  # settled: first drawings done
        rss0 = rss_mb(pid)
        ids = {}
        if kind != "none":
            items = obs.request("GetSceneItemList", sceneName="Scene")["sceneItems"]
            ids = {i["sourceName"]: i["sceneItemId"] for i in items if i["sourceName"] != "Back"}
        stats0 = obs.request("GetStats")
        cpu0 = threads(pid)
        t0 = time.monotonic()
        k = 0
        while time.monotonic() - t0 < seconds:
            k += 1
            if sc["action"] == "update" and ids:
                reqs = [{"requestType": "SetInputSettings",
                         "requestData": {"inputName": n, "inputSettings": {
                             "text": sc["text"] + " %d.%d" % (k // 10, k % 10)}}} for n in ids]
                batch(obs, reqs)
                time.sleep(max(0.0, t0 + k * TICK - time.monotonic()))
            elif sc["action"] == "zoom" and ids:
                f = 1.0 + 0.5 * abs(((k % 60) / 30.0) - 1.0)  # 1 to 1.5 and back, every 2 s
                reqs = [{"requestType": "SetSceneItemTransform",
                         "requestData": {"sceneName": "Scene", "sceneItemId": i,
                                         "sceneItemTransform": {"scaleX": f, "scaleY": f}}} for i in ids.values()]
                batch(obs, reqs)
                time.sleep(max(0.0, t0 + k / 30.0 - time.monotonic()))
            else:
                time.sleep(0.2)
        wall = time.monotonic() - t0
        cpu1 = threads(pid)
        stats1 = obs.request("GetStats")
        per_thread = {n: (cpu1.get(n, 0) - cpu0.get(n, 0)) / wall * 100 for n in cpu1}
        return {
            "cpu": sum(per_thread.values()),
            "threads": {n: v for n, v in per_thread.items() if v >= 0.5},
            "render_ms": stats1["averageFrameRenderTime"],
            "skipped": stats1["renderSkippedFrames"] - stats0["renderSkippedFrames"],
            "rss": rss_mb(pid),
            "rss_settled": rss0,
        }


# --- Editing ------------------------------------------------------------------------------
#
# Typeset Text is edited in the preview; OBS's own text in its properties
# dialog (its only way), each keystroke going to the source as it's typed.
# Measured: OBS's CPU while editing but not typing (a caret blinking, the
# dialog open) and while typing, and how long each key takes to show in the
# preview, from a capture of the text's part of the screen 120 times a
# second.

EDIT_TEXT = "Doors 19:30"
TYPED = "and the support at 20:15 sharp"


class Watcher:
    """Frames of a part of the screen, with when each arrived."""

    def __init__(self, obs, x, y, w, h):
        self.w, self.h = w, h
        self.frames = []  # (time, frame)
        self.proc = subprocess.Popen(
            ["ffmpeg", "-loglevel", "error", "-f", "x11grab", "-draw_mouse", "0", "-framerate", "120",
             "-video_size", "%dx%d" % (w, h), "-i", "%s+%d,%d" % (obs.display, x, y),
             "-f", "rawvideo", "-pix_fmt", "gray", "pipe:1"],
            env=obs.env, stdout=subprocess.PIPE, stdin=subprocess.DEVNULL)
        self.thread = threading.Thread(target=self._read, daemon=True)
        self.thread.start()

    def _read(self):
        size = self.w * self.h
        while True:
            buf = self.proc.stdout.read(size)
            if len(buf) < size:
                return
            self.frames.append((time.monotonic(), np.frombuffer(buf, np.uint8)))

    def stop(self):
        self.proc.terminate()
        self.thread.join(5)

    def latency(self, t):
        """From t to the first frame that differs from the last one before t."""
        before = None
        for ft, f in self.frames:
            if ft <= t:
                before = f
            elif before is not None and np.count_nonzero(np.abs(f.astype(int) - before) > 40) > 20:
                return ft - t
        return None


def edit_run(kind, seconds=8.0):
    sc = dict(n=1, text=EDIT_TEXT, scale=1.0)
    col = collection(kind, sc)
    col["sources"][0]["settings"]["items"][1]["pos"] = {"x": 100, "y": 100}
    col["sources"][0]["settings"]["items"].append({"name": "Locator", "id": 9, "visible": True,
                                                   "pos": {"x": 0, "y": 0}, "scale": {"x": 1, "y": 1},
                                                   "rot": 0.0, "align": 5, "bounds_type": 0})
    col["sources"].append({"id": "color_source", "versioned_id": "color_source_v3", "name": "Locator",
                           "settings": {"color": 0xFF000000 | capture.LOCATOR[2] << 16 |
                                        capture.LOCATOR[1] << 8 | capture.LOCATOR[0],
                                        "width": 1920, "height": 1080}})
    with ObsSession(col) as obs:
        pid = obs.obs.pid
        time.sleep(3)
        x0, y0, s = capture.canvas_on_screen(obs.screen())
        obs.request("RemoveSceneItem", sceneName="Scene", sceneItemId=9)
        time.sleep(1)
        t = obs.request("GetSceneItemTransform", sceneName="Scene", sceneItemId=2)["sceneItemTransform"]
        left, top = x0 + 100 * s, y0 + 100 * s
        right, bottom = left + t["width"] * s, top + t["height"] * s
        if kind == "typeset":
            obs.click(right - 10, (top + bottom) / 2)
            obs.click(right - 10, (top + bottom) / 2, repeat=2)
            obs.key("End")
        else:
            obs.request("OpenInputPropertiesDialog", inputName="T0")
            time.sleep(2)
            wid = obs.xdo("search", "--onlyvisible", "--name", "Properties", check=False).split()[-1]
            obs.xdo("windowmove", wid, "700", "420")
            obs.xdo("windowsize", wid, "880", "560")
            time.sleep(1)
            obs.xdo("windowactivate", wid, check=False)
            obs.xdo("windowfocus", wid, check=False)
            time.sleep(0.5)
            # The text box: below the dialog's own preview, after the font.
            dialog = obs.screen()
            from PIL import Image
            Image.fromarray(dialog).save(os.path.join(os.environ.get("BENCH_OUT", "/tmp"), "ft2-dialog.png"))
            obs.click(*FT2_TEXT_BOX)
            obs.key("ctrl+End")
        time.sleep(1.5)
        cpu0 = threads(pid)
        time.sleep(seconds)
        cpu1 = threads(pid)
        idle = {n: (cpu1.get(n, 0) - cpu0.get(n, 0)) / seconds * 100 for n in cpu1}

        watch = Watcher(obs, int(left), int(top) - 4, int(1375 - left), int(bottom - top) + 8)
        time.sleep(0.5)
        keys = []
        cpu0 = threads(pid)
        t0 = time.monotonic()
        for ch in TYPED:
            a = time.monotonic()
            obs.xdo("type", "--delay", "0", ch)
            keys.append((a + time.monotonic()) / 2)
            time.sleep(max(0.0, a + 0.2 - time.monotonic()))
        time.sleep(0.5)
        wall = time.monotonic() - t0
        cpu1 = threads(pid)
        typing = {n: (cpu1.get(n, 0) - cpu0.get(n, 0)) / wall * 100 for n in cpu1}
        watch.stop()
        lat = [watch.latency(k) for k in keys[2:] if TYPED[keys.index(k)] != " "]
        lat = [x * 1000 for x in lat if x is not None]
        text = obs.settings("T0")["text"]
        return {
            "idle_cpu": sum(idle.values()), "idle_threads": {n: v for n, v in idle.items() if v >= 0.5},
            "typing_cpu": sum(typing.values()), "typing_threads": {n: v for n, v in typing.items() if v >= 0.5},
            "latency_ms": statistics.median(lat) if lat else None,
            "latency_p90_ms": sorted(lat)[int(len(lat) * 0.9)] if lat else None,
            "keys_seen": len(lat), "text_ok": text.endswith(TYPED), "text": text,
        }


FT2_TEXT_BOX = (1190, 840)  # its text box, with the dialog where edit_run puts it


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--runs", type=int, default=2)
    parser.add_argument("--json")
    parser.add_argument("--editing", action="store_true", help="the editing measurements instead")
    parser.add_argument("--kinds", default="none,ft2,typeset-plain,typeset",
                        help="which texts: none (a baseline), ft2 (OBS's own), typeset-plain (no shadow), typeset")
    parser.add_argument("scenarios", nargs="*", default=list(SCENARIOS))
    args = parser.parse_args()
    results = {}
    if args.editing:
        for kind in ("ft2", "typeset"):
            runs = [edit_run(kind) for _ in range(args.runs)]
            for r in runs:
                print("editing %-8s idle %5.1f%%  typing %5.1f%%  key to screen: median %s ms, 90%% %s ms "
                      "(%d keys)  text ok: %s" % (kind, r["idle_cpu"], r["typing_cpu"], r["latency_ms"],
                                                   r["latency_p90_ms"], r["keys_seen"], r["text_ok"]), flush=True)
                print("    idle threads: " + ", ".join("%s %.1f%%" % kv for kv in sorted(
                    r["idle_threads"].items(), key=lambda x: -x[1])))
                print("    typing threads: " + ", ".join("%s %.1f%%" % kv for kv in sorted(
                    r["typing_threads"].items(), key=lambda x: -x[1])), flush=True)
            results[kind] = runs
        if args.json:
            with open(args.json, "w") as f:
                json.dump(results, f, indent=1)
        return
    for name in args.scenarios:
        for kind in args.kinds.split(","):
            runs = [run(kind, name) for _ in range(args.runs)]
            med = lambda key: statistics.median(r[key] for r in runs)  # noqa: E731
            results.setdefault(name, {})[kind] = {
                "cpu": med("cpu"), "render_ms": med("render_ms"), "rss": med("rss"),
                "skipped": med("skipped"), "threads": runs[-1]["threads"]}
            r = results[name][kind]
            print("%-10s %-13s cpu %6.1f%%  render %5.2f ms  rss %6.0f MB  skipped %d" % (
                name, kind, r["cpu"], r["render_ms"], r["rss"], r["skipped"]), flush=True)
            print("           threads: " + ", ".join("%s %.1f%%" % (n, v) for n, v in
                                                    sorted(r["threads"].items(), key=lambda x: -x[1])), flush=True)
    if args.json:
        with open(args.json, "w") as f:
            json.dump(results, f, indent=1)


if __name__ == "__main__":
    main()
