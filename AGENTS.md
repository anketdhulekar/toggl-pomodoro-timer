# AGENTS.md

Guidance for coding agents working on Knob Pomodoro: firmware for a Waveshare ESP32-S3 round knob display, plus an HTML design preview. Read `README.md` for what the product does. This file covers how to change it safely.

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
   - Update `preview/knob-pomodoro.html` and check it in a browser. Playwright with system Chrome works: `npm i playwright-core`, `executablePath` = Chrome.
   - Then implement in firmware. Preview and firmware must agree on layout, sizes and behavior.
2. Build. There must be no new warnings.
3. Flash.
4. Run the harness:
   - `python tools/knob_test.py --quick` for fast feedback.
   - The full run (`python tools/knob_test.py`) before you call the work done.
   - Look at the PNG captures in `test-output/` when you touched anything visual.
5. **New behavior gets a test.**
   - Add a row to `firmware/TESTS.md` (ID, case, pass condition).
   - Add a function to `tools/knob_test.py` and register it in `CASES`.
   - If the test needs a hook (a new command or state field), add it to `serial_commands()`, `line_command()` or `state_dump()` in `main.c`.
6. Update `README.md` (user-facing behavior), this file (process or constraints), and `TESTS.md`.

## Owner's design decisions (don't undo without asking)

- **Layout and look**
  - Minimal: one timer, no sessions or breaks, no tomato theme.
  - Two hands on a fixed 60-minute dial; the slice shrinks; the dial doesn't rotate. The digits invert where the slice crosses them.
  - The face is always black with gray ticks. A selected Toggl project colors the **rim** (r ≥ 155, ticks become dark notches), not the slice.
  - The status text ("tap to start", hints) is 14 px.
