#!/usr/bin/env python3
"""End-to-end tests for the Knob Timer, run against the real device over USB.

Implements every case in docs/TESTS.md. The firmware's debug interface
(serial_commands() in main/main.c) is driven over the USB serial port:

  single letters   s (tap: start/pause)  r (hold: reset)  + - (knob)  e (tap the task: past tasks)
                   o (tap the pill: projects)  m (mic)  t (tap in a list / stop listening)
                   u p (swipe up: mode / down: refresh)  < > (swipe left/right: profile)
                   d (print STATE json)  c (capture the screen)
  "#" lines        #tap X Y   #task TEXT   #project ID [#RRGGBB NAME]   #set N   #app track|pomo   #theme mono|inverted
                   #task_id N   #session N   #remaining N   #toggl start TEXT|stop   #wifi on|off   #telemetry on|off   #list   #reboot

Usage (from firmware/, with the ESP-IDF environment loaded: . ../idf-env.sh):
    python tools/knob_test.py                 # everything (speaks through the Mac, creates Toggl entries)
    python tools/knob_test.py --quick         # no slow cases, no side effects (a few minutes)
    python tools/knob_test.py --only R4,S5    # chosen cases (B1, B2 and B4 always run)
    python tools/knob_test.py --telemetry     # also record TELEMETRY lines to test-output/telemetry.jsonl

Timer cases that don't test Toggl run with Wi-Fi switched off (#wifi off) and keep every
running stretch under 10 s, so they never reach Toggl (Toggl's free plan allows few requests).
Your settings (mode, profile, length, task and its Toggl task, project, telemetry) are restored at the end.
Captures (PNG), the serial log and report.json land in test-output/ (git-ignored).
Opening the serial port restarts the knob, so a run starts from a fresh boot.
"""
import argparse
import base64
import json
import os
import re
import struct
import subprocess
import sys
import threading
import time
import zlib

try:
    import serial
except ImportError:
    sys.exit("pyserial is missing: load the ESP-IDF environment first (. ../idf-env.sh)")

CRASH = re.compile(r"Guru Meditation|abort\(\) was called|panic'ed|Backtrace:|assert failed|Stack canary|task_wdt:")
ANSI = re.compile(r"\x1b\[[0-9;]*m")
SLOW = {"P5", "S1", "S3", "S4", "S6", "S7"}                                   # skipped by --quick
SIDE_EFFECTS = {"V1", "V2", "V3", "S1", "S2", "S3", "S4", "S5", "S6", "S7"}  # speak aloud / Toggl entries


class Knob:
    """Serial connection: a background reader collects log lines; helpers wait for them."""

    def __init__(self, port):
        self.s = serial.Serial()
        self.s.port, self.s.baudrate, self.s.timeout = port, 115200, 0.05
        self.s.dtr = self.s.rts = False
        self.s.open()
        self.lines = []
        self.lock = threading.Lock()
        self.alive = True
        threading.Thread(target=self._read, daemon=True).start()

    def _read(self):
        buf = b""
        while self.alive:
            try:
                data = self.s.read(65536)
            except serial.SerialException:
                time.sleep(0.2)
                continue
            if not data:
                continue
            buf += data
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = ANSI.sub("", raw.decode("utf-8", "replace").rstrip("\r"))
                with self.lock:
                    self.lines.append(text)

    def close(self):
        self.alive = False
        time.sleep(0.1)
        self.s.close()

    def mark(self):
        with self.lock:
            return len(self.lines)

    def since(self, i=0):
        with self.lock:
            return self.lines[i:]

    def send(self, keys, gap=0.18):
        for c in keys:
            self.s.write(c.encode())
            self.s.flush()
            time.sleep(gap)

    def wait(self, pattern, timeout, since=None):
        rx = re.compile(pattern)
        i = self.mark() if since is None else since
        end = time.time() + timeout
        while time.time() < end:
            for line in self.since(i):
                m = rx.search(line)
                if m:
                    return m
            time.sleep(0.05)
        return None

    def cmd(self, line, timeout=3):
        """Send a "#..." line command; True if the knob answered "CMD ok"."""
        i = self.mark()
        self.s.write(("#" + line + "\n").encode())
        self.s.flush()
        name = line.split(" ", 1)[0]
        m = self.wait(rf"^CMD (ok|error) {re.escape(name)}\b", timeout, since=i)
        time.sleep(0.1)
        return bool(m and m.group(1) == "ok")

    def state(self, timeout=3):
        for _ in range(3):
            i = self.mark()
            self.send("d", gap=0)
            m = self.wait(r"^STATE (\{.*\})$", timeout, since=i)
            if m:
                try:
                    return json.loads(m.group(1))
                except json.JSONDecodeError:
                    continue  # a garbled line: ask again
        raise RuntimeError("no STATE reply")

    def wait_state(self, check, timeout, every=0.4):
        end = time.time() + timeout
        st = None
        while time.time() < end:
            st = self.state()
            if check(st):
                return st
            time.sleep(every)
        return st

    def lists(self):
        for _ in range(3):
            i = self.mark()
            self.cmd("list")
            p = self.wait(r"^PROJECTS (\[.*\])$", 3, since=i)
            e = self.wait(r"^ENTRIES (\[.*\])$", 3, since=i)
            try:
                return (json.loads(p.group(1)) if p else []), (json.loads(e.group(1)) if e else [])
            except json.JSONDecodeError:
                continue  # a garbled line: ask again
        return [], []

    def capture(self, path, timeout=15):
        """Save the screen as a PNG; returns rows of (r, g, b) or None."""
        i = self.mark()
        self.send("c", gap=0)
        head = self.wait(r"^CAPTURE (\d+) (\d+)$", timeout, since=i)
        if not head or not self.wait(r"^CAPTURE END$", timeout, since=i):
            return None
        w, h = int(head.group(1)), int(head.group(2))
        lines = self.since(i)
        start = next(n for n, l in enumerate(lines) if re.fullmatch(r"CAPTURE \d+ \d+", l))
        data = b""
        for l in lines[start + 1:]:
            if l == "CAPTURE END":
                break
            if re.fullmatch(r"[A-Za-z0-9+/=]+", l):
                data += base64.b64decode(l)
        if len(data) != w * h * 2:
            return None
        rows = [[None] * w for _ in range(h)]
        for y in range(h):
            for x in range(w):
                v = data[(y * w + x) * 2] << 8 | data[(y * w + x) * 2 + 1]  # byte-swapped RGB565
                rows[y][x] = ((v >> 11) * 255 // 31, ((v >> 5) & 63) * 255 // 63, (v & 31) * 255 // 31)
        write_png(path, w, h, rows)
        return rows


def write_png(path, w, h, rows):
    raw = b"".join(b"\x00" + bytes(c for px in row for c in px) for row in rows)

    def chunk(kind, body):
        return struct.pack(">I", len(body)) + kind + body + struct.pack(">I", zlib.crc32(kind + body) & 0xFFFFFFFF)

    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) +
                chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def lit(rows, x0, y0, x1, y1, level=60):
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if max(rows[y][x]) > level)


