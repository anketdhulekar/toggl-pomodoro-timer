# Tests

`firmware/tools/knob_test.py` tests the real knob end to end over USB. It drives the knob the way a person would: taps, holds, knob turns, swipes and speech. Then it checks the knob's state, its log and pixels in screen captures. This page describes every scenario it runs and what counts as a pass.

## Running it

From `firmware/`, with the ESP-IDF environment loaded (`. ../idf-env.sh`):

```sh
python tools/knob_test.py                 # everything (a few minutes)
python tools/knob_test.py --quick         # skips the slow cases and those with side effects
python tools/knob_test.py --only R4,S5    # just these cases (B1, B2 and B4 always run)
python tools/knob_test.py --telemetry     # also saves TELEMETRY lines to test-output/telemetry.jsonl
```

The harness prints `PASS` or `FAIL` for each case, with a short detail line, and exits non-zero if anything failed.

## How a run works

1. **Fresh boot.** Opening the serial port restarts the knob. The harness waits up to 30 s for Wi-Fi, Toggl and the project list.
2. **Your settings are recorded.** It saves the mode, profile, Pomodoro length, task, the task's Toggl task link, project (with its color and name, since it may not be in Toggl's active list) and the telemetry setting. It also reads your projects and recent entries (`#list`).
3. **The cases run in the order of this page,** with three exceptions: the offline block runs between the lists and voice, and G2, B5 and B4 run last. Timer cases that aren't about Toggl (M, R, P, H and L8) run with Wi-Fi switched off (`#wifi off`) and keep every running stretch under 10 s, so they never reach Toggl. The one exception is P5's 1-minute run, which is logged once Wi-Fi is back.
4. **Your settings are restored,** even after a failure or Ctrl-C.
5. **Output goes to `firmware/test-output/`** (git-ignored):
   - `report.json`: every result.
   - `serial.log`: the whole log, with the Wi-Fi name masked.
   - The PNG captures named in the detail lines.
   - `telemetry.jsonl`, with `--telemetry`.

**How it talks to the knob.** It uses the firmware's debug interface (`CONFIG_KNOB_DEBUG_INTERFACE`), listed in [AGENTS.md](../AGENTS.md#debug-interface-and-telemetry-for-tests-and-debugging-only):
- Single keys stand in for the gestures: `s` tap, `r` hold, `+` and `-` knob, `e` tap the task, `o` tap the pill, `m` mic, `t` tap in a list, `u` and `p` swipe up and down, `<` and `>` swipe left and right.
- `#` commands set state directly, such as `#task`, `#project`, `#set`, `#remaining`, `#session` and `#wifi off`.
- `d` prints the state as JSON, and `c` sends a screen capture.

