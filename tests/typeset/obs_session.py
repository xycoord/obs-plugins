"""Run the installed OBS on a virtual display with a throwaway configuration and
drive it: obs-websocket to set scenes up and take frames of its output, and
xdotool to use its window the way a person would.

Nothing of the user's own OBS setup is read or changed: the configuration is
made in a temporary folder and the plugins folder is linked from the real one.
"""

import base64
import io
import itertools
import json
import os
import shutil
import subprocess
import tempfile
import time

import numpy as np
import websocket
from PIL import Image

PLUGINS = os.environ.get(
    "OBS_PLUGINS_DIR",
    os.path.join(os.environ.get("XDG_CONFIG_HOME", os.path.expanduser("~/.config")), "obs-studio", "plugins"),
)
PORT = 4466
SCREEN = (1600, 1000)
CANVAS = (1920, 1080)


class ObsError(Exception):
    pass


class ObsSession:
    def __init__(self, scene, fps=30):
        """scene: the scene collection (a dict as OBS saves it); fps: the canvas's frame rate."""
        self.fps = fps
        self.dir = tempfile.mkdtemp(prefix="typeset-test-")
        self.display = None
        self.xvfb = self.obs = self.ws = None
        self.ids = itertools.count(1)
        self._write_config(scene)

    def _write_config(self, scene):
        cfg = os.path.join(self.dir, ".config", "obs-studio")
        os.makedirs(os.path.join(cfg, "basic", "scenes"))
        os.makedirs(os.path.join(cfg, "basic", "profiles", "Test"))
        os.symlink(PLUGINS, os.path.join(cfg, "plugins"))
        for ini in ("global.ini", "user.ini"):  # OBS 30 reads global.ini, 31+ user.ini
            with open(os.path.join(cfg, ini), "w") as f:
                f.write(
                    "[General]\nFirstRun=true\nEnableAutoUpdates=false\n"
                    "[Basic]\nSceneCollection=Test\nSceneCollectionFile=Test\n"
                    "Profile=Test\nProfileDir=Test\nConfigOnNewProfile=false\n"
                    "[BasicWindow]\nPreviewProgramMode=false\nSnappingEnabled=false\n"
                    # obs-websocket 5.3 (OBS 30) reads its settings here; 5.4 from its config.json
                    "[OBSWebSocket]\nFirstLoad=false\nServerEnabled=true\nServerPort=%d\n"
                    "AuthRequired=false\nAlertsEnabled=false\n" % PORT
                )
        with open(os.path.join(cfg, "basic", "profiles", "Test", "basic.ini"), "w") as f:
            f.write(
                "[General]\nName=Test\n[Video]\nBaseCX=%d\nBaseCY=%d\nOutputCX=%d\nOutputCY=%d\nFPSType=1\nFPSInt=%d\n"
                % (CANVAS + CANVAS + (self.fps,))
            )
        with open(os.path.join(cfg, "basic", "scenes", "Test.json"), "w") as f:
            json.dump(scene, f)
        ws_cfg = os.path.join(cfg, "plugin_config", "obs-websocket")
        os.makedirs(ws_cfg)
        with open(os.path.join(ws_cfg, "config.json"), "w") as f:
            json.dump(
                {"first_load": False, "server_enabled": True, "server_port": PORT,
                 "auth_required": False, "alerts_enabled": False},
                f,
            )

    @property
    def env(self):
        env = dict(os.environ, DISPLAY=self.display, HOME=self.dir,
                   XDG_CONFIG_HOME=os.path.join(self.dir, ".config"))
        env.pop("WAYLAND_DISPLAY", None)
        env["QT_QPA_PLATFORM"] = "xcb"
        env["TYPESET_DEBUG"] = "1"  # Typeset Text logs each drawing, for the performance checks
        return env

    def start(self):
        # A display of its own: Xvfb picks a free number and says which.
        read, write = os.pipe()
        self.xvfb = subprocess.Popen(
            ["Xvfb", "-displayfd", str(write), "-screen", "0", "%dx%dx24" % SCREEN, "-nolisten", "tcp"],
            stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, pass_fds=(write,))
        os.close(write)
        with os.fdopen(read) as f:
            self.display = ":" + f.readline().strip()
        self.obs_out = open(os.path.join(self.dir, "obs.out"), "w")
        self.obs = subprocess.Popen(
            ["obs", "--multi", "--disable-shutdown-check", "--disable-updater"],
            cwd=self.dir, env=self.env, stdout=self.obs_out, stderr=subprocess.STDOUT)
        deadline = time.time() + 60
        while True:
            try:
                self.ws = websocket.create_connection("ws://127.0.0.1:%d" % PORT, timeout=10)
                break
            except OSError:
                if time.time() > deadline or self.obs.poll() is not None:
                    raise ObsError("OBS didn't start; its log:\n" + self.log_tail())
                time.sleep(0.5)
        hello = json.loads(self.ws.recv())
        assert hello["op"] == 0
        self.ws.send(json.dumps({"op": 1, "d": {"rpcVersion": 1}}))
        assert json.loads(self.ws.recv())["op"] == 2
        self._wait_for_window()

    def _wait_for_window(self):
        deadline = time.time() + 30
        while time.time() < deadline:
            out = self.xdo("search", "--onlyvisible", "--name", "OBS [0-9]", check=False)
            if out.strip():
                self.window = out.split()[0]
                # Fill the screen, so the preview is large.
                self.xdo("windowmove", self.window, "0", "0")
                self.xdo("windowsize", self.window, str(SCREEN[0]), str(SCREEN[1]))
                time.sleep(3)
                return
            time.sleep(0.5)
        raise ObsError("OBS's window didn't appear")

    def stop(self):
        if self.ws:
            self.ws.close()
        for p in (self.obs, self.xvfb):
            if p and p.poll() is None:
                p.terminate()
                try:
                    p.wait(10)
                except subprocess.TimeoutExpired:
                    p.kill()
        if not os.environ.get("TYPESET_KEEP"):
            shutil.rmtree(self.dir, ignore_errors=True)

    def __enter__(self):
        try:
            self.start()
        except BaseException:
            self.stop()
            raise
        return self

    def __exit__(self, *exc):
        self.stop()

    # --- OBS ---------------------------------------------------------------------------

    def request(self, kind, **data):
        rid = str(next(self.ids))
        self.ws.send(json.dumps({"op": 6, "d": {"requestType": kind, "requestId": rid, "requestData": data}}))
        while True:
            msg = json.loads(self.ws.recv())
            if msg["op"] == 7 and msg["d"]["requestId"] == rid:
                status = msg["d"]["requestStatus"]
                if not status["result"]:
                    raise ObsError("%s failed: %s" % (kind, status.get("comment", status["code"])))
                return msg["d"].get("responseData", {})

    def frame(self, source="Scene"):
        """The source as OBS renders it (a scene: the canvas), as an RGBA array."""
        data = self.request("GetSourceScreenshot", sourceName=source, imageFormat="png")["imageData"]
        png = base64.b64decode(data.split(",", 1)[1])
        return np.asarray(Image.open(io.BytesIO(png)).convert("RGBA"))

    def settings(self, source):
        return self.request("GetInputSettings", inputName=source)["inputSettings"]

    def log(self):
        logs = os.path.join(self.dir, ".config", "obs-studio", "logs")
        files = sorted(os.listdir(logs)) if os.path.isdir(logs) else []
        return open(os.path.join(logs, files[-1]), errors="replace").read() if files else ""

    def log_tail(self, n=40):
        out = open(os.path.join(self.dir, "obs.out"), errors="replace").read() if self.obs_out else ""
        return "\n".join((self.log() or out).splitlines()[-n:])

    # --- The window --------------------------------------------------------------------

    def xdo(self, *args, check=True):
        r = subprocess.run(["xdotool", *args], env=self.env, capture_output=True, text=True)
        if check and r.returncode:
            raise ObsError("xdotool %s: %s" % (" ".join(args), r.stderr))
        return r.stdout

    def screen(self):
        """The whole virtual screen, as an RGB array."""
        out = subprocess.run(["import", "-window", "root", "png:-"], env=self.env, capture_output=True, check=True)
        return np.asarray(Image.open(io.BytesIO(out.stdout)).convert("RGB"))

    def click(self, x, y, button=1, repeat=1):
        self.xdo("mousemove", str(int(x)), str(int(y)))
        time.sleep(0.15)
        self.xdo("click", "--repeat", str(repeat), "--delay", "80", str(button))
        time.sleep(0.3)

    def drag(self, x0, y0, x1, y1):
        self.xdo("mousemove", str(int(x0)), str(int(y0)))
        time.sleep(0.1)
        self.xdo("mousedown", "1")
        for t in np.linspace(0, 1, 8)[1:]:
            self.xdo("mousemove", str(int(x0 + (x1 - x0) * t)), str(int(y0 + (y1 - y0) * t)))
            time.sleep(0.04)
        self.xdo("mouseup", "1")
        time.sleep(0.3)

    def type(self, text):
        self.xdo("type", "--delay", "40", text)
        time.sleep(0.3)

    def key(self, *keys):
        self.xdo("key", "--delay", "60", *keys)
        time.sleep(0.3)