def lit_at(rows, cx, cy, r, level=60):
    return lit(rows, cx - r, cy - r, cx + r, cy + r, level)


def diff(a, b, x0, y0, x1, y1):
    return sum(1 for y in range(y0, y1) for x in range(x0, x1) if a[y][x] != b[y][x])


def near(c1, c2, tol=40):
    return all(abs(p - q) <= tol for p, q in zip(c1, c2))


def hexrgb(h):
    return tuple(int(h[i:i + 2], 16) for i in (1, 3, 5))


def words(text):
    return set(re.findall(r"[a-z0-9]+", text.lower()))


class Run:
    """Shared state for the cases: the knob, options and the output folder."""

    def __init__(self, k, a):
        self.k, self.a, self.out = k, a, a.out
        self.original = None
        self.projects, self.entries = [], []
        self.is_offline = False

    def png(self, name):
        return os.path.join(self.out, name + ".png")

    def _stop(self):
        k = self.k
        st = k.state()
        if st["mode"] == "running":
            k.send("s")
            st = k.state()
        if st["mode"] != "idle":
            k.send("r")
        return k.state()

    def idle(self, app=None, length=None, theme="mono"):
        """A stopped, reset timer in the given mode, no list open, nothing listening."""
        k = self.k
        st = k.state()
        if st["voice"] == "listening":
            k.send("t")
            k.wait_state(lambda s: s["voice"] == "idle", 15)
        elif st["voice"] == "waiting":
            k.wait_state(lambda s: s["voice"] == "idle", 15)
        if st["list"] != "closed":
            k.send("u")
        if st["syncing"]:
            k.wait_state(lambda s: not s["syncing"], 20)
        st = self._stop()
        if app and st["app"] != app:
            k.cmd(f"app {app}")
            st = self._stop()
        if length:
            k.cmd(f"set {length}")
        if theme and st["theme"] != theme:
            k.cmd(f"theme {theme}")
        time.sleep(1.6)  # hints fade
        return k.state()

    def offline(self):
        """Wi-Fi off: timer runs stay on the knob (a stretch under 10 s is dropped)."""
        if not self.is_offline:
            self.k.cmd("wifi off")
            self.k.wait_state(lambda s: not s["wifi"], 10)
            self.is_offline = True

    def online(self):
        if self.is_offline:
            self.k.cmd("wifi on")
            self.k.wait_state(lambda s: s["wifi"] and s["toggl"], 40)
            self.is_offline = False

    def pick_in_list(self, open_key, target_idx):
        """Open a list, move to row target_idx and choose it."""
        k = self.k
        k.send(open_key)
        if open_key == "e":
            st = k.wait_state(lambda s: s["list"] == "tasks" and s["entries_state"] != "loading", 15)
        else:
            st = k.wait_state(lambda s: s["list"] == "projects", 5)
        steps = target_idx - st["list_idx"]
        if steps:
            k.send(("+" if steps > 0 else "-") * abs(steps))
        elif open_key == "o":  # a tap without a turn keeps the project: turn away and back
            k.send("-" if st["list_idx"] == 0 else "-+")
        k.send("t")
        return k.state()


def rim_px(rows, deg, r=167):
    import math
    a = math.radians(deg)
    return rows[round(180 - r * math.cos(a))][round(180 + r * math.sin(a))]


# ---- Cases ------------------------------------------------------------------------------
# Each takes the Run and returns (ok, detail), or a dict of them for combined cases.

def B1(r):
    log = "\n".join(r.k.since())
    need = {"display": "panel ready", "touch": "touch on I2C", "haptics": "DRV2605 ready",
            "mic": "PDM microphone ready", "render": "layers ready"}
    missing = [n for n, s in need.items() if s not in log]
    return not missing, "all components came up" if not missing else f"missing: {', '.join(missing)}"


def B2(r):
    st = r.k.wait_state(lambda s: s["wifi"] and s["toggl"], 30)
    return st["wifi"] and st["toggl"], f"Wi-Fi {'up' if st['wifi'] else 'DOWN'}, Toggl {'ready' if st['toggl'] else 'NOT ready'}"


def B3(r):
    kb = r.k.state()["free_internal_kb"]
    return kb >= 40, f"{kb} KB internal RAM free (need 40)"


def B4(r):
    bad = [l for l in r.k.since() if CRASH.search(l)]
    return not bad, bad[0][:160] if bad else "no crashes, asserts or watchdog resets"


def B5(r):
    # Only windows with continuous animation (30+ frames) count: a window with a handful of
    # frames is dominated by one-off redraws (e.g. rebuilding the background after a profile change).
    ts = [float(m.group(2)) for l in r.k.since() for m in [re.search(r"(\d+) frames in 10 s: ([0-9.]+) ms", l)]
          if m and int(m.group(1)) >= 30]
    if not ts:
        return True, "no animated windows logged yet"
    return max(ts) <= 45, f"{len(ts)} animated windows, {min(ts):.1f}-{max(ts):.1f} ms per frame (limit 45, i.e. 22+ fps)"


# -- Display ---------------------------------------------------------------------------------

def D1(r):
    k = r.k
    r.idle("track")
    if not r.projects:
        return False, "no projects"
    proj = r.projects[0]
    k.cmd(f"project {proj['id']}")
    k.cmd("task knob-test layout")
    time.sleep(0.6)
    rows = k.capture(r.png("layout-track"))
    color = hexrgb(proj["color"])
    pill = sum(1 for x in range(100, 260) if near(rows[64][x], color, 30))
    task = lit(rows, 60, 98, 300, 132, level=200)
    digits = lit(rows, 60, 150, 300, 212, level=200)
    rim = near(rim_px(rows, 100), color, 30)
    k.cmd("task")
    time.sleep(0.6)
    rows2 = k.capture(r.png("layout-untitled"))
    bright = lit(rows2, 60, 98, 300, 132, level=200)
    gray = sum(1 for y in range(98, 132) for x in range(60, 300) if 90 < max(rows2[y][x]) <= 200)
    ok = pill > 40 and task > 200 and digits > 1500 and rim and bright == 0 and gray > 100
    return ok, (f"pill {pill} px in {proj['color']}, task {task} px, time {digits} px, rim in the project color: {rim}; "
                f"'Untitled' in gray ({gray} px, {bright} white) (layout-*.png)")