- **Warm-up and time's up**
  - Fade from the profile color to Leuchtorange (#FF4D06) at 5:00, then to crimson (#E8143A) at 1:00, interpolated in OKLCH.
  - A fade must never sweep through purple (hue 285–345°) unless it starts there.
  - In the last minute the **time text pulses** (swell plus glow). No blinking, no rings.
- **Knob and gestures**
  - Locked while running. While paused, each click steps whole minutes and zeroes the seconds.
  - Hold = reset **only**. Voice was moved off hold because hold-to-talk fired when the owner meant to reset.
- **Buttons**
  - Mic at the bottom, recent-entries clock badge at 12 o'clock (a black disc over the hand). Both are 1.5× the base icon size and show only while stopped.
  - Hit radius 38 px. Dead zone 46 px: a near miss bumps and does nothing.
- **Wi-Fi glyph:** in the rim at 6 o'clock, replacing that tick; steady when connected, pulsing while reconnecting.
- **Entries list:** recent Toggl entries, newest first, fetched fresh on every press (at most 50), with "No task" on top. **No takeover** of timers started elsewhere: the owner explicitly dropped that.
- **Task display:** task above the digits, project in small type under it. A task wider than 230 px scrolls (marquee).

## Architecture

- **`main.c`:** the UI state machine.
  - State: `S` for timer, voice, task and project; `P` for the lists.
  - Input handlers: `turn`, `tap`, `hold`, `swipe`, `swipe_vertical`.
  - Each loop builds a `scene_t` and calls `render_frame` or `render_picker`.
  - Settings are saved to NVS 2 s after a change.
  - The debug interface lives here: `serial_commands`, `line_command`, `state_dump`, `list_dump`, `capture_send`, `telemetry`.
- **`render.c`:** the software renderer.
  - A static background (`build_bg`, rebuilt only when the profile, rim or Wi-Fi glyph changes) sits under the text layers (`s_text`, `s_glow`, both in PSRAM).
  - Each row is drawn in passes: coverage, then compositing, then overlays (`composite_top` for the badge, `composite_wifi`, the hold ring).
  - Only dirty rows are redrawn (`dirty_rows`). `render_capture` draws a full frame into memory for tests.
- **`cloud.c`:** one FreeRTOS task (core 1, internal-RAM stack) owns all HTTPS. The UI posts messages and polls results.
  - Speech-to-text: multipart WAV upload to `gpt-transcribe`, with Toggl project names sent as `keywords[]`.
  - Toggl 2.0: setup (workspace from `/users/me/settings`, organization from secrets), projects, entries, and the run queue `s_seg` with offline retry.
- **`mic.c`:**
  - PDM capture at 16 kHz (2.048 MHz clock).
  - Live speech tracking (`track`): noise floor, 3-chunk sustain, end after 1.3 s of quiet.
  - `mic_prepare`: 150 Hz 4th-order high-pass, trim, normalize.
- **`net.c`:** Wi-Fi station with backoff reconnects, public DNS (1.1.1.1 and 8.8.8.8), no power save, SNTP.
- **`settings.c`:** NVS keys `set_min`, `palette`, `task`, `task_id`, `proj_id`, `proj_color`, `proj_name`, `telemetry`.
- **`assets.c`** is generated by `tools/gen_assets.py`: the Manrope font at weight 600 (96 px digits, 18 and 15 px run-time fonts, 14 px status words) plus icon masks. Run it with a Python that has Pillow, after changing strings, sizes or icons.

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
  - `tracking/start` (`type: "activity"`)
  - `tracking/stop` (`end`); it stops **whatever** is running, so check `tracking/current` first
  - `time-entries` (needs both `date_from` and `date_to`; `order_by=-start`)
  - `time-entries/{id}` (PATCH)
  - `tasks/{id}/time-entries`
  - `projects?per_page=100`
- Projects marked `"active": false` (often default projects people still log to) are still in use; filter only on archived, deleted, template and draft.
- Rate limits are reportedly 30 requests/hour on the free plan, so avoid polling.

## Debug interface and telemetry (for tests and debugging only)

It's compiled in when `CONFIG_KNOB_DEBUG_INTERFACE=y` (the default; `idf.py menuconfig` → Knob Pomodoro). With it off, the knob never reads USB input and has no telemetry.

| Input | Effect |
|---|---|
| `+` `-` | turn the knob |
| `s` | start / pause (tap) |
| `r` | hold (reset) |
| `<` `>` | swipe left / right |
| `p` `u` | swipe down / up |
| `e` | entries button |
| `m` | mic button |
| `t` | tap inside a list, or stop listening |
| `d` | `STATE {json}` |
| `c` | screen capture (`CAPTURE 360 360` … base64 RGB565 … `CAPTURE END`) |
| `#tap X Y` | tap at an interface pixel (hit-area tests) |
| `#task TEXT` / `#project ID` / `#set N` | set state directly |
| `#wifi off\|on` | simulate an outage |
| `#telemetry on\|off` | `TELEMETRY {json}` every 5 s (saved in NVS) |
| `#list` | `PROJECTS [...]` and `ENTRIES [...]` |
| `#reboot` | save settings and restart |

`#` commands reply with `CMD ok …` or `CMD error …`.

**Telemetry:**
- Turn it on with `#telemetry on`, or run the harness with `--telemetry`, which saves the lines to `test-output/telemetry.jsonl`.
- Fields:
  - `uptime_s`, `mode`, `remaining_s`
  - `free_internal_kb`, `min_free_internal_kb`, `largest_internal_kb`, `free_psram_kb`
  - `wifi`, `rssi`, `clock_synced`
  - `toggl_ready`, `toggl_pending`, `toggl_backoff_s`
  - `http_last_status`, `http_last_ms`, `http_errors`
- **Turn it off when done** (`#telemetry off`). It persists across restarts.
- When off, its cost is a flag check per frame. When on, one ~350-byte line every 5 s. That's negligible, but it isn't meant for everyday use.

The per-request timing line (`connect … send … reply …`) and the 10-second frame stats are always logged. They're cheap and are the first thing to check for slowness.

## Testing

- The cases are in `firmware/TESTS.md` (IDs B1–M1). The harness is `firmware/tools/knob_test.py`.
- The full run speaks through the Mac's speakers and **creates Toggl entries named `knob-test …`. The owner has approved this.**
- `--quick` skips slow cases and anything with side effects.
- The harness records the owner's length, profile, task, project and telemetry setting, and restores them at the end. Keep it that way for new tests.
- **Flaky-looking failures are usually environmental:**
  - a weak Wi-Fi signal (check `rssi`)
  - people talking near the mic (it keeps listening)
  - the Toggl rate limit

  Read `test-output/serial.log` before changing code.

## Commits

- Small, focused commits with a body that says why.
- Never commit `secrets.h`, `build/`, `managed_components/`, `test-output/` or the factory backup (`.gitignore` covers these).
- Check `git status` and `git diff --cached` before every commit.
