# TODO

These UX problems come from a review of the controls on 2026-11-01. None of them is fixed yet.

- Items in the first section do not change the owner's design decisions.
- Items in the second section change a design decision in [AGENTS.md](AGENTS.md#owners-design-decisions-dont-undo-without-asking). Get the owner's approval before you start them.
- Each item needs the usual workflow: change the preview first, then the firmware, then the tests and the docs.

## Small fixes

- [ ] **Use the same rule to choose a row in both lists.**
  - In the projects list, a tap chooses a row only after you turn the knob. In the past-tasks list, a tap chooses the highlighted row immediately.
  - The past-tasks list opens on the newest finished entry. This entry is usually not the current task, because the running entry is not in the list.
  - As a result, a tap to close the list can change the task and split the Toggl entry.
  - Fix: use the turn-then-tap rule in both lists. Show "turn to choose" until the knob turns (see the next item).
  - Where: `picker_close()` in `firmware/main/main.c`. In the preview, `closePicker()`. Also, the preview's past-tasks list includes the running entry, but the firmware's list does not. Make the preview match the firmware.
- [ ] **Show the correct hint in the projects list.**
  - The list shows "tap to choose" when it opens, but a tap without a turn closes the list.
  - Fix: show "turn to choose" until the knob turns, then "tap to choose". This needs a new status string in `firmware/tools/gen_assets.py`.
- [ ] **Stop accidental taps near the pill and the task.**
  - The mic has a dead zone, but the pill and the task do not have one.
  - A tap in the gap between the pill and the task (y 86–92), or just above the pill, starts or stops the timer.
  - For a short project name, the pill target is approximately 11 mm × 5 mm, which is smaller than a fingertip.
  - Each accidental stop and start splits the Toggl entry.
  - Fix: give the pill a larger target of a fixed size (for example x 90–270, y 34–90). Start the task box at y 88. Add a dead band around both targets that gives a bump and does nothing else.
  - Where: `tap()` and `in_pill()` in `firmware/main/main.c`, `tapAt()` in the preview.
- [ ] **Do not let a knob turn cause a hold.**
  - A hold of 0.7 s resets the timer and clears the task and the project, from all positions on the screen.
  - If a finger touches the glass while the knob turns, the press does not move. After 0.7 s it becomes a hold, resets the timer and clears the task and the project. This is not confirmed on the device.
  - Fix: cancel the press when a knob step arrives during the press. Ignore presses that start less than approximately 300 ms after a knob step.
  - Where: `input.c` and the main loop in `firmware/main/main.c`.
- [ ] **Give feedback for gestures that do nothing.**
  - A swipe down in Pomodoro mode does nothing and gives no feedback.
  - A swipe left or right in a list does nothing and gives no feedback.
  - A hold while the knob listens fills the hold ring, but nothing occurs.
  - Fix: give a bump for each of these gestures. For a swipe down in Pomodoro mode, also show "tracking only". Do not draw the hold ring while the knob listens or while a list is open.
- [ ] **Add a "Quick start" section at the top of the user guide.**
  - The guide tells the user to pause before they unplug the knob, but this information is near the end.
  - The section must include: tap to start or pause, hold to reset, tap the task or the pill to choose, the mic, swipe up for Pomodoro, and "pause before you unplug, or swipe down before you start".
  - Also say that a hold resets from all positions, also on the task and the pill, and that it clears the task and the project.

## Changes to design decisions

- [ ] **Use one rule when the task changes during a run.**
  - When you say a new task, the knob renames the running Toggl entry. All the time in that stretch moves to the new task.
  - When you choose a task from the list during Tracking, the knob starts a new entry. The time stays on the old task.
  - Possible fix: rename the entry only if the stretch is shorter than 2 minutes, or if the task is "Untitled". In all other cases, start a new entry.
  - Decision that changes: "in a running Pomodoro it renames the entry", and voice renames the running entry.
- [ ] **Explain each knob bump.**
  - In Tracking mode the knob works only while the timer runs. In Pomodoro mode it works only while the timer does not run.
  - In the other states, the knob gives a bump with no text. A new user turns the knob first and sees nothing happen.
  - Possible fix: show a short hint with each bump, for example "start first" (Tracking, reset) or "resume to adjust" (Tracking, paused).
  - Decision that changes: "Not running: a bump" and "No hint text".
- [ ] **Let the knob correct a timer that you forgot to pause.**
  - The knob can move the start of a stretch earlier, which corrects a late start.
  - The knob cannot move the end of a stretch. As a result, you cannot remove the time after you forgot to pause.
  - Possible fix: while Tracking is paused, anticlockwise moves the end of the last stretch earlier, and clockwise moves it back, up to the time of the pause. The knob sends the new end to Toggl in a `PATCH`. The cloud task must keep the ID of the last entry.
  - Decision that changes: the Tracking knob works only while running.
- [ ] **Make the large time match the task above it.**
  - The time shows the session total, not the time on the current task.
  - The total continues when the task changes.
  - After an overnight pause, a tap adds the new time to the total from yesterday.
  - Possible fix: start the count again when a task change starts a new entry, or when the timer resumes after a long pause (for example more than 4 hours, or on a new day).
  - Decision that changes: "Pause keeps the session total", and the counter continues on a task change.
- [ ] **Show a hint after a restart during a run.**
  - After a restart, the knob shows "tap to track", also when its Toggl entry still runs.
  - A tap then stops the old entry at that time, so the old entry includes all the time that the knob was off.
  - The owner decided that sync is the user's responsibility. This item does not change that.
  - Possible fix: keep a "was running" flag in NVS. Set it on start and clear it on pause and reset. After a restart with the flag set, show "swipe down to sync" in place of "tap to track". This fix sends no requests to Toggl.
  - Decision that changes: none for the hint. A stronger version, where the first tap checks Toggl, changes "Syncing is manual".

## Smaller problems

- [ ] **Add a title to each list.** After an accidental tap, you must read the rows to know which list is open. Show "projects" or "recent tasks" near the top.
- [ ] **Make the mode easier to see at rest.** A reset Pomodoro looks almost the same as a reset Tracking timer. A small mode word or icon near the status line can show the difference.
- [ ] **Show when data is not in Toggl yet.** When the knob is offline, the only sign is the small Wi-Fi symbol. The knob does not show that stretches are waiting to go to Toggl, and "not saved in Toggl" shows for only 1.4 s. Show a mark that stays until the next successful sync.
- [ ] **Use different haptics for "closed" and "chosen".** A list that closes without a change plays the same haptic as a choice. Use the click haptic when nothing changed.
- [ ] **Confirm a task switch.** When a past task starts a new entry during Tracking, show a short hint such as "new entry".
- [ ] **Return a short Pomodoro run to reset.** If you pause a Pomodoro within 10 s, the Toggl entry is deleted, but the countdown stays paused at 24:5x. Return it to the set length.
- [ ] **Make three messages clearer.**
  - "no projects yet" means that the projects did not load. Better text: "projects not loaded".
  - "couldn't reach Toggl" also shows when the knob is offline. Better text in that case: "no Wi-Fi". During the rate-limit backoff: "Toggl busy, try later".
  - "synced from Toggl" means that the knob took a timer from another device. Better text: "picked up from Toggl".
- [ ] **Do not show "No project" on the first boot.** The pill shows "No project" for approximately 5 s, until the projects load and "Work" becomes the default. A new user can think that the setup failed.