def D2(r):
    k = r.k
    r.idle("track")
    k.cmd("task knob-test this task name is much too long to fit across the round screen")
    time.sleep(2.6)
    a = k.capture(r.png("scroll-1"))
    time.sleep(2.5)
    b = k.capture(r.png("scroll-2"))
    d = diff(a, b, 60, 98, 300, 132) if a and b else 0
    edge = max(max(a[y][56]) for y in range(98, 132)) if a else 255  # faded out at the window edge
    return d > 200 and edge < 60, f"{d} px of the task line changed in 2.5 s, faded edge (scroll-*.png)"


def D3(r):
    k = r.k
    r.idle("track", theme="inverted")
    time.sleep(0.5)
    rows = k.capture(r.png("inverted"))
    k.cmd("theme mono")
    face = rows[260][60] if rows else (0, 0, 0)
    dark = sum(1 for y in range(150, 212) for x in range(60, 300) if max(rows[y][x]) < 60) if rows else 0
    return min(face) > 230 and dark > 1500, f"white face {face}, {dark} dark time pixels (inverted.png)"


def D4(r):
    k = r.k
    st = r.idle("track")
    rows = k.capture(r.png("wifi"))
    # The glyph is a dark notch on the band: look for pixels around (180, 347) that differ from the band.
    band = rows[347][150] if rows else (0, 0, 0)
    n = sum(1 for y in range(338, 356) for x in range(170, 191) if not near(rows[y][x], band, 25)) if rows else 0
    return st["wifi"] and n > 8, f"glyph in the rim: {n} px (wifi.png)"


# -- Lists (need Toggl, read-only) ------------------------------------------------------------

def L1(r):
    k = r.k
    st0 = r.idle("track")
    k.send("o")
    st = k.state()
    rows = k.capture(r.png("projects"))
    want = next((i for i, p in enumerate(r.projects) if p["id"] == st0["project_id"]), len(r.projects))
    k.send("u")
    closed = k.state()
    # A tap without turning the knob keeps the project, even one that isn't in the list
    # (the list opens on "No project" for it, and a reflex tap mustn't clear it).
    k.send("o")
    k.send("t")
    kept = k.state()
    k.cmd("project 999999 #3366CC knob-test unlisted")
    k.send("o")
    unl = k.state()
    k.send("t")
    unl_kept = k.state()
    if st0["project_id"] > 0:
        k.cmd(f"project {st0['project_id']} {st0['project_color']} {st0['project']}")
    else:
        k.cmd("project 0")
    ok = (st["list"] == "projects" and st["projects"] > 0 and st["list_idx"] == want and closed["list"] == "closed" and rows
          and kept["project_id"] == st0["project_id"] and kept["list"] == "closed"
          and unl["list_idx"] == len(r.projects) and unl_kept["project_id"] == 999999)
    return ok, (f"{st['projects']} projects + 'No project' last, opened on row {st['list_idx']} (projects.png); swipe up closed it; "
                f"a tap without turning kept the project (also an unlisted one, opened on 'No project')")


def L2(r):
    k = r.k
    st0 = r.idle("track")
    if not r.projects:
        return False, "no projects"
    proj = r.projects[-1] if r.projects[-1]["id"] != st0["project_id"] else r.projects[0]
    idx = r.projects.index(proj)
    st = r.pick_in_list("o", idx)
    time.sleep(0.6)
    rows = k.capture(r.png("rim"))
    rim = rim_px(rows, 100) if rows else (0, 0, 0)
    picked = st["project_id"] == proj["id"] and near(rim, hexrgb(proj["color"]), 30)
    none = r.pick_in_list("o", len(r.projects))
    time.sleep(0.6)
    rows2 = k.capture(r.png("rim-none"))
    white = rows2 and min(rim_px(rows2, 100)) > 220  # no project: the Tracking rim takes the ink color (white on Mono)
    ok = picked and none["project_id"] == 0 and none["project"] == "" and white
    return ok, f"picked {proj['name']!r} (rim {rim} vs {proj['color']}), then 'No project': white rim (rim*.png)"


def L3(r):
    k = r.k
    r.idle("track")
    k.send("o")
    opened = k.state()["list"] == "projects"
    st = k.wait_state(lambda s: s["list"] == "closed", 14, every=1.0)
    return opened and st["list"] == "closed", "closed by itself after ~10 s"


def L4(r):
    k = r.k
    r.idle("track")
    k.send("e")
    first = k.state()
    st = k.wait_state(lambda s: s["entries_state"] in ("ok", "failed"), 15)
    rows = k.capture(r.png("tasks"))
    k.send("u")
    ok = first["list"] == "tasks" and st["entries_state"] == "ok" and st["entries"] > 0 and st["list_idx"] == 1
    return ok, f"{first['entries_state']} -> {st['entries_state']}, {st['entries']} entries, starts on row {st['list_idx']} (tasks.png)"


def L5(r):
    k = r.k
    r.idle("track")
    i = k.mark()
    for _ in range(2):
        k.send("e")
        k.wait_state(lambda s: s["entries_state"] in ("ok", "failed"), 15)
        k.send("u")
        time.sleep(1.0)
    n = sum(1 for l in k.since(i) if re.search(r"Toggl: \d+ recent entries", l))
    return n == 2, f"{n} fetches for 2 opens"


def L6(r):
    k = r.k
    r.idle("track")
    st = r.pick_in_list("e", 1)
    _, entries = k.lists()
    want = entries[0] if entries else None
    ok = bool(want) and st["task"] == want["label"] and st["project_id"] == want["project_id"]
    return ok, f"task {st['task']!r}, project {st['project']!r}"


def L7(r):
    k = r.k
    st0 = r.idle("track")
    k.cmd("task knob-test L7")
    st = r.pick_in_list("e", 0)
    return st["task"] == "" and st["project_id"] == st0["project_id"], "task cleared ('Untitled'), project kept"


