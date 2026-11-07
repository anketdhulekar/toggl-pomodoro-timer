# AGENTS.md

Guidance for coding agents working on Knob Timer (formerly Knob Pomodoro): firmware for a Waveshare ESP32-S3 round knob display that tracks time in Toggl, plus an HTML design preview. Read `README.md` for what the product does. This file covers how to change it safely.

## Hard rules

1. **Never read, print, copy, or commit `firmware/main/secrets.h`.**
   - It holds the owner's Wi-Fi password and API keys.
   - Don't `cat` or `grep` its contents, and don't paste values into chat or logs.
   - The build uses it automatically.
   - If a value must change, ask the owner to edit it. The only exception is an explicit request, such as appending a non-secret line: append without reading.
   - `secrets.example.h` is the committed template, with all values empty.
2. **Never log secrets.** Keys are only used to build `Authorization` headers in `cloud.c`. The key clean-up logs byte codes of stripped characters, never key content.
3. **Don't flash anything but this firmware** without the owner's go-ahead. Restoring `backup/factory-firmware-esp32s3-16MB.bin` erases all settings.
4. **Mask the Wi-Fi name** in logs you show (`sed -E 's/joining Wi-Fi ".*"/joining Wi-Fi "<ssid>"/'`). The harness does this in `test-output/serial.log`.
5. **Transcripts are user data.** The mic can pick up conversations in the room. Don't quote transcripts from logs back to the owner beyond what a test needs.

## Environment

- macOS. ESP-IDF v5.5.6 lives in `~/esp/esp-idf`.
- Load it with `. ./idf-env.sh` from the repo root. It pins `IDF_PYTHON_ENV_PATH` to the Python 3.13 env, because the system `python3` is too new and non-interactive shells pick up 3.9.
- Build and flash, from `firmware/`:
  - `idf.py build`
  - `idf.py -p /dev/cu.usbmodem101 flash`
- **USB plug orientation matters.** One orientation reaches the ESP32-S3 (`/dev/cu.usbmodem101`, Espressif USB-Serial/JTAG). The other reaches a second ESP32 through a CH340 (`/dev/cu.usbserial-*`). If the port is missing, ask the owner to flip the plug at the knob end.
- **Opening the serial port restarts the board.** The harness relies on this for a clean boot. Time any commands for after boot: Toggl is ready about 5 s in.
- **After adding a Kconfig option** (`main/Kconfig.projbuild`), run `idf.py reconfigure`, or the incremental build won't see it.
- **`sdkconfig` is committed**, and every deliberate choice is also in `sdkconfig.defaults`. Keep the two in sync: edit the defaults, and either delete `sdkconfig` or edit both.

## Workflow for any change

1. **Design changes start in the preview.**
   - Update `preview/toggl-pomodoro-timer.html` and check it in a browser. Playwright with system Chrome works: `npm i playwright-core`, `executablePath` = Chrome.
   - Then implement in firmware. Preview and firmware must agree on layout, sizes and behavior.
2. Build. There must be no new warnings.
3. Flash.
4. Run the harness:
   - `python tools/knob_test.py --quick` for fast feedback.
   - The full run (`python tools/knob_test.py`) before you call the work done.
   - Look at the PNG captures in `test-output/` when you touched anything visual.
5. **New behavior gets a test.**
   - Add a row to `docs/TESTS.md` (ID, scenario, pass condition).
   - Add a function to `tools/knob_test.py` and register it in `CASES`.
   - If the test needs a hook (a new command or state field), add it to `serial_commands()`, `line_command()` or `state_dump()` in `main.c`.
6. Update `README.md` and `docs/USER-GUIDE.md` (user-facing behavior), this file (process or constraints), and `docs/TESTS.md`. Screenshots live in `docs/assets/`.

## Owner's design decisions (don't undo without asking)

Proposed changes that the owner has not decided yet are in `TODO.md`. Don't start an item there that changes a decision below without asking.