**Markers:**
- ⏱ **slow**, and ✎ **side effects** (it speaks through the Mac's speakers or creates Toggl entries named `knob-test …`). `--quick` skips both.
- **Creating `knob-test` entries in your Toggl workspace is expected.** Delete them whenever you like.

## Boot and health

| ID | Scenario | Pass when |
|----|----------|-----------|
| B1 | **Boot.** Reads the boot log. | The display, touch controller, haptics driver (DRV2605), microphone and render layers all report ready. |
| B2 | **Connectivity.** Waits up to 30 s after boot. | Wi-Fi is connected and Toggl is ready. |
| B3 | **Memory.** Reads the free internal RAM from the state after boot. | At least 40 KB is free. |
| B4 | **No crashes** (runs last). Scans the log of the whole run. | No panic, abort, assert, stack overflow or watchdog reset. |
| B5 | **Frame time** (runs near the end). Reads the "N frames in 10 s" lines logged during the run, counting only windows with continuous animation (30+ frames). | Every such window averages 45 ms per frame or less (22+ fps). |

## Display

| ID | Scenario | Pass when |
|----|----------|-----------|
| D1 | **Layout.** Tracking, Mono, reset. Picks your first project and the task "knob-test layout" and captures the screen. Then clears the task and captures again. | The pill row is filled with the project's color, the task and time are drawn in white, and the rim is in the project's color. With no task, "Untitled" is drawn in gray, with no white pixels in the task line. |
| D2 | **Long task scrolls.** Sets a task far wider than the 250 px window, waits out the 2 s rest, and captures twice, 2.5 s apart. | Over 200 pixels of the task line changed between the captures, and the window's left edge is faded to dark. |
| D3 | **Inverted.** Switches to Inverted and captures. | The face is white and the time has 1,500+ dark pixels. |
| D4 | **Wi-Fi glyph.** Captures with Wi-Fi up. | Pixels at 6 o'clock in the rim differ from the band, so the glyph is drawn. |

## Lists (online, read-only)

| ID | Scenario | Pass when |
|----|----------|-----------|
| L1 | **Projects list.** Taps the pill, captures, swipes up. Opens it again and taps without turning the knob. Then sets a project that isn't in the list (`#project 999999 …`), opens the list and taps. Restores the project. | The list opens on the current project with "No project" last, and swipe up closes it. A tap without a turn keeps the project. For a project that isn't in the list, the list opens on "No project" and a tap still keeps the project. |
| L2 | **Pick a project.** Turns to a different project and taps, captures. Then picks "No project" and captures. | The project is selected and the Tracking rim takes its color. "No project" clears it, and the rim turns white (Mono). |
| L3 | **Auto-close.** Opens the projects list and leaves it alone. | It closes by itself within 14 s (it closes after 10 s without input). |
| L4 | **Past tasks.** Taps the task, reads the state at once and again after loading, and captures. | It shows "loading…" first, then your entries, opening on row 1 (the newest). |
| L5 | **Fetched fresh.** Opens and closes past tasks twice. | Two fetches from Toggl are logged. |
| L6 | **Pick a task.** Picks row 1. | The task is the newest entry's name, and the project is that entry's project. |
| L7 | **Untitled.** Sets the task "knob-test L7" and picks row 0. | The task is cleared and the project is unchanged. |
| L8 | **A list that couldn't load** (in the offline block). With Wi-Fi off, sets the task "knob-test L8 kept", taps the task, waits for "couldn't reach Toggl", then taps. | The list closes, the task is unchanged and the timer didn't start: the "Untitled" placeholder isn't chosen. |

## Settings

| ID | Scenario | Pass when |
|----|----------|-----------|
| G1 | **Saved.** Swipes to the other profile and back. | A save to flash is logged within 5 s (settings are written 2 s after the last change). |
| G2 | **Survives restart** (runs near the end). Sets Pomodoro, 17 min, Inverted, the task "knob-test G2 survives restart" and your first project. Waits for the save, then restarts the knob (`#reboot`). | After the restart, the mode, profile, length, task and project are the same. |

## Modes and profiles (offline)

| ID | Scenario | Pass when |
|----|----------|-----------|
| M1 | **Switch modes.** From Tracking, reset, swipes up twice. | It switches to Pomodoro (hint "pomodoro"), then back to Tracking (hint "tracking"). |
| M2 | **Not while running.** Starts Tracking and swipes up. | It stays in Tracking, still running, with the hint "pause to switch". |
| M3 | **Profile any time.** Starts Tracking, swipes right, then left. | It switches to Inverted (hint "inverted") while the timer keeps running, then back to Mono. |

## Tracking (offline)

| ID | Scenario | Pass when |
|----|----------|-----------|
| R1 | **Count up.** Taps and waits 2.3 s. | It's running at 00:02, with no status text. |
| R2 | **Pause and resume.** Taps (pause), waits 1.5 s, taps (resume), waits 1.3 s. | It shows "paused" and the total stays put while paused. After resuming, it counts on from that total. |
| R3 | **Reset.** Holds while running. Starts again, pauses and holds. Holds once more while reset. | Every hold leaves it reset at 00:00, and the hold while reset changes nothing. |
| R4 | **Knob adjusts.** Turns the knob while reset. Starts the timer, then turns 3 clicks clockwise, 2 anticlockwise and 2 more anticlockwise. Pauses and turns again. | Reset: no change. Running: +3 clicks add 3 minutes, −2 take 2 off, and the last −2 stop at the start of the running stretch. Paused: no change. |
| R5 | **Hours.** Sets the total to 1:02:05 (`#session 3725`) and captures. Then sets it to 59 s. | It shows "01:02:05" in the smaller digits (wider than MM:SS), and "00:59" under an hour. |

## Pomodoro (offline)

| ID | Scenario | Pass when |
|----|----------|-----------|
| P1 | **Set length.** While reset: one click up, one down. Then sets 1 min and clicks down, and sets 60 min and clicks up. | Each click changes the length by 1 minute. It stops at 1 ("1 min minimum") and 60 ("60 min maximum"), and 60 minutes shows as 60:00. |
| P2 | **Locked while running.** Sets 25 min, starts, and turns 3 clicks. | The length stays 25, the time keeps counting down, and the hint says "pause to adjust". |
| P3 | **Adjust while paused.** Sets 25 min, runs 2.2 s and pauses (24:58). Clicks down. Sets 24:58 again (`#remaining`), clicks up, then down. | 24:58 goes down to 24:00. 24:58 goes up to 25:00, then down to 24:00. |
| P4 | **Start, pause, reset.** Sets 25 min. Taps, waits 2.2 s. Taps, waits 1.5 s. Taps, waits 1.3 s. Then holds twice. | It counts down, stays put while paused, counts on after resuming, and resets to 25:00. The second hold changes nothing. |
| P5 ⏱ | **Time's up.** Sets 1 min and the task "knob-test P5 times up", taps, and waits for 00:00. Captures, waits for the vibration to end, then taps. | At 00:00 the time pulses and the status says "time's up". The vibration lasts 29.5–30.6 s (measured from log timestamps) and stops by itself. The tap returns to the set length. |
| P6 | **Rim.** Sets 15 min with a project, captures in Mono and in Inverted. Then sets 25 min and captures. | At 15 min, the arc reaches 3 o'clock in the project's color over a faint ring, and the rim is identical in both profiles. At 25 min, the arc is filled all the way from 12 o'clock to 5 o'clock, and dark beyond. |
| P7 | **Silence early.** Sets 2 s left (`#remaining 2`) and taps. When it vibrates, turns the knob, then taps. | The turn stops the vibration and "time's up" stays on screen. The tap resets. |
| P8 | **Time's up with a list open.** Sets 3 s left, taps, and opens the projects list. Waits for time's up, waits 4 s more, then taps. | The list closes at time's up and "time's up" shows. It's still vibrating 4 s later. The tap resets without choosing a project. |

## Hit areas (offline)

| ID | Scenario | Pass when |
|----|----------|-----------|
| H1 | **Taps.** Taps (`#tap X Y`) on the mic, 40 px to the right of it, on the task, on the pill and on the status line. | The mic is pressed: offline, it answers "no Wi-Fi". The near miss does nothing. The task opens past tasks, the pill opens projects, and the status line starts the timer like any other tap. |

## Voice (online)

| ID | Scenario | Pass when |
|----|----------|-----------|
| V1 ✎ | **Waveform.** Taps the mic, the Mac says a phrase (`say`), and the harness captures 1.6 s in. | The waveform bars are lit (100+ pixels) where the status line was. |
| V2 ✎ | **Transcript.** Continues from V1 and waits for the transcript. | It contains at least 60% of the phrase's words, becomes the task, and took at most 15 s. |
| V3 ✎ | **Project by voice.** Clears the project. The Mac says "\<project\> session" for one of your projects with a distinctive, speakable name. | That project is selected. |
| V4 | **Nothing said.** Taps the mic and taps again 0.7 s later. | It says "didn't catch that" and the task is unchanged. |

## Toggl sync (online, creates `knob-test …` entries)

| ID | Scenario | Pass when |
|----|----------|-----------|
| S1 ✎⏱ | **Start right away.** Sets the task "knob-test S1 tracked", taps, waits 11 s and taps again. | A Toggl entry started within 8 s of the tap, and it's the knob's entry. Pausing stopped it. |
| S2 ✎ | **Accidental start.** Sets the task "knob-test S2 accidental", taps, and taps again as soon as the entry has started. | The entry is deleted from Toggl. |
| S3 ✎⏱ | **Fast knob.** Sets the task "knob-test S3 adjusted", taps, and right away sends 12 fast clockwise clicks while the start request is still on its way. | The knob shows 12 minutes. Toggl got one update that moved the entry's start 12 minutes earlier and changed nothing else. Pausing stopped it. |
| S4 ✎⏱ | **Switch task.** Sets the task "knob-test S4 first", taps, and waits 11 s. Picks an earlier knob-test entry from past tasks, then picks the same one again. | The first entry stopped and a new one started for the picked task, while the counter kept going. Picking the same task again changed nothing in Toggl: no new entry. |
| S5 ✎ | **Sync.** Starts "knob-test S5 elsewhere" as another device would (`#toggl start`), then swipes down. Turns the knob one click. Stops the timer elsewhere (`#toggl stop`) and swipes down. Swipes down once more. Finally starts "knob-test S5 adopted" elsewhere, swipes down and pauses at once. | The first swipe adopts the timer: its task, running, "synced from Toggl". The click sends only the new start, never the description. The next swipe pauses the knob ("stopped in Toggl"), and the one after says "in sync". The timer adopted and then paused within seconds is stopped in Toggl, not deleted. |
| S6 ✎⏱ | **Offline.** Sets the task "knob-test S6 offline run", switches Wi-Fi off, runs 12 s, then switches Wi-Fi back on. | Within 40 s of reconnecting, the stretch is logged in Toggl with its start and stop times. |
| S7 ✎⏱ | **Pomodoro entry.** Sets 25 min and the task "knob-test S7 pomodoro", taps, waits 11 s and holds. | An entry started right away, and the reset stopped it. |

## Telemetry

| ID | Scenario | Pass when |
|----|----------|-----------|
| X1 | **Telemetry.** Turns telemetry on (`#telemetry on`), waits for two lines, turns it off and waits 7 s. | A line arrives about every 5 s with memory, Wi-Fi and Toggl fields, and none after it's turned off. |

## Not covered

These are checked by reading the code, not by the harness. Most need a way to fake Toggl's replies:
- Toggl turning down a start or a change for good ("not saved in Toggl"), and swipe down then pausing a timer that has no entry.
- Toggl's rate limit (HTTP 429), the backoff, and swipe down and lists not asking during it.
- The warm-up colors at 4:59 and 0:59, and how the pulse looks (P5 checks only that it's on).
- The Wi-Fi glyph pulsing while reconnecting (D4 checks only the steady glyph).
- Text cut at a multi-byte character, and accents and emoji on screen.
- The "Work" default when no project was ever chosen.
- A projects list reloading while it's open.

## Adding a test

1. Add a row here: ID, scenario, pass condition.
2. Add a function to `firmware/tools/knob_test.py` and register it in `CASES`.
3. If the test needs a new hook (a command or a state field), add it to `serial_commands()`, `line_command()` or `state_dump()` in `firmware/main/main.c`.
4. Put timer tests that don't need Toggl in the offline block, and keep their running stretches under 10 s.