def L8(r):
    # (Runs in the offline block.) A list that couldn't load has only the "Untitled"
    # placeholder: a tap closes it and keeps the task.
    k = r.k
    r.offline()
    r.idle("track")
    k.cmd("task knob-test L8 kept")
    k.send("e")
    failed = k.wait_state(lambda s: s["entries_state"] == "failed", 10)
    k.send("t")
    st = k.state()
    ok = failed["entries_state"] == "failed" and st["list"] == "closed" and st["task"] == "knob-test L8 kept" and st["mode"] == "idle"
    return ok, f"past tasks offline: {failed['entries_state']} (couldn't reach Toggl); a tap closed it, task {st['task']!r} kept"


# -- Settings ---------------------------------------------------------------------------------

def G1(r):
    k = r.k
    r.idle("track")
    i = k.mark()
    k.send(">")
    saved = k.wait(r"saved: ", 5, since=i)
    k.send("<")
    k.wait(r"saved: ", 5, since=k.mark())
    return bool(saved), "saved to flash about 2 s after the change" if saved else "no save logged"


def G2(r):
    k = r.k
    r.idle("pomo", length=17)
    k.cmd("theme inverted")
    k.cmd("task knob-test G2 survives restart")
    if r.projects:
        k.cmd(f"project {r.projects[0]['id']}")
    want = k.state()
    time.sleep(2.6)  # saved 2 s after the last change
    i = k.mark()
    k.cmd("reboot")
    k.wait(r"pomodoro: starting:", 15, since=i)
    got = k.wait_state(lambda s: s["wifi"], 20)
    keys = ("app", "theme", "set_min", "task", "project_id")
    ok = all(got[x] == want[x] for x in keys)
    return ok, "after reboot: " + ", ".join(f"{x}={got[x]!r}" for x in keys)


# -- Offline block: modes, Tracking, Pomodoro, hit areas (nothing reaches Toggl) ------------

def M1(r):
    k = r.k
    r.offline()
    st0 = r.idle("track")
    k.send("u")
    a = k.state()
    k.send("u")
    b = k.state()
    ok = a["app"] == "pomo" and a["status"] == "pomodoro" and b["app"] == "track" and b["status"] == "tracking"
    return ok, f"{st0['app']} -> {a['app']} ({a['status']!r}) -> {b['app']} ({b['status']!r})"


def M2(r):
    k = r.k
    r.offline()
    r.idle("track")
    k.send("s")
    k.send("u")
    st = k.state()
    k.send("s")
    k.send("r")
    ok = st["app"] == "track" and st["mode"] == "running" and st["status"] == "pause to switch"
    return ok, f"still {st['app']}, {st['mode']}, hint {st['status']!r}"


def M3(r):
    k = r.k
    r.offline()
    r.idle("track")
    k.send("s")
    time.sleep(0.5)
    k.send(">")
    a = k.state()
    k.send("<")
    b = k.state()
    k.send("s")
    k.send("r")
    ok = a["theme"] == "inverted" and a["mode"] == "running" and a["status"] == "inverted" and b["theme"] == "mono"
    return ok, f"while running: mono -> {a['theme']} ({a['status']!r}) -> {b['theme']}, timer still {a['mode']}"


def R1_R3(r):
    """Tracking: count-up, pause and resume, reset (all stretches under 10 s)."""
    k = r.k
    r.offline()
    r.idle("track")
    res = {}
    k.send("s")
    time.sleep(2.3)
    a = k.state()
    res["R1"] = (a["mode"] == "running" and a["elapsed_s"] >= 2 and a["time"].startswith("00:0") and a["status"] == "",
                 f"running, {a['time']} after 2.3 s, no status text")
    k.send("s")
    p = k.state()
    time.sleep(1.5)
    p2 = k.state()
    k.send("s")
    time.sleep(1.3)
    q = k.state()
    ok = (p["mode"] == "paused" and p2["elapsed_s"] == p["elapsed_s"] and p["status"] == "paused"
          and q["mode"] == "running" and q["elapsed_s"] > p["elapsed_s"])
    res["R2"] = (ok, f"paused at {p['time']}, still {p2['time']} 1.5 s later; resumed to {q['time']}")
    k.send("r")
    i1 = k.state()
    k.send("s")
    time.sleep(1.2)
    k.send("s")
    k.send("r")
    i2 = k.state()
    k.send("r")
    i3 = k.state()
    ok = all(s["mode"] == "idle" and s["time"] == "00:00" for s in (i1, i2, i3)) and i1["status"] in ("reset", "tap to track")
    res["R3"] = (ok, "reset to 00:00 from running and from paused; hold while reset changes nothing")
    return res


def R4(r):
    k = r.k
    r.offline()
    r.idle("track")
    k.send("+")
    idle = k.state()
    k.send("s")
    time.sleep(0.5)
    a = k.state()["elapsed_s"]
    k.send("+++", gap=0.25)
    b = k.state()["elapsed_s"]
    k.send("--", gap=0.25)
    c = k.state()["elapsed_s"]
    k.send("--", gap=0.25)  # the second one can't go below the start of this stretch
    d = k.state()["elapsed_s"]
    k.send("s")
    k.send("+")
    paused = k.state()
    k.send("r")
    ok = (idle["mode"] == "idle" and idle["elapsed_s"] == 0 and 180 <= b - a <= 183 and 60 <= c - a <= 63
          and d <= 2 and paused["mode"] == "paused" and paused["elapsed_s"] <= 2)
    return ok, f"running: {a} s, +3 clicks {b} s, -2 clicks {c} s, -2 more {d} s (clamped); paused/reset: no change"


def R5(r):
    k = r.k
    r.offline()
    r.idle("track")
    k.cmd("session 3725")
    time.sleep(0.4)
    a = k.state()
    rows = k.capture(r.png("track-hours"))
    k.cmd("session 59")
    b = k.state()
    k.send("r")
    wide = lit(rows, 30, 160, 60, 212, level=60) if rows else 0  # HH:MM:SS reaches further out than MM:SS (dimmed: paused)
    ok = a["time"] == "01:02:05" and b["time"] == "00:59" and wide > 30
    return ok, f"{a['time']} at 62 px (track-hours.png), {b['time']} under an hour"