- **Two modes**
  - **Tracking** (primary, the default) counts up. Pause keeps the session total; each running stretch is its own Toggl entry. Hold resets to 00:00.
  - **Pomodoro** (secondary) counts down from 1–60 min, always as MM:SS (a 60-minute timer shows 60:00). Hold resets to the set length.
  - Swipe up switches modes, never while running ("pause to switch"), and not while a swipe-down sync is in flight.
- **Layout** (symmetric about y 180; the preview is the reference)
  - Project pill at the top (y 50–78), filled with the Toggl color; text 15 px bold, dark or light by OKLab L > 0.66; cut with an ellipsis at 120 px. No project: an outlined "No project" pill.
  - Task under it, one line, 28 px, baseline 123. Wider than 250 px: it scrolls (rest 2 s, 30 px/s, gap 48, faded edges in x 55–305). No task: "Untitled" in the secondary color.
  - Time 88 px (MM:SS) at baseline 212; from an hour on HH:MM:SS at 62 px. Tabular figures. Status 14 px at baseline 238.
  - The mic (1.5×) at (180, 282) is the only button, shown in both modes, running or not.
  - No clock hands, no slice, no clock badge, no profile dots.
- **Rim** (band r 155–180)
  - Tracking: a full ring in the project color (no project: the profile's ink color).
  - Pomodoro: an arc from 12 o'clock clockwise, remaining / 60 min, over a faint full ring (16%) on a black band. It always looks as in Mono, whatever the profile (no project: a white arc).
  - Ticks under the band become dark notches. The Wi-Fi glyph replaces the 6 o'clock tick, centered in the band; steady when connected, pulsing while reconnecting.
- **Color profiles:** only Mono (white on black) and Inverted (black on white). Swipe left/right switches, any time, even while running.
- **Warm-up and time's up (Pomodoro)**
  - Fade from the ink color to Leuchtorange (#FF4D06) at 5:00, then to crimson (#E8143A) at 1:00, interpolated in OKLCH. A fade must never sweep through purple (hue 285–345°) unless it starts there.
  - In the last minute the **time pulses** (swell plus glow). No blinking, no rings.
  - Time's up vibrates continuously at full intensity for **30 s** (DRV2605 real-time playback at full scale, `haptics_buzz`); any touch or knob turn stops it early. The owner couldn't feel the old single burst. Other feedback (a sync result, say) doesn't interrupt it: `haptics_play` is skipped while it buzzes. Don't raise the driver's voltage clamp without asking: 30 s continuous could overheat the motor.
  - Knob detents play a soft tick (library effect 26, "Sharp Tick 3", 60 %), at most one per 40 ms (`CLICK_GAP_US`). On battery, a full-strength tick per detent dipped the supply and made the backlight flicker during a turn; on USB power it didn't. Don't make the detent tick stronger without checking this on battery.
  - Time's up closes an open list, so the face shows "time's up" and the silencing tap can't pick a row.
- **Knob**
  - Tracking, running: clockwise adds a minute (the start moves a minute earlier), anticlockwise takes one off, never past the start of the running stretch. **No hint text.** Not running: a bump.
  - Pomodoro: sets the length while reset; while paused, each click goes to the neighbouring whole minute (24:58 → 25:00 or 24:00); locked while running.
- **Gestures:** hold = reset **only** (voice moved off hold because it fired when the owner meant to reset). Tap the task: past tasks list. Tap the pill: projects list. Swipe down: Tracking only, sync with Toggl.
- **Mic hit area:** radius 34 px. The 44 px dead zone applies only below the status line (y > 243): a near miss bumps and does nothing.
- **Lists**
  - Past tasks: "Untitled" on top, then recent entries deduped by (description, project), newest first, fetched fresh on every open (at most 50, last 30 days), opening on row 1. Picking one while tracking switches entries (the counter keeps going); in a running Pomodoro it renames the entry. Picking the task that's already set changes nothing (no split entry). While the list is loading or failed, a tap only closes it ("Untitled" is a placeholder there).
  - Projects: sorted by name, "No project" last, opening on the current project ("No project" if it isn't listed). A tap only chooses after a turn of the knob, so a reflex tap never clears the project; re-picking the current project changes nothing. A reload while open keeps the highlighted project. "Work" is the default when no project was ever chosen.
  - Lists use the profile's face and ink colors.
- **Toggl**
  - Entries start in Toggl immediately. A stretch stopped within 10 s is **deleted** (accidental-tap guard).
  - Toggl is contacted **only on user actions** (start, pause, change, opening a list, swipe down). No polling: the free plan's rate limit is small.
  - **Syncing is manual, and it's the user's job.** The knob never checks Toggl by itself: no resume after a restart, no check at boot, no background reconciliation. After a restart it comes back reset even if its entry still runs in Toggl; the user pauses before unplugging or swipes down before starting. Don't add automatic syncing without asking.
  - Swipe down (Tracking): a timer started elsewhere is adopted (task, project, start); the knob's own timer stopped elsewhere, or running with no Toggl entry at all → paused ("stopped in Toggl"); otherwise "in sync".
  - A start, change or stop Toggl turns down for good shows "not saved in Toggl".

## Architecture

- **`main.c`:** the UI state machine.
  - State: `S` holds the mode (`app`), profile, Tracking (`t_state`, `t_session_us`, `t_start_us`), Pomodoro (`p_state`, `set_min`, `remaining_us`), the alarm, voice, task and project. `P` holds the lists.
  - Input handlers: `turn`, `tap` (hit tests: mic, task box, pill box, dead zone), `hold`, `swipe`, `swipe_up`, `swipe_down`; per mode `track_*` and `pomo_*`.
  - Each loop builds a `scene_t` and calls `render_frame`, or `render_picker` while a list is open.
  - Settings are saved to NVS 2 s after a change.
  - The debug interface lives here: `serial_commands`, `line_command`, `state_dump`, `list_dump`, `capture_send`, `telemetry`.
- **`render.c`:** the software renderer.
  - Two static backgrounds (`s_bg_off`, `s_bg_on`) plus a tick map, rebuilt (`build_bg`, ~110 ms) only when the profile, mode, project or Wi-Fi glyph changes. They hold the face, the pill and the rim with the band off and on; `rim_span` picks per pixel by angle (the Pomodoro arc), switching whole ticks at once.
  - One text layer (`s_text`) and the pulse glow (`s_glow`), both in PSRAM. The text color depends on the row: task rows use the task color, rows from y 140 the live color.
  - Only dirty 24-row bands are redrawn (`dirty_bands`, a bitmask, so changes at the top and bottom don't redraw the middle). `render_capture` draws a full frame into memory for tests.
- **`cloud.c`:** one FreeRTOS task (core 1, internal-RAM stack) owns all HTTPS. The UI posts messages and polls results.
  - Speech-to-text: multipart WAV upload to `gpt-transcribe`, with Toggl project names sent as `keywords[]`.
  - Toggl 2.0: setup (workspace from `/users/me/settings`, organization from secrets), projects, entries, the run queue `s_seg` with offline retry, the swipe-down check (`toggl_check`), and debug "elsewhere" start/stop.
  - Changes to a running entry (knob, rename, project) go out as one `PATCH` once they settle (1.5 s), with only the fields that changed: a knob turn sends just `start`, never the knob's 95-byte copy of the description.
  - The message queue (32 slots) lives in PSRAM. Knob clicks are held on the UI side (`cloud_session_shift`) and posted as one message once the knob rests 250 ms (`cloud_tick`), or before any other message, so a fast spin can't fill the queue. `post()` reports a full queue, and the UI falls back ("transcription failed", "couldn't reach Toggl").
  - Text from Toggl is cut with `ucopy`, which never splits a UTF-8 character.
- **`mic.c`:**
  - PDM capture at 16 kHz (2.048 MHz clock).
  - Live speech tracking (`track`): noise floor, 3-chunk sustain, end after 1.3 s of quiet.
  - `mic_prepare`: 150 Hz 4th-order high-pass, trim, normalize.
- **`net.c`:** Wi-Fi station with backoff reconnects, public DNS (1.1.1.1 and 8.8.8.8), no power save, SNTP.
- **`settings.c`:** NVS keys `app`, `theme`, `set_min`, `task`, `task_id`, `proj_id`, `proj_color`, `proj_name`, `telemetry`. A missing `proj_id` means "never chosen" (the Work default applies).
- **`haptics.c`:** DRV2605 library effects (`haptics_play`) and continuous full-scale vibration in real-time playback mode (`haptics_buzz`, time's up).
- **`assets.c`** is generated by `tools/gen_assets.py` from Manrope: time 88 and 62 px (weight 600, tabular `.tf` digits via fontTools) plus a blurred 88 px glow set, the 28 px task font, 18 px list font, 15 px bold pill font, 14 px status words, and the mic and Wi-Fi masks. Run it with a Python that has Pillow and fontTools after changing strings, sizes or icons.

## Hard-won constraints

- **Internal RAM is scarce (~85–95 KB free).**
  - Big buffers go in PSRAM. Wi-Fi, TLS and the display DMA need internal RAM, and below ~20 KB free, uploads slowed or failed.
  - Keep the `cloud` task stack in **internal** RAM: with a PSRAM stack, TLS handshakes were about 10× slower.
- **TLS uses software AES** (`CONFIG_MBEDTLS_HARDWARE_AES=n`). Hardware AES needs DMA-capable internal buffers per record and failed mid-upload (`esp-aes: Failed to allocate memory`).
- **Networking**
  - The router's DNS took about 7 s per first lookup, so the knob uses public DNS and IPv4 only (`addr_type`).
  - TCP windows are 32 KB (PSRAM pbufs), and Wi-Fi power save is off. Without these, uploads crawled at about 25 KB/s.
  - Brief connection drops happen. `https()` retries connections that couldn't be opened (nothing was sent), and transcription retries once.
- **Rendering performance**
  - Xtensa doesn't inline libm (`sqrtf`, `floorf`, …) and float division is in software. Keep them out of per-pixel inner loops.
  - Split big per-row work into small passes; many live floats spill registers.
  - Frame budget: continuous animation ≤ 45 ms per frame (harness case B5). The last-minute pulse is the heaviest, at about 36 ms.
- **Console:** the primary console is UART at 115200, mirrored to USB-Serial/JTAG. Anything large, like screen captures, must go straight to USB (`usb_puts`) with logging paused, or it trips the task watchdog. Print machine-read lines with `emit()` (one `write`), so other tasks' logs can't split them.

## Toggl 2.0 API notes

- Base URL `https://focus.toggl.com/api`, header `Authorization: Bearer toggl_sk_…`. The classic Track v9 API rejects these keys.
- Spec: https://engineering.toggl.com/docs/focus/openapi (Swagger JSON). It isn't fully accurate: `/workspaces/{ws}/context` needs a browser session.
- Endpoints under `/organizations/{org}/workspaces/{ws}/`:
  - `tracking/start` (`type: "activity"`). If it's turned down (400/409/422), the knob checks `tracking/current`: only if a timer runs does it stop that one and retry. Otherwise the refusal is about the entry (an archived project, say) and nothing else is touched.
  - `tracking/stop` (`end`); it stops **whatever** is running, so check `tracking/current` first
  - `time-entries` (needs both `date_from` and `date_to`; `order_by=-start`)
  - `time-entries/{id}` (PATCH)
  - `tasks/{id}/time-entries`
  - `projects?per_page=100`
- Projects marked `"active": false` (often default projects people still log to) are still in use; filter only on archived, deleted, template and draft.
- Rate limits are reportedly 30 requests/hour on the free plan, so avoid polling. After a 429, swipe down and the lists answer at once without a request until the backoff ends.
- Timers adopted from another device are never deleted by the 10 s rule (that's for the knob's own accidental starts).

## Debug interface and telemetry (for tests and debugging only)

It's compiled in when `CONFIG_KNOB_DEBUG_INTERFACE=y` (the default; `idf.py menuconfig` → Knob Pomodoro). With it off, the knob never reads USB input and has no telemetry.

| Input | Effect |
|---|---|
| `s` | tap: start / pause / resume |
| `r` | hold (reset) |
| `+` `-` | turn the knob (clockwise / anticlockwise) |
| `e` | tap the task (past tasks list) |
| `o` | tap the pill (projects list) |
| `m` | mic button |
| `t` | tap inside a list, or stop listening |
| `u` `p` | swipe up (mode) / down (Toggl sync) |
| `<` `>` | swipe left / right (profile) |
| `d` | `STATE {json}` |
| `c` | screen capture (`CAPTURE 360 360` … base64 RGB565 … `CAPTURE END`) |
| `#tap X Y` | tap at an interface pixel (hit-area tests) |
| `#task TEXT` / `#task_id N` / `#project ID [#RRGGBB NAME]` / `#set N` | set state directly (`#task` clears the Toggl task link; `#task_id` sets it) |
| `#app track\|pomo` / `#theme mono\|inverted` | switch mode (not while running) / profile |
| `#session N` | Tracking total in seconds, paused (Toggl isn't told) |
| `#remaining N` | Pomodoro time left in seconds, paused |
| `#toggl start TEXT` / `#toggl stop` | start or stop a Toggl timer directly, as another device would |
| `#wifi off\|on` | simulate an outage |
| `#telemetry on\|off` | `TELEMETRY {json}` every 5 s (saved in NVS) |
| `#list` | `PROJECTS [...]` and `ENTRIES [...]` |
| `#reboot` | save settings and restart |

`#` commands reply with `CMD ok …` or `CMD error …`.

**Telemetry:**
- Turn it on with `#telemetry on`, or run the harness with `--telemetry`, which saves the lines to `test-output/telemetry.jsonl`.
- Fields:
  - `uptime_s`, `app`, `mode`, `elapsed_s`, `remaining_s`
  - `free_internal_kb`, `min_free_internal_kb`, `largest_internal_kb`, `free_psram_kb`
  - `wifi`, `rssi`, `clock_synced`
  - `toggl_ready`, `toggl_pending`, `toggl_backoff_s`
  - `http_last_status`, `http_last_ms`, `http_errors`
- **Turn it off when done** (`#telemetry off`). It persists across restarts.
- When off, its cost is a flag check per frame. When on, one ~350-byte line every 5 s. That's negligible, but it isn't meant for everyday use.

The per-request timing line (`connect … send … reply …`) and the 10-second frame stats are always logged. They're cheap and are the first thing to check for slowness.

## Testing

- The cases are in `docs/TESTS.md` (IDs B, D, L, G, M, R, P, H, V, S, X), each with its steps and pass condition. The harness is `firmware/tools/knob_test.py`.
- Timer cases that aren't about Toggl (M, R, P, H, and L8) run with `#wifi off` and keep every stretch under 10 s, so they never reach Toggl. Keep new local timer tests in that block.
- The full run speaks through the Mac's speakers and **creates Toggl entries named `knob-test …`. The owner has approved this.**
- `--quick` skips slow cases and anything with side effects (a few minutes).
- The harness records the owner's mode, profile, length, task, the task's Toggl task link (`task_id`), project (with its color and name: it may not be in the active list) and telemetry setting, and restores them at the end. Keep it that way for new tests.
- **Flaky-looking failures are usually environmental:**
  - a weak Wi-Fi signal (check `rssi`)
  - people talking near the mic (it keeps listening)
  - the Toggl rate limit

  Read `test-output/serial.log` before changing code.

## Commits

- Small, focused commits with a body that says why.
- Never commit `secrets.h`, `build/`, `managed_components/`, `test-output/` or the factory backup (`.gitignore` covers these).
- Check `git status` and `git diff --cached` before every commit.
