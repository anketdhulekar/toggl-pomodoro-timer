# Knob test cases

`tools/knob_test.py` runs every case below against the real knob over USB.
Each case has an ID; the harness prints `PASS`/`FAIL` per ID and writes `test-output/report.json`.
It records your settings first (length, profile, task, project, telemetry) and restores them at the end.

Modes:
- **Default:** everything. It speaks through the Mac's speakers and creates Toggl entries named `knob-test …`.
- **`--quick`:** skips the slow cases and anything with side effects (marked ⏱ and ✎ below).
- **`--only K1,T5`:** runs just those cases. B1, B2 and B4 always run.

## Boot and health
| ID | Case | Pass when |
|----|------|-----------|
| B1 | Boot | The log shows the display, touch, haptics (DRV2605), mic and render layers coming up. |
| B2 | Connectivity | Wi-Fi connects and Toggl is ready within 30 s of boot. |
| B3 | Memory | At least 40 KB of internal RAM is free after boot. |
| B4 | No crashes | No panic, abort, assert, watchdog or stack overflow anywhere in the run's log (checked last). |
| B5 | Frame time | Every 10 s window with continuous animation (30+ frames) averages 45 ms per frame or less (22+ fps). The last-minute pulse is the heaviest, at about 36 ms. |

## Knob and timer
| ID | Case | Pass when |
|----|------|-----------|
| K1 | Set length | While reset, one knob click changes the length by ±1 minute. |
| K2 | Limits | The length can't go below 1 or above 60 minutes. |
| K3 | Locked while running | Turning the knob while running changes nothing. |
| K4 | Adjust while paused | Each click steps whole minutes and zeroes the seconds (24:58 → up → 25:00 → down → 24:00). |
| T1 | Start | A tap starts the countdown and the time goes down. |
| T2 | Pause | A tap pauses and the time stays put. |
| T3 | Resume | A tap resumes from where it paused. |
| T4 | Reset | Hold resets to the set length from running or paused; hold while reset changes nothing. |
| T5 ⏱ | Time's up | A 1-minute timer reaches 0:00, shows "time's up" with the pulse, and a tap returns to reset. |

## Gestures, buttons, look
| ID | Case | Pass when |
|----|------|-----------|
| G1 | Profiles | Swipe left/right cycles the color profile and wraps around. |
| G2 | Saved | A change is written to flash (`saved:` in the log) about 2 s later. |
| G3 | Survives restart | Length, profile, task and project are the same after a reboot. |
| G4 | Buttons show when stopped | The mic and entries badge show while reset or paused and are hidden while running; the capture shows them. |
| G5 | Hit areas and dead zone | A tap on the mic starts listening. A tap 42 px from a button does nothing. A tap in the middle starts the timer. |
| G6 | Wi-Fi glyph | With Wi-Fi up, the glyph is drawn in the rim at 6 o'clock. |

## Lists (Toggl)
| ID | Case | Pass when |
|----|------|-----------|
| P1 | Project list | Swipe down opens the list with "No project" first and at least one project; swipe up closes it. |
| P2 | Pick a project | Choosing a project selects it and the rim shows its color; "No project" clears it. |
| P3 | Auto-close | An untouched list closes by itself after about 10 s. |
| E1 | Entries list | The clock badge opens recent entries: "loading…", then entries, starting on the newest (row 1). |
| E2 | Fetched fresh | Each press fetches the list from Toggl again. |
| E3 | Pick an entry | Choosing an entry sets the task and the entry's project. |
| E4 | No task | "No task" clears the task and its project. |

## Task display
| ID | Case | Pass when |
|----|------|-----------|
| D1 | Two lines | A task with a project shows the task, with the project in small type under it. |
| D2 | Long task scrolls | A task wider than the screen moves between two captures 2.5 s apart. |

## Voice
| ID | Case | Pass when |
|----|------|-----------|
| V1 ✎ | Waveform | While listening, the waveform bars are drawn under the time. |
| V2 ✎ | Transcript | A phrase spoken by the Mac comes back with at least 60% of its words and becomes the task within 15 s (typically about 3 s; a weak Wi-Fi signal slows the upload). |
| V3 ✎ | Project by voice | Saying a project's name selects that project. |
| V4 | Nothing said | Starting and stopping the mic with nothing said ends with "didn't catch that" and changes nothing. |

## Toggl sync
| ID | Case | Pass when |
|----|------|-----------|
| S1 ✎⏱ | Short runs ignored | A run under 10 s creates no entry. |
| S2 ✎⏱ | Run is logged | A run over 10 s starts an entry (task and project) and pausing stops it. If a network hiccup blocks the start, the complete entry must still be logged later. |
| S3 ✎⏱ | Reset stops it | Reset while running stops the entry. |
| S4 ✎⏱ | Offline | A run during a Wi-Fi outage is logged once Wi-Fi is back. |

## Telemetry
| ID | Case | Pass when |
|----|------|-----------|
| M1 | Telemetry | `#telemetry on` prints `TELEMETRY {…}` about every 5 s with memory, Wi-Fi and Toggl fields; `#telemetry off` stops it. |