def P1(r):
    k = r.k
    r.offline()
    m0 = r.idle("pomo")["set_min"]
    up, down = ("+", "-") if m0 < 60 else ("-", "+")
    k.send(up)
    m1 = k.state()["set_min"]
    k.send(down)
    m2 = k.state()["set_min"]
    k.cmd("set 1")
    k.send("-")
    low = k.state()
    k.cmd("set 60")
    k.send("+")
    high = k.state()
    k.cmd(f"set {m0}")
    ok = (abs(m1 - m0) == 1 and m2 == m0 and low["set_min"] == 1 and high["set_min"] == 60
          and high["time"] == "60:00")
    return ok, (f"{m0} -> {m1} -> {m2} min; stops at {low['set_min']} ({low['status']!r}) and {high['set_min']} "
                f"({high['status']!r}), shown as {high['time']}")


def P2(r):
    k = r.k
    r.offline()
    r.idle("pomo", length=25)
    k.send("s")
    time.sleep(1.2)
    a = k.state()
    k.send("+++")
    b = k.state()
    k.send("s")
    k.send("r")
    ok = (a["mode"] == "running" and b["set_min"] == 25 and a["remaining_s"] - 4 <= b["remaining_s"] <= a["remaining_s"]
          and b["status"] == "pause to adjust")
    return ok, f"3 clicks while running: {a['time']} -> {b['time']}, length {b['set_min']}, hint {b['status']!r}"


def P3(r):
    k = r.k
    r.offline()
    r.idle("pomo", length=25)
    k.send("s")
    time.sleep(2.2)
    k.send("s")
    p = k.state()  # 24:58 or so
    k.send("-")
    down = k.state()  # straight down to 24:00
    k.cmd(f"remaining {p['remaining_s']}")
    k.send("+")
    up = k.state()  # up to 25:00
    k.send("-")
    back = k.state()  # 24:00
    k.send("r")
    ok = (p["mode"] == "paused" and p["remaining_s"] % 60 and down["remaining_s"] == 24 * 60
          and up["remaining_s"] == 25 * 60 and back["remaining_s"] == 24 * 60)
    return ok, f"paused at {p['time']} -> down {down['time']}; from {p['time']} up {up['time']} -> down {back['time']}"


def P4(r):
    k = r.k
    r.offline()
    full = r.idle("pomo", length=25)["set_min"] * 60
    k.send("s")
    time.sleep(2.2)
    a = k.state()
    k.send("s")
    p = k.state()
    time.sleep(1.5)
    p2 = k.state()
    k.send("s")
    time.sleep(1.3)
    q = k.state()
    k.send("r")
    i1 = k.state()
    k.send("r")
    i2 = k.state()
    ok = (a["mode"] == "running" and a["remaining_s"] < full and p["mode"] == "paused"
          and p2["remaining_s"] == p["remaining_s"] and q["mode"] == "running" and q["remaining_s"] < p["remaining_s"]
          and i1["mode"] == "idle" and i1["remaining_s"] == full and i2["remaining_s"] == full)
    return ok, f"counted down to {a['time']}, paused at {p['time']}, resumed to {q['time']}, reset to {i1['time']}"


def log_ms(line):
    m = re.match(r"[IWE] \((\d+)\)", line)
    return int(m.group(1)) if m else None


def P5(r):
    k = r.k
    r.offline()
    m0 = r.idle("pomo")["set_min"]
    k.cmd("task knob-test P5 times up")
    k.cmd("set 1")
    i = k.mark()
    k.send("s")
    done = k.wait_state(lambda s: s["mode"] == "done", 75, every=1.0)
    cap = k.capture(r.png("timesup"))
    on = k.wait(r"time's up: vibrating for 30 s", 2, since=i)
    st = k.state()
    off = k.wait(r"time's up: vibration done", 35, since=i)
    after = k.state()
    k.send("s")  # tap dismisses
    back = k.state()
    k.cmd(f"set {m0}")
    secs = (log_ms(off.string) - log_ms(on.string)) / 1000 if on and off else 0
    ok = (done["pulsing"] and done["alarm"] and st["status"] == "time\u2019s up" and 29.5 <= secs <= 30.6
          and after["mode"] == "done" and not after["alarm"] and back["mode"] == "idle" and cap is not None)
    return ok, f"reached 00:00 with the pulse, vibrated for {secs:.1f} s, then a tap went back to reset (timesup.png)"


def P7(r):
    k = r.k
    r.offline()
    r.idle("pomo")
    k.cmd("remaining 2")  # time's up in 2 s
    i = k.mark()
    k.send("s")
    done = k.wait_state(lambda s: s["alarm"], 6, every=0.3)
    time.sleep(1.0)
    k.send("+")  # a knob turn silences it
    stopped = k.wait(r"time's up: vibration stopped by a touch or the knob", 3, since=i)
    st = k.state()
    k.send("s")
    back = k.state()
    ok = done["mode"] == "done" and done["alarm"] and bool(stopped) and st["mode"] == "done" and not st["alarm"] and back["mode"] == "idle"
    return ok, "time's up started the vibration; a knob turn stopped it early (still on \"time's up\"), a tap reset"


def P8(r):
    k = r.k
    r.offline()
    r.idle("pomo")
    k.cmd("remaining 3")
    k.send("s")
    k.send("o")  # a list is open when time runs out
    opened = k.state()["list"] == "projects"
    done = k.wait_state(lambda s: s["alarm"], 8, every=0.3)
    time.sleep(4)
    later = k.state()  # still vibrating: nothing else cut it short
    k.send("s")  # the silencing tap resets, it doesn't pick a row
    back = k.state()
    ok = (opened and done["mode"] == "done" and done["list"] == "closed" and done["status"] == "time\u2019s up"
          and later["alarm"] and back["mode"] == "idle" and back["project_id"] == done["project_id"])
    return ok, (f"list open at time's up: closed ({done['list']}), \"{done['status']}\" shown, still vibrating 4 s later: "
                f"{later['alarm']}; the tap reset without picking")


