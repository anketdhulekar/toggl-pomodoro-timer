#!/usr/bin/env python3
"""End-to-end tests for the Knob Pomodoro, run against the real device over USB.

Implements every case in TESTS.md (IDs B1..M1). The firmware's debug interface
(serial_commands() in main/main.c) is driven over the USB serial port:

  single letters   + - (knob)  s (start/pause)  r (hold = reset)  < > (swipe left/right)
                   p u (swipe down/up)  e (entries button)  m (mic)  t (tap: choose / stop listening)
                   d (print STATE json)  c (capture the screen)
  "#" lines        #tap X Y   #task TEXT   #project ID   #set N   #wifi on|off
                   #telemetry on|off   #list   #reboot

Usage (from firmware/, with the ESP-IDF environment loaded: . ../idf-env.sh):
    python tools/knob_test.py                 # everything (speaks through the Mac, creates Toggl entries)
    python tools/knob_test.py --quick         # no slow cases, no side effects (about 2 minutes)
    python tools/knob_test.py --only K1,T5    # chosen cases (B1, B2 and B4 always run)
    python tools/knob_test.py --telemetry     # also record TELEMETRY lines to test-output/telemetry.jsonl

Your settings (length, profile, task, project, telemetry) are restored at the end.
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
SLOW = {"T5", "S1", "S2", "S3", "S4"}            # skipped by --quick
SIDE_EFFECTS = {"V1", "V2", "V3", "S1", "S2", "S3", "S4"}  # speak aloud / create Toggl entries


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

    def png(self, name):
        return os.path.join(self.out, name + ".png")

    def idle(self, length=None):
        """Back to a reset timer with no list open and the buttons visible."""
        k = self.k
        st = k.state()
        if st["voice"] == "listening":
            k.send("t")
            k.wait_state(lambda s: s["voice"] == "idle", 15)
        elif st["voice"] == "waiting":
            k.wait_state(lambda s: s["voice"] == "idle", 15)
        if st["list"] != "closed":
            k.send("u")
        st = k.state()
        if st["mode"] == "running":
            k.send("s")
        if st["mode"] != "idle":
            k.send("r")
        if length:
            k.cmd(f"set {length}")
        time.sleep(1.8)  # hints and profile dots fade; the buttons come back
        return k.state()

    def pick_in_list(self, open_key, target_idx):
        """Open a list, move to row target_idx and choose it."""
        k = self.k
        k.send(open_key)
        if open_key == "e":
            st = k.wait_state(lambda s: s["list"] == "entries" and s["entries_state"] != "loading", 15)
        else:
            st = k.wait_state(lambda s: s["list"] == "projects", 5)
        steps = target_idx - st["list_idx"]
        k.send(("+" if steps > 0 else "-") * abs(steps))
        k.send("t")
        return k.state()


# ---- Cases ------------------------------------------------------------------------------
# Each takes the Run and returns (ok, detail).

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
    # frames is dominated by one-off redraws (e.g. rebuilding the background after a reset).
    ts = [float(m.group(2)) for l in r.k.since() for m in [re.search(r"(\d+) frames in 10 s: ([0-9.]+) ms", l)]
          if m and int(m.group(1)) >= 30]
    if not ts:
        return True, "no animated windows logged yet"
    return max(ts) <= 45, f"{len(ts)} animated windows, {min(ts):.1f}-{max(ts):.1f} ms per frame (limit 45, i.e. 22+ fps)"


def K1(r):
    k = r.k
    m0 = r.idle()["set_min"]
    up, down = ("+", "-") if m0 < 60 else ("-", "+")
    k.send(up)
    m1 = k.state()["set_min"]
    k.send(down)
    m2 = k.state()["set_min"]
    return abs(m1 - m0) == 1 and m2 == m0, f"{m0} -> {m1} -> {m2} min"


def K2(r):
    k = r.k
    m0 = r.idle()["set_min"]
    k.cmd("set 1")
    k.send("-")
    low = k.state()["set_min"]
    k.cmd("set 60")
    k.send("+")
    high = k.state()["set_min"]
    k.cmd(f"set {m0}")
    return low == 1 and high == 60, f"stops at {low} and {high} min"


def K3(r):
    k = r.k
    r.idle(25)
    k.send("s")
    time.sleep(1.2)
    a = k.state()
    k.send("+++")
    b = k.state()
    k.send("s")
    k.send("r")
    ok = a["mode"] == "running" and b["set_min"] == 25 and a["remaining_s"] - 4 <= b["remaining_s"] <= a["remaining_s"]
    return ok, f"3 clicks while running: {a['remaining_s']} s -> {b['remaining_s']} s, length {b['set_min']}"


def K4(r):
    k = r.k
    r.idle(25)
    k.send("s")
    time.sleep(2.2)
    k.send("s")
    p = k.state()
    minutes = (p["remaining_s"] - 1) // 60 if p["remaining_s"] % 60 == 0 else p["remaining_s"] // 60
    k.send("+")
    up = k.state()
    k.send("-")
    down = k.state()
    k.send("r")
    ok = p["mode"] == "paused" and up["remaining_s"] == (minutes + 1) * 60 and down["remaining_s"] == minutes * 60
    fmt = lambda s: f"{s // 60}:{s % 60:02d}"
    return ok, f"paused at {fmt(p['remaining_s'])} -> up {fmt(up['remaining_s'])} -> down {fmt(down['remaining_s'])}"


def T1_T4(r):
    """T1-T4 share one run so it stays under 10 s (nothing goes to Toggl)."""
    k = r.k
    full = r.idle(25)["set_min"] * 60
    res = {}
    k.send("s")
    time.sleep(2.2)
    a = k.state()
    res["T1"] = (a["mode"] == "running" and a["remaining_s"] < full, f"running, {full} -> {a['remaining_s']} s")
    k.send("s")
    p = k.state()
    time.sleep(1.5)
    p2 = k.state()
    res["T2"] = (p["mode"] == "paused" and p2["remaining_s"] == p["remaining_s"], f"paused at {p['remaining_s']} s, still {p2['remaining_s']} s after 1.5 s")
    k.send("s")
    time.sleep(1.3)
    q = k.state()
    res["T3"] = (q["mode"] == "running" and q["remaining_s"] < p["remaining_s"], f"resumed, {p['remaining_s']} -> {q['remaining_s']} s")
    k.send("r")
    i1 = k.state()
    k.send("s")
    time.sleep(0.6)
    k.send("s")
    k.send("r")
    i2 = k.state()
    k.send("r")
    i3 = k.state()
    ok = all(s["mode"] == "idle" and s["remaining_s"] == full for s in (i1, i2, i3))
    res["T4"] = (ok, f"reset from running and from paused to {full} s; hold while reset changes nothing")
    return res


def T5(r):
    k = r.k
    st0 = r.idle()
    m0 = st0["set_min"]
    k.cmd("task knob-test T5 times up")
    k.cmd("set 1")
    i = k.mark()
    k.send("s")
    done = k.wait_state(lambda s: s["mode"] == "done", 75, every=1.0)
    cap = k.capture(r.png("timesup"))
    up = k.wait(r"time's up", 1, since=i)
    toggl = k.wait(r"Toggl entry \d+ stopped", 15, since=i) if not r.a.quick else None
    k.send("s")  # tap dismisses
    back = k.state()
    k.cmd(f"set {m0}")
    ok = done["mode"] == "done" and done["pulsing"] and bool(up) and back["mode"] == "idle" and cap is not None
    extra = ", Toggl entry stopped at 0:00" if toggl else ""
    return ok, f"reached 0:00 with the pulse, tap went back to reset (timesup.png){extra}"


def G1(r):
    k = r.k
    p0 = r.idle()["palette"]
    k.send(">")
    p1 = k.state()["palette"]
    k.send("<" * 8)
    p2 = k.state()["palette"]
    k.send("<")
    p3 = k.state()["palette"]
    k.send(">")
    p4 = k.state()["palette"]
    ok = p1 == (p0 - 1) % 8 and p2 == p1 and p3 == p0 and p4 == p1 and True
    k.send("<")
    return ok, f"profile {p0} -> {p1}, full cycle back to {p2}, wraps"


def G2(r):
    k = r.k
    r.idle()
    i = k.mark()
    k.send(">")
    saved = k.wait(r"saved: ", 5, since=i)
    k.send("<")
    k.wait(r"saved: ", 5, since=k.mark())
    return bool(saved), "saved to flash about 2 s after the change" if saved else "no save logged"


def G3(r):
    k = r.k
    st = r.idle()
    proj = r.projects[0] if r.projects else None
    k.cmd("set 17")
    k.send(">")
    k.cmd("task knob-test G3 survives restart")
    if proj:
        k.cmd(f"project {proj['id']}")
    want = k.state()
    time.sleep(2.6)  # saved 2 s after the last change
    i = k.mark()
    k.cmd("reboot")
    k.wait(r"pomodoro: starting:", 15, since=i)
    got = k.wait_state(lambda s: s["wifi"], 20)
    keys = ("set_min", "palette", "task", "project_id")
    ok = all(got[x] == want[x] for x in keys)
    return ok, "after reboot: " + ", ".join(f"{x}={got[x]!r}" for x in keys)


def G4(r):
    k = r.k
    st = r.idle(25)
    rows = k.capture(r.png("buttons-stopped"))
    shown = st["mic_shown"] and st["entries_shown"] and rows and lit_at(rows, 180, 298, 22) > 150 and lit_at(rows, 180, 60, 22) > 150
    k.send("s")
    time.sleep(0.5)
    run = k.state()
    rows2 = k.capture(r.png("buttons-running"))
    hidden = not run["mic_shown"] and not run["entries_shown"] and rows2 and lit_at(rows2, 180, 298, 22) < 40
    k.send("s")
    paused = k.state()
    k.send("r")
    ok = bool(shown and hidden and paused["mic_shown"] and paused["entries_shown"])
    return ok, "shown when reset and paused, hidden while running (buttons-*.png)"


def G5(r):
    k = r.k
    r.idle(25)
    k.cmd("tap 180 298")  # the mic
    on_mic = k.state()
    k.send("t")
    k.wait_state(lambda s: s["voice"] == "idle", 15)
    time.sleep(1.5)
    k.cmd("tap 222 298")  # 42 px right of the mic: dead zone
    dead1 = k.state()
    k.cmd("tap 180 102")  # 42 px below the entries badge: dead zone
    dead2 = k.state()
    k.cmd("tap 180 180")  # the middle
    mid = k.state()
    k.send("s")
    k.send("r")
    ok = (on_mic["voice"] == "listening" and dead1["mode"] == "idle" and dead1["voice"] == "idle"
          and dead2["mode"] == "idle" and dead2["list"] == "closed" and mid["mode"] == "running")
    return ok, "mic tap listens; taps 42 px off a button do nothing; a tap in the middle starts"


def G6(r):
    k = r.k
    st = r.idle()
    pid = st["project_id"]
    if pid:
        k.cmd("project 0")
        time.sleep(0.5)
    rows = k.capture(r.png("wifi"))
    if pid:
        k.cmd(f"project {pid}")
    n = lit_at(rows, 180, 341, 9, level=50) if rows else 0
    return st["wifi"] and n > 8, f"glyph in the rim: {n} px lit (wifi.png)"


def P1(r):
    k = r.k
    r.idle()
    k.send("p")
    st = k.state()
    rows = k.capture(r.png("projects"))
    k.send("u")
    closed = k.state()
    ok = st["list"] == "projects" and st["projects"] > 0 and closed["list"] == "closed" and rows is not None
    return ok, f"{st['projects']} projects + 'No project' (projects.png); swipe up closed it"


def P2(r):
    k = r.k
    r.idle(25)
    if not r.projects:
        return False, "no projects"
    proj = r.projects[0]
    st = r.pick_in_list("p", 1)
    rows = k.capture(r.png("rim"))
    rim = rows[14][224] if rows else (0, 0, 0)
    picked = st["project_id"] == proj["id"] and near(rim, hexrgb(proj["color"]))
    cleared = r.pick_in_list("p", 0)["project_id"] == 0
    return picked and cleared, f"picked {proj['name']!r} (rim {rim} vs {proj['color']}), then 'No project' cleared it"


def P3(r):
    k = r.k
    r.idle()
    k.send("p")
    opened = k.state()["list"] == "projects"
    st = k.wait_state(lambda s: s["list"] == "closed", 14, every=1.0)
    return opened and st["list"] == "closed", "closed by itself after ~10 s"


def E1(r):
    k = r.k
    r.idle()
    k.send("e")
    first = k.state()
    st = k.wait_state(lambda s: s["entries_state"] in ("ok", "failed"), 15)
    rows = k.capture(r.png("entries"))
    k.send("u")
    ok = first["list"] == "entries" and st["entries_state"] == "ok" and st["entries"] > 0 and st["list_idx"] == 1
    return ok, f"{first['entries_state']} -> {st['entries_state']}, {st['entries']} entries, starts on row {st['list_idx']} (entries.png)"


def E2(r):
    k = r.k
    r.idle()
    i = k.mark()
    for _ in range(2):
        k.send("e")
        k.wait_state(lambda s: s["entries_state"] in ("ok", "failed"), 15)
        k.send("u")
        time.sleep(1.8)
    n = sum(1 for l in k.since(i) if re.search(r"Toggl: \d+ recent entries", l))
    return n == 2, f"{n} fetches for 2 presses"


def E3(r):
    k = r.k
    r.idle()
    _, entries = r.k.lists()
    st = r.pick_in_list("e", 1)
    _, entries = k.lists()
    want = entries[0] if entries else None
    ok = bool(want) and st["task"] == want["label"] and st["project_id"] == want["project_id"]
    return ok, f"task {st['task']!r}, project {st['project']!r}"


def E4(r):
    st = r.pick_in_list("e", 0)
    return st["task"] == "" and st["project_id"] == 0, "task and project cleared"


def D1(r):
    k = r.k
    r.idle()
    k.cmd("task knob-test two lines")
    if r.projects:
        k.cmd(f"project {r.projects[0]['id']}")
    time.sleep(0.5)
    rows = k.capture(r.png("two-lines"))
    task, proj = lit(rows, 70, 95, 290, 113), lit(rows, 70, 117, 290, 132)
    return task > 150 and proj > 80, f"task line {task} px, project line {proj} px (two-lines.png)"


def D2(r):
    k = r.k
    r.idle()
    k.cmd("task knob-test this task name is much too long to fit across the round screen")
    time.sleep(2.6)
    a = k.capture(r.png("scroll-1"))
    time.sleep(2.5)
    b = k.capture(r.png("scroll-2"))
    d = diff(a, b, 60, 90, 300, 114) if a and b else 0
    return d > 200, f"{d} px of the task line changed in 2.5 s (scroll-*.png)"


def V1_V2(r):
    k = r.k
    r.idle()
    phrase = r.a.phrase
    i = k.mark()
    k.send("m")
    subprocess.Popen(["say", phrase])
    time.sleep(1.6)
    rows = k.capture(r.png("listening"))
    bars = lit(rows, 105, 236, 256, 261) if rows else 0
    m = k.wait(r'heard: "(.*)"', 45, since=i)  # up to 15 s of talking + upload
    res = {"V1": (bars > 120, f"waveform: {bars} px lit (listening.png)")}
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
    r.idle()
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
    m = k.wait(r'heard: "(.*)"', 45, since=i)  # up to 15 s of talking + upload
    st = k.state()
    return st["project_id"] == proj["id"], f'said "{phrase_for(proj)}", heard "{m.group(1) if m else "?"}", project {st["project"]!r}'


def V4(r):
    k = r.k
    st0 = r.idle()
    i = k.mark()
    k.cmd("tap 180 298")
    time.sleep(0.7)
    k.send("t")
    msg = k.wait(r"voice: didn't catch that", 15, since=i)
    st = k.state()
    return bool(msg) and st["task"] == st0["task"], "\"didn't catch that\", task unchanged"


def S1(r):
    k = r.k
    r.idle(25)
    k.cmd("task knob-test S1 short run")
    i = k.mark()
    k.send("s")
    time.sleep(5)
    k.send("s")
    started = k.wait(r"Toggl entry \d+ (started|logged)", 13, since=i)
    k.send("r")
    return not started, "5 s run, no entry after 13 s"


def S2(r):
    k = r.k
    r.idle(25)
    k.cmd("task knob-test S2 logged run")
    i = k.mark()
    k.send("s")
    started = k.wait(r'Toggl entry (\d+) started: "knob-test S2 logged run"', 25, since=i)
    k.send("s")
    if started:
        stopped = k.wait(rf"Toggl entry {started.group(1)} stopped", 20, since=i)
        k.send("r")
        return bool(stopped), f"entry {started.group(1)} started after 10 s and stopped on pause"
    # A network hiccup at the start: the run must still end up in Toggl, complete.
    logged = k.wait(r'Toggl entry (\d+) logged: "knob-test S2 logged run"', 40, since=i)
    k.send("r")
    return bool(logged), "start failed (network), but the complete entry was logged afterwards" if logged else "never logged"


def S3(r):
    k = r.k
    r.idle(25)
    k.cmd("task knob-test S3 reset run")
    i = k.mark()
    k.send("s")
    started = k.wait(r'Toggl entry (\d+) started: "knob-test S3 reset run"', 25, since=i)
    k.send("r")
    stopped = k.wait(rf"Toggl entry {started.group(1)} stopped" if started else "$^", 20, since=i)
    return bool(started and stopped), f"entry {started.group(1) if started else '?'} stopped by reset"


def S4(r):
    k = r.k
    r.idle(25)
    k.cmd("task knob-test S4 offline run")
    k.cmd("wifi off")
    off = k.wait_state(lambda s: not s["wifi"], 10)
    i = k.mark()
    k.send("s")
    time.sleep(12)
    k.send("s")
    k.cmd("wifi on")
    k.wait_state(lambda s: s["wifi"], 30)
    logged = k.wait(r'Toggl entry (\d+) logged: "knob-test S4 offline run"', 40, since=i)
    k.send("r")
    return not off["wifi"] and bool(logged), "12 s run while offline, logged after Wi-Fi came back"


def M1(r):
    k = r.k
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
    need = {"free_internal_kb", "min_free_internal_kb", "rssi", "wifi", "toggl_ready", "toggl_pending", "http_last_status"}
    ok = bool(m1 and m2) and need <= set(fields) and quiet
    return ok, f"every ~5 s with {len(fields)} fields (rssi {fields.get('rssi')} dBm); stopped when turned off"


CASES = [("B1", B1), ("B2", B2), ("B3", B3), ("K1", K1), ("K2", K2), ("K3", K3), ("K4", K4), ("T1-T4", T1_T4),
         ("T5", T5), ("G1", G1), ("G2", G2), ("G4", G4), ("G5", G5), ("G6", G6), ("P1", P1), ("P2", P2), ("P3", P3),
         ("E1", E1), ("E2", E2), ("E3", E3), ("E4", E4), ("D1", D1), ("D2", D2), ("V1-V2", V1_V2), ("V3", V3),
         ("V4", V4), ("S1", S1), ("S2", S2), ("S3", S3), ("S4", S4), ("M1", M1), ("G3", G3), ("B5", B5), ("B4", B4)]


def save_original(r):
    st = r.k.state()
    r.original = {x: st[x] for x in ("set_min", "palette", "task", "project_id", "telemetry")}


def restore(r):
    k, o = r.k, r.original
    if not o:
        return
    r.idle()
    k.cmd(f"set {o['set_min']}")
    for _ in range(8):
        if k.state()["palette"] == o["palette"]:
            break
        k.send("<")
    k.cmd("task " + o["task"])
    k.cmd(f"project {o['project_id']}")
    k.cmd("telemetry " + ("on" if o["telemetry"] else "off"))
    time.sleep(2.6)  # let it save


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", default="/dev/cu.usbmodem101")
    ap.add_argument("--quick", action="store_true", help="skip slow cases and side effects")
    ap.add_argument("--only", help="comma-separated IDs, e.g. K1,T5,V2")
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
            ids = name.split("-") if "-" not in name or name[0] not in "TV" else \
                [f"{name[0]}{n}" for n in range(int(name[1]), int(name.split("-")[1][1:]) + 1)]
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
                print(f"  {'PASS' if ok else 'FAIL'}  {case:<6} {detail}  [{time.time() - t0:.1f} s]")
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