def P6(r):
    k = r.k
    r.offline()
    st = r.idle("pomo", length=15)
    if not st["project_id"] and r.projects:
        k.cmd(f"project {r.projects[0]['id']}")
        st = k.state()
    proj = next((p for p in r.projects if p["id"] == st["project_id"]), None)
    time.sleep(0.8)  # the arc glides
    a = k.capture(r.png("pomo-mono"))
    k.cmd("theme inverted")
    time.sleep(0.6)
    b = k.capture(r.png("pomo-inverted"))
    k.cmd("theme mono")
    if not (a and b and proj):
        return False, "no capture or project"
    k.cmd("set 25")  # 150°: the whole arc, from 12 o'clock on, must be filled (no gap behind its end)
    time.sleep(0.8)
    c = k.capture(r.png("pomo-25"))
    k.cmd("set 15")
    color = hexrgb(proj["color"])
    on, off = rim_px(a, 45), rim_px(a, 225)
    same = all(rim_px(a, d) == rim_px(b, d) for d in range(5, 360, 10))
    filled = c and all(near(rim_px(c, d), color, 30) for d in (5, 20, 45, 75, 100, 140))
    empty = c and all(max(rim_px(c, d)) < 90 for d in (160, 200, 250, 340))
    ok = near(on, color, 30) and max(off) < 90 and same and min(b[260][60]) > 230 and filled and empty
    return ok, (f"15 min: arc to 3 o'clock {on}, faint ring at 7:30 {off}; same rim in Inverted: {same}; "
                f"25 min: arc filled from 12 o'clock to 5 o'clock: {bool(filled and empty)} (pomo-*.png)")


def H1(r):
    """Hit areas (offline: a mic tap answers "no Wi-Fi", which shows it was hit)."""
    k = r.k
    r.offline()
    r.idle("track")
    i = k.mark()
    k.cmd("tap 180 282")  # the mic
    mic = bool(k.wait(r"voice: no Wi-Fi", 3, since=i))
    time.sleep(1.5)
    k.cmd("tap 220 282")  # 40 px right of the mic, below the status line: a near miss
    miss = k.state()
    k.cmd("tap 180 113")  # the task
    task = k.state()["list"]
    k.send("u")
    k.cmd("tap 180 64")  # the pill
    pill = k.state()["list"]
    k.send("u")
    k.cmd("tap 180 240")  # the status line (42 px above the mic): a normal tap
    status = k.state()["mode"]
    k.send("s")
    k.send("r")
    ok = mic and miss["mode"] == "idle" and miss["voice"] == "idle" and task == "tasks" and pill == "projects" and status == "running"
    return ok, (f"mic {'hit' if mic else 'MISSED'}; near miss did nothing; task -> {task} list; pill -> {pill} list; "
                f"status line tap -> {status}")


# -- Voice (online) ---------------------------------------------------------------------------

def V1_V2(r):
    k = r.k
    r.online()
    r.idle("track")
    phrase = r.a.phrase
    i = k.mark()
    k.send("m")
    subprocess.Popen(["say", phrase])
    time.sleep(1.6)
    rows = k.capture(r.png("listening"))
    bars = lit(rows, 105, 226, 256, 251) if rows else 0
    m = k.wait(r'heard: "(.*)"', 45, since=i)  # up to 15 s of talking + upload
    res = {"V1": (bars > 100, f"waveform: {bars} px lit (listening.png)")}
    if not m:
        why = k.wait(r"voice: (.*)", 1, since=i)
        res["V2"] = (False, f"no transcript ({why.group(1) if why else 'timeout'})")
        return res
    heard = m.group(1)
    ms = k.wait(r"transcription: HTTP 200 in (\d+) ms", 1, since=i)
    overlap = len(words(heard) & words(phrase)) / max(1, len(words(phrase)))
    st = k.state()
    ok = overlap >= 0.6 and st["task"] == heard and ms and int(ms.group(1)) <= 15000
    res["V2"] = (ok, f'heard "{heard}" ({overlap:.0%} of words) in {ms.group(1) if ms else "?"} ms')
    return res


def V3(r):
    k = r.k
    r.online()
    r.idle("track")
    # A distinctive name (5+ letters), in a phrase that contains no other project's name.
    names = {p["name"].lower() for p in r.projects}
    candidates = [p for p in r.projects if re.fullmatch(r"[A-Za-z][A-Za-z ]{4,20}", p["name"])]
    phrase_for = lambda p: f"{p['name']} session"
    candidates = [p for p in candidates if not (words(phrase_for(p)) - words(p["name"])) & names]
    if not candidates:
        return False, "no project with a distinctive, speakable name"
    proj = sorted(candidates, key=lambda p: len(p["name"]))[0]
    k.cmd("project 0")
    i = k.mark()
    k.send("m")
    subprocess.Popen(["say", phrase_for(proj)])
    m = k.wait(r'heard: "(.*)"', 45, since=i)
    st = k.state()
    return st["project_id"] == proj["id"], f'said "{phrase_for(proj)}", heard "{m.group(1) if m else "?"}", project {st["project"]!r}'


def V4(r):
    k = r.k
    r.online()
    st0 = r.idle("track")
    i = k.mark()
    k.cmd("tap 180 282")
    time.sleep(0.7)
    k.send("t")
    msg = k.wait(r"voice: didn't catch that", 15, since=i)
    st = k.state()
    return bool(msg) and st["task"] == st0["task"], "\"didn't catch that\", task unchanged"


# -- Toggl (online; creates entries named "knob-test ...") ----------------------------------

def started(k, desc, since, timeout=15):
    m = k.wait(rf'Toggl entry (\d+) started: "{re.escape(desc)}"', timeout, since=since)
    return m.group(1) if m else None


def S1(r):
    k = r.k
    r.online()
    r.idle("track")
    k.cmd("task knob-test S1 tracked")
    i = k.mark()
    t0 = time.time()
    k.send("s")
    eid = started(k, "knob-test S1 tracked", i)
    lag = time.time() - t0
    st = k.state()
    time.sleep(max(0, 11 - (time.time() - t0)))
    k.send("s")
    stopped = k.wait(rf"Toggl entry {eid} stopped", 20, since=i) if eid else None
    k.send("r")
    ok = bool(eid and stopped) and lag < 8 and str(st["toggl_entry"]) == eid
    return ok, f"entry {eid} started {lag:.1f} s after the tap, stopped on pause after 11 s"


def S2(r):
    k = r.k
    r.online()
    r.idle("track")
    k.cmd("task knob-test S2 accidental")
    i = k.mark()
    k.send("s")
    eid = started(k, "knob-test S2 accidental", i)
    k.send("s")
    deleted = k.wait(rf"Toggl entry {eid} deleted", 20, since=i) if eid else None
    k.send("r")
    return bool(eid and deleted), f"entry {eid} started, then deleted when paused within 10 s"


def S3(r):
    k = r.k
    r.online()
    r.idle("track")
    k.cmd("task knob-test S3 adjusted")
    i = k.mark()
    k.send("s")
    k.send("+" * 12, gap=0.05)  # a fast spin while the start request is still on its way
    eid = started(k, "knob-test S3 adjusted", i)
    upd = k.wait(rf"Toggl entry {eid} updated \(start\): start (\S+)", 15, since=i) if eid else None
    st = k.state()
    time.sleep(2)
    n_upd = sum(1 for l in k.since(i) if re.search(rf"Toggl entry {eid} updated", l))
    k.send("s")
    stopped = k.wait(rf"Toggl entry {eid} stopped", 20, since=i) if eid else None
    k.send("r")
    ok = bool(eid and upd and stopped) and 720 <= st["elapsed_s"] < 780 and n_upd == 1
    return ok, (f"12 fast clicks: {st['time']} on the knob, entry {eid} start moved 12 min earlier in one update "
                f"(start only), then stopped")


def S4(r):
    k = r.k
    r.online()
    r.idle("track")
    k.cmd("task knob-test S4 first")
    i = k.mark()
    k.send("s")
    first = started(k, "knob-test S4 first", i)
    time.sleep(11)
    # Switch to a knob-test entry from the list (not one of your real tasks).
    k.send("e")
    k.wait_state(lambda s: s["entries_state"] in ("ok", "failed"), 15)
    _, entries = k.lists()
    idx = next((n for n, e in enumerate(entries) if e["label"].startswith("knob-test S")), None)
    k.send("u")
    if idx is None:
        k.send("s")
        k.send("r")
        return False, "no knob-test entry in the list to switch to"
    label = entries[idx]["label"]
    before = k.state()["elapsed_s"]
    st = r.pick_in_list("e", idx + 1)
    second = started(k, label, i, timeout=20)
    stopped = k.wait(rf"Toggl entry {first} stopped", 20, since=i) if first else None
    after = k.state()
    # Picking the same task again (it's in the list from an earlier run) changes nothing in Toggl.
    k.send("e")
    k.wait_state(lambda s: s["entries_state"] in ("ok", "failed"), 15)
    _, entries = k.lists()
    k.send("u")
    same = next((n for n, e in enumerate(entries) if e["label"] == label and e["project_id"] == after["project_id"]), None)
    j = k.mark()
    again = r.pick_in_list("e", same + 1) if same is not None else None
    time.sleep(1.5)
    unchanged = bool(again) and bool(k.wait(r"task from the list: unchanged", 1, since=j)) \
        and not any(re.search(r"Toggl entry \d+ (started|stopped)", l) for l in k.since(j)) \
        and str(again["toggl_entry"]) == second
    k.send("s")
    k.send("r")
    ok = (bool(first and second and stopped) and st["task"] == label and after["mode"] == "running"
          and after["elapsed_s"] >= before and unchanged)
    return ok, (f"entry {first} stopped, {second} started for {label!r}; the counter went on ({before} -> {after['elapsed_s']} s); "
                f"picking it again changed nothing: {unchanged}")


def S5(r):
    k = r.k
    r.online()
    r.idle("track")
    res_ok, notes = True, []
    i = k.mark()
    k.cmd("toggl start knob-test S5 elsewhere")
    m = k.wait(r"debug: started Toggl entry (\d+) elsewhere \(HTTP 200\)", 15, since=i)
    eid = m.group(1) if m else None
    k.send("p")
    adopted = k.wait_state(lambda s: not s["syncing"], 20)
    a_ok = bool(eid) and adopted["mode"] == "running" and adopted["task"] == "knob-test S5 elsewhere" \
        and str(adopted["toggl_entry"]) == eid and bool(k.wait(r"synced from Toggl", 1, since=i))
    notes.append(f"adopted {eid}: {adopted['task']!r} {adopted['mode']}")
    j = k.mark()
    k.send("+")  # only the start goes to Toggl, never the knob's (possibly cut) copy of the description
    upd = k.wait(rf"Toggl entry {eid} updated \((.*?)\)", 15, since=j)
    u_ok = bool(upd) and upd.group(1) == "start"
    notes.append(f"knob turn sent ({upd.group(1) if upd else 'nothing'})")
    j = k.mark()
    k.cmd("toggl stop")
    k.wait(r"debug: stopped the Toggl timer elsewhere", 15, since=j)
    k.send("p")
    stopped = k.wait_state(lambda s: not s["syncing"], 20)
    s_ok = stopped["mode"] == "paused" and bool(k.wait(r"pomodoro: stopped in Toggl", 1, since=j))
    notes.append(f"stopped elsewhere -> {stopped['mode']}")
    j = k.mark()
    k.send("p")
    k.wait_state(lambda s: not s["syncing"], 20)
    i_ok = bool(k.wait(r"in sync with Toggl", 2, since=j))
    notes.append("then in sync" if i_ok else "no 'in sync'")
    k.send("r")
    # An adopted timer paused on the knob within seconds is stopped, not deleted (the 10 s
    # rule is for the knob's own accidental starts).
    j = k.mark()
    k.cmd("toggl start knob-test S5 adopted")
    m2 = k.wait(r"debug: started Toggl entry (\d+) elsewhere \(HTTP 200\)", 15, since=j)
    eid2 = m2.group(1) if m2 else None
    k.send("p")
    k.wait_state(lambda s: not s["syncing"], 20)
    k.send("s")
    kept = k.wait(rf"Toggl entry {eid2} (stopped|deleted)", 20, since=j) if eid2 else None
    d_ok = bool(kept) and kept.group(1) == "stopped"
    notes.append(f"adopted and paused at once: {kept.group(1) if kept else 'nothing'}")
    k.send("r")
    return a_ok and u_ok and s_ok and i_ok and d_ok, "; ".join(notes)


def S6(r):
    k = r.k
    r.online()
    r.idle("track")
    k.cmd("task knob-test S6 offline run")
    r.offline()
    i = k.mark()
    k.send("s")
    time.sleep(12)
    k.send("s")
    r.online()
    logged = k.wait(r'Toggl entry (\d+) logged: "knob-test S6 offline run"', 40, since=i)
    k.send("r")
    return bool(logged), "12 s run while offline, logged after Wi-Fi came back"


def S7(r):
    k = r.k
    r.online()
    r.idle("pomo", length=25)
    k.cmd("task knob-test S7 pomodoro")
    i = k.mark()
    k.send("s")
    eid = started(k, "knob-test S7 pomodoro", i)
    time.sleep(11)
    k.send("r")
    stopped = k.wait(rf"Toggl entry {eid} stopped" if eid else "$^", 20, since=i)
    return bool(eid and stopped), f"Pomodoro entry {eid} started right away, stopped by reset"


def X1(r):
    k = r.k
    r.online()
    i = k.mark()
    k.cmd("telemetry on")
    m1 = k.wait(r"^TELEMETRY (\{.*\})$", 8, since=i)
    j = k.mark()
    m2 = k.wait(r"^TELEMETRY (\{.*\})$", 8, since=j)
    k.cmd("telemetry off")
    time.sleep(0.5)
    q = k.mark()
    quiet = not k.wait(r"^TELEMETRY ", 7, since=q)
    if r.a.telemetry:
        k.cmd("telemetry on")
    fields = json.loads(m1.group(1)) if m1 else {}
    need = {"free_internal_kb", "min_free_internal_kb", "rssi", "wifi", "toggl_ready", "toggl_pending", "http_last_status", "app"}
    ok = bool(m1 and m2) and need <= set(fields) and quiet
    return ok, f"every ~5 s with {len(fields)} fields (rssi {fields.get('rssi')} dBm); stopped when turned off"


CASES = [("B1", B1), ("B2", B2), ("B3", B3),
         ("D1", D1), ("D2", D2), ("D3", D3), ("D4", D4),
         ("L1", L1), ("L2", L2), ("L3", L3), ("L4", L4), ("L5", L5), ("L6", L6), ("L7", L7), ("G1", G1),
         # Offline block: nothing below reaches Toggl until Wi-Fi comes back.
         ("M1", M1), ("M2", M2), ("M3", M3), ("R1-R3", R1_R3), ("R4", R4), ("R5", R5),
         ("P1", P1), ("P2", P2), ("P3", P3), ("P4", P4), ("P5", P5), ("P6", P6), ("P7", P7), ("P8", P8), ("H1", H1),
         ("L8", L8),
         ("V1-V2", V1_V2), ("V3", V3), ("V4", V4),
         ("S1", S1), ("S2", S2), ("S3", S3), ("S4", S4), ("S5", S5), ("S6", S6), ("S7", S7),
         ("X1", X1), ("G2", G2), ("B5", B5), ("B4", B4)]


def save_original(r):
    st = r.k.state()
    r.original = {x: st[x] for x in ("app", "theme", "set_min", "task", "task_id", "project_id", "project", "project_color",
                                     "telemetry")}


def restore(r):
    k, o = r.k, r.original
    if not o:
        return
    r.online()
    r.idle(o["app"], theme=o["theme"])
    k.cmd(f"set {o['set_min']}")
    k.cmd("task " + o["task"])
    k.cmd(f"task_id {o['task_id']}")  # the Toggl task the name belongs to (#task clears it)
    if o["project_id"] > 0:  # (with its color and name, in case it isn't in the active list)
        k.cmd(f"project {o['project_id']} {o['project_color']} {o['project']}")
    elif o["project_id"] == 0:
        k.cmd("project 0")
    k.cmd("telemetry " + ("on" if o["telemetry"] else "off"))
    time.sleep(2.6)  # let it save


def case_ids(name):
    if "-" not in name:
        return [name]
    first, last = name.split("-")
    return [f"{first[0]}{n}" for n in range(int(first[1:]), int(last[1:]) + 1)]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/cu.usbmodem101")
    ap.add_argument("--quick", action="store_true", help="skip slow cases and side effects")
    ap.add_argument("--only", help="comma-separated IDs, e.g. R4,S5,V2")
    ap.add_argument("--phrase", default="Review the quarterly planning document")
    ap.add_argument("--telemetry", action="store_true", help="also record TELEMETRY lines (telemetry.jsonl)")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test-output"))
    a = ap.parse_args()
    os.makedirs(a.out, exist_ok=True)
    only = set(a.only.upper().split(",")) if a.only else None

    k = Knob(a.port)
    r = Run(k, a)
    print(f"Connected to {a.port}; the knob restarts when the port opens.")
    if not k.wait(r"pomodoro: starting:", 6, since=0):
        k.cmd("reboot")  # the port opened without a reset: force a fresh boot
        k.wait(r"pomodoro: starting:", 15)
    results = {}
    t_start = time.time()
    try:
        k.wait_state(lambda s: s["wifi"] and s["toggl"] and s["projects"] > 0, 30)  # projects arrive just after Toggl connects
        save_original(r)
        r.projects, r.entries = k.lists()
        if a.telemetry:
            k.cmd("telemetry on")
        for name, fn in CASES:
            ids = case_ids(name)
            always = name in ("B1", "B2", "B4")
            if not always and only and not (set(ids) & only):
                continue
            if not always and a.quick and (set(ids) & (SLOW | SIDE_EFFECTS)):
                print(f"  skip  {name:<6} (--quick)")
                continue
            t0 = time.time()
            try:
                out = fn(r)
            except Exception as e:
                out = (False, f"{type(e).__name__}: {e}")
            outs = out if isinstance(out, dict) else {name: out}
            for case, (ok, detail) in outs.items():
                results[case] = {"pass": bool(ok), "detail": detail}
                print(f"  {'PASS' if ok else 'FAIL'}  {case:<6} {detail}  [{time.time() - t0:.1f} s]", flush=True)
    finally:
        try:
            restore(r)
        except Exception as e:
            print(f"  (could not restore your settings: {e})")
        lines = k.since()
        with open(os.path.join(a.out, "serial.log"), "w") as f:
            f.write("\n".join(re.sub(r'joining Wi-Fi ".*?"', 'joining Wi-Fi "<ssid>"', l) for l in lines))
        if a.telemetry:
            with open(os.path.join(a.out, "telemetry.jsonl"), "w") as f:
                f.write("\n".join(l[len("TELEMETRY "):] for l in lines if l.startswith("TELEMETRY ")))
        with open(os.path.join(a.out, "report.json"), "w") as f:
            json.dump({"duration_s": round(time.time() - t_start), "results": results}, f, indent=2)
        k.close()
    passed = sum(1 for v in results.values() if v["pass"])
    print(f"{passed}/{len(results)} passed in {time.time() - t_start:.0f} s. Output: {os.path.abspath(a.out)}")
    sys.exit(0 if passed == len(results) else 1)


if __name__ == "__main__":
    main()
