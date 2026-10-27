# Research: what the lab's and Etch's game drivers repeat today

Ticket: happy-dwarf-team/tongs#98 (child of the map #94). Static reading only:
no game was started and no script was run.

Sources, read on 27 October 2026:

- **The lab** (`eu4-engine-lab`, a private repository at `565d0ed` plus its
  working tree): `scripts/measure-year-tongs.ps1`, `scripts/anvil-play.ps1`,
  `scripts/drive-years.py`, `scripts/workshop-e1.ps1`,
  `scripts/lib/{LabProcess,GameLaunch,GameThreads,SimulationTrial}.psm1`,
  `scripts/post-window-input.ps1`, `scripts/monitor-gameplay.ps1`,
  `scripts/tests/`, `docs/ui-workflow.md`, `docs/safety.md`,
  `docs/measurement.md`, `docs/reverse-engineering.md`, the
  `docs/*-game-test.md` reports, the measurement reports that use these
  drivers (`docs/bellows-*.md`, `docs/anvil-1.*-budgets.md`,
  `docs/simulation-profile.md`), `HANDOFF.md` and `AGENTS.md`. Untracked
  batch scripts in the lab's scratch folder that the reports name (a memory
  logger, an input-latency loop, a load-from-menu loop and their PowerShell
  wrappers) were read as examples of how the tracked drivers get copied. The
  lab is private, so this note describes what its scripts do and names them by
  repo-relative path only.
- **Etch** at [`f83286d`](https://github.com/happy-dwarf-team/etch/tree/f83286d364f1dacacfeb91cb00f12222891f3a00):
  the corpus capture driver in
  [`packages/core/src/corpus/driver/`](https://github.com/happy-dwarf-team/etch/tree/f83286d364f1dacacfeb91cb00f12222891f3a00/packages/core/src/corpus/driver)
  (`cli.ts`, `run.ts`, `view.ts`, `tongs.ts`, `game-script.ts`, `game.ps1`),
  [`corpus/README.md`](https://github.com/happy-dwarf-team/etch/blob/f83286d364f1dacacfeb91cb00f12222891f3a00/corpus/README.md),
  [`corpus/manifest.json`](https://github.com/happy-dwarf-team/etch/blob/f83286d364f1dacacfeb91cb00f12222891f3a00/corpus/manifest.json)
  and [`docs/live-game.md`](https://github.com/happy-dwarf-team/etch/blob/f83286d364f1dacacfeb91cb00f12222891f3a00/docs/live-game.md).
- **Tongs** at `7336ad4` (0.18.0): [`scripts/tongs.py`](../../scripts/tongs.py),
  [`scripts/tongs.ps1`](../../scripts/tongs.ps1),
  [`mcp/src/tongs_mcp/tongs.py`](../../mcp/src/tongs_mcp/tongs.py),
  [`mcp/src/tongs_mcp/server.py`](../../mcp/src/tongs_mcp/server.py),
  [`mcp/live_check.py`](../../mcp/live_check.py),
  [`mcp/live_check_events.py`](../../mcp/live_check_events.py) and
  [`docs/commands.md`](../commands.md).

"Verified" below means read in the source or in a report's recorded
observations; it says nothing new about run-time behaviour.

## Answer in short

- Six code paths drive the game today. Two own the whole run (the lab's
  SIMULATION trial `measure-year-tongs.ps1` and Etch's corpus capture driver),
  one hosts a session for someone else to drive (`anvil-play.ps1`), and the
  rest are pipe clients that run inside such a session (`drive-years.py` and
  its copies, the Tongs MCP live checks) or wrap one (`workshop-e1.ps1`).
- **The process ritual is shared code:** game lock, no running game, Process
  Lasso rule and `eu4.exe` hash, start with an explicit mask, close only a
  verified PID, close the game's own restarts, restore the settings baseline.
  It lives in the lab's `LabProcess.psm1` and `GameLaunch.psm1`; Etch calls
  `LabProcess.psm1` through its own `game.ps1`. Python drivers do none of it
  and rely on the PowerShell wrapper that started the session.
- **The game-level steps are copied, not shared:** connect with retries; wait
  for `state` to say `in_game` and `paused`; sleep a fixed 8 s (Etch: 5 s)
  because input is ignored right after a load; `speed 5`; `run_until` a date
  with `max_pause`; on a pause, `resume`, else `key return`, and go on; log
  each answer as a JSON line. The same loop is in four Python files.
- **Pixel coordinates are left in three places:** the documented UI path
  without Tongs (the old SIMULATION method), Etch's province-view recipes
  (a click at a map point, 4 recipe variants), and Anvil's ImGui overlay and
  Manager, which are outside the game's widget tree.
- **The most frequent failure is an event that pauses the game:** about one
  SIMULATION trial in five is rejected for it, and long coverage sessions
  answer up to 124 events with the default option. Next come the steps around
  a save load: input ignored for seconds, the Esc menu left open, a confirm
  button reported `covered`, and the wanted save not among the rows the load
  dialog shows.
- **What `tongs_mcp`'s client offers a runner** is a small synchronous pipe
  connection (`Tongs.call`, `raw`, `subscribe`, `notifications`), with no read
  timeout and nothing about processes, readiness or results. The MCP server
  adds call cancellation, progress and date arithmetic, but as closures inside
  `build_server`, not as a library.

## The drivers

| Driver | Repo, language | Starts and ends the game | Game lock | Setup checks | Talks to Tongs | Waits | Records |
| --- | --- | --- | --- | --- | --- | --- | --- |
| `scripts/measure-year-tongs.ps1` | lab, PowerShell | per trial `Invoke-GameLaunch` with a `-Start` block that runs the lab's launcher with the affinity broker (`continue=<save>`); closes the verified session | one batch lock for all trials (`Enter-LabGameLock -HolderPid`) | preflight in `Invoke-GameLaunch`; launcher verifies both pools; then Tongs `threads` (pools, process mask, main thread id and mask) before, after warm-up, after the stretch; `speed 5` must answer `speed_internal` 4; save hash before and after; settings diff | `scripts/tongs.ps1` run as a new `pwsh` process per command (`Invoke-Tongs`), one connection each | poll the launcher's `status.json`, then `ping` until its `pid` is the launcher's, then `state` until `in_game` and `paused`, then sleep 8 s; `run_until … timeout=300 max_pause=5` | `result.json` per trial with the launch verdict, thread CPU, both `run_until` answers and the rule verdict; one CSV row per trial |
| `scripts/anvil-play.ps1` | lab, PowerShell | `Invoke-GameLaunch`, own start (suspended `CreateProcess`, mask `0xFE`), `continue=<save>` or main menu; ends on game exit, `stop.txt`, `next.txt` or `-MaxMinutes`, then closes the verified session and its restarts | series lock for all starts | preflight, plugin manifests (`api = 2`, Anvil version), save hash, settings baseline | none itself: installs Tongs with `input` / `game_calls` switches; an agent (MCP) or a Python driver drives | polls the session every 2 s | `result.json` (starts, close outcome, restarts, settings diff, files left), copies of `anvil.log` and plugin data; `monitor-gameplay.ps1` samples memory and the native speed |
| `scripts/drive-years.py` | lab, Python | does not start or close; runs inside an `anvil-play` session started by a wrapper that also writes `stop.txt` | none (the wrapper holds it) | none: no pid check, no hash, `speed 5` answer only logged | `tongs/scripts/tongs.py` imported from the lab's Tongs checkout, one connection for the whole run | retry `Tongs(timeout_s=1)` until the pipe opens, `state` until `in_game` and `paused`, sleep 8 s; `run_until <year>.1.1 speed=5 timeout=900 max_pause=3` per year | one JSON line per command answer; optional copy of a plugin counters file after each year |
| `scripts/workshop-e1.ps1` | lab, PowerShell | starts `anvil-play.ps1 -Scenario tongs -NoContinue -TongsInput` in the background; `-Action Stop` writes `stop.txt` and waits | through `anvil-play` | backs up and restores `dlc_load.json`, hashes the probe mod | none itself; the checksum and the achievements tooltip were read through Tongs by an agent | waits for the `anvil-play` process | `e1-start.json`, `e1-stop.json` with the nested `anvil-play` result |
| Etch corpus capture driver (`cli.ts`, `run.ts`, `view.ts`, `game.ps1`) | Etch, TypeScript + PowerShell | `game.ps1 -Action Start`: lab preflight on both the original and a junction copy, `Start-LabGame` with mask `0xFE`, `-userdir=` a private profile, `continue=` when the view starts from a save; `-Action Stop` closes only the verified record, its restarts, and lists crash reporters | `game.ps1 -Action Lock` / `Unlock` (`Enter-LabGameLock`), inheriting a batch's lock; no session starts after the lock's expiry | copy and plugin hashes against `host.json`, manifest's exe hash, save and mod hashes before and after, settings and `pdx_settings.txt` baselines, the player's profile unchanged | own Node client (`tongs.ts`): one connection, 30 s read timeout per call that kills the connection | `ping` until its `pid` and `process_start_ms` match the started game, `Running application` in the profile's `system.log`, `state` (`in_game` and `paused`, or `in_game` false for the main menu), `frames`, then sleep 5 s (save) or 3 s (menu); per step `frames` plus 700 ms; after moving the pointer away 1.5 s | a capture record per view (window, widget trees, screenshot hashes), PNGs, `report.json`, `events.jsonl` per session |
| [`mcp/live_check.py`](../../mcp/live_check.py), [`mcp/live_check_events.py`](../../mcp/live_check_events.py) | Tongs, Python | none: needs a loaded game | none | none | MCP stdio client to `python -m tongs_mcp`, plus a raw `Tongs` connection for a parallel wait | `tongs_wait_until` with progress, `tongs_cancel`, 30-day steps until an event opens | one JSON line per tool call |

Etch's live game in the VS Code extension also starts and closes the game,
with its own lock file and only Tongs' in-process service `tongs.ui` (the pipe
is kept off), so it is not a pipe driver and is left out here
([`docs/live-game.md`](https://github.com/happy-dwarf-team/etch/blob/f83286d364f1dacacfeb91cb00f12222891f3a00/docs/live-game.md)).

### `measure-year-tongs.ps1`

- **One trial = one game launch.** `Invoke-GameLaunch` runs the checks before
  anything changes: the lock and no running game (`Assert-NoGameRunning`), the
  Process Lasso rule `eu4.exe,0,1-7` and the original `eu4.exe` hash
  (`Assert-LabPreflight`), the given file hashes, then the settings baseline.
  It installs Anvil, anvil-game and Tongs with `enabled = true` and
  `input = true` (and optionally Bellows) as its prepare step, starts through
  `launch-worker-test.ps1` with the broker, opens the launcher's
  `verified-session.json` (PID, image path, creation time) and runs the step.
  Afterwards, always: close the session and its restarts, cleanup only when
  nothing runs, restore the baseline.
- **Setup on the pipe.** `ping` must answer the launcher's PID. `threads`
  must show two pools at N workers, process mask `0xFFFF`, the launcher's main
  thread as the window thread and its mask `0x10`. The game re-pins its main
  thread when its main loop starts, so the script pins it again through
  `GameThreads.psm1` when the mask differs. `speed 5` must answer `speed` 5
  and `speed_internal` 4. The rules are pure functions in
  `SimulationTrial.psm1`, tested on trimmed recorded answers
  (`scripts/tests/test-simulation-trial.ps1`).
- **Measuring.** `run_until <warm-up date>` then `run_until <end date>`, both
  `speed=5 timeout=300 max_pause=5`; Tongs' `wall_ms`, `paused_ms`,
  `start_speed`, `speed_internal` and `overshoot_hours` decide validity.
  Thread CPU is read from outside before and after the stretch.
- **Tongs channel.** Each command is a new `pwsh` process running
  `tongs.ps1 <command> -TimeoutMs 5000` (the connect timeout); a blocking
  `run_until` holds its process until the game answers.
- A working-tree change (not committed when read) adds `-Observe`: Tongs with
  `game_calls = true` and `console observe` after loading, for a late save
  whose player events would pause every trial. The script marks it as not the
  standard condition.

### `anvil-play.ps1` and the session wrappers

`anvil-play.ps1` is the lab's host for gameplay sessions: it installs the
chosen plugins in the real game folder, starts the game (own start with mask
`0xFE`, so the Process Lasso baseline), writes the session's
`verified-session-<n>.json` for other tools, starts the read-only
`monitor-gameplay.ps1`, and waits until the game exits, `stop.txt` or
`next.txt` appears, or the time limit passes. It sends nothing to Tongs.

Driving happens beside it, in one of two ways:

- an agent through the MCP server `tongs` (lab `AGENTS.md`, "Interfejs":
  start with `tongs_state`, find widgets with `tongs_ui_find`, click by name
  with `tongs_ui_click`);
- a Python pipe client started by a PowerShell wrapper. The reports name four:
  `scripts/drive-years.py` (tracked) and untracked copies for a memory logger,
  an input-latency loop and a load-from-menu loop. Each wrapper takes the game
  lock, starts `anvil-play.ps1` in the background, runs the Python driver,
  writes `stop.txt`, waits for `anvil-play` and releases the lock. The wrapper
  hard-codes the game folder to copy plugin output from.

### `drive-years.py` and its copies

All four Python drivers start with the same block, copied: retry
`Tongs(timeout_s=1.0)` once a second until the pipe opens or a load timeout
passes, poll `state` every 0.5 s until `in_game` and `paused`, then
`time.sleep(8)` with the comment that the main loop handles input only seconds
after loading. The year loop is copied as well: `run_until <year>.1.1 speed=5
timeout=900 max_pause=3`; when the answer's `error` mentions `max_pause`,
send `resume`; when `resume` is refused (an event waits for a choice), send
`key return` and loop. They log each answer and end with `pause`.

None of them checks that the pipe answers from the game the wrapper started
(`ping`'s `pid` and `process_start_ms`), none uses `run_until events=first`
(Tongs 0.6.0, a day after `drive-years.py` was written), and none reads
`inputs` or `game_calls` from the wait answers.

### Etch's corpus capture driver

- **Plan.** One session per playset × resolution × start state (`main-menu`
  or `save`); each session is a fresh start in a private profile whose
  `settings.txt` and `pdx_settings.txt` are set to the resolution and put back
  afterwards.
- **Ready.** `waitReady` in `run.ts`: the pipe's `ping` must name the started
  PID and a `process_start_ms` within 2 s of the recorded creation time
  (otherwise the run stops), `Running application` must be in the profile's
  `system.log` after the start, `state` must show the start state, and a
  `frames` answer must come. A playset may name a `confirm` widget (Anbennar's
  achievements warning, `DefaultConfirm/AgreeButton`) that is clicked while
  the save loads. Then a fixed 5 s (save) or 3 s (menu).
- **Views.** `view.ts` brings the game back to the start state with Escape,
  at most 4 presses, comparing the visible top-level windows from
  `ui tree depth=0`; runs the recipe's steps (`key`, `ui click … timeout=10`,
  `text`, `click X Y`, `scroll`), each followed by `frames` and 700 ms; moves
  the pointer away; checks `state` (same `pid`, `game_calls_total` 0, start
  state still true); reads `ui tree ROOT all` for each root; takes at least
  two screenshots (four when they differ) and reads the trees, the window and
  the state again, failing the view if anything changed. A `covered` refusal
  of `ui click` is retried up to 4 times.
- **Failure handling.** A view that loses the start state ends the session;
  the remaining views get a fresh start, at most 2 per session. Anything that
  breaks the lab's procedure (game not closed, settings not back, save, mod or
  player profile changed) stops the run.
- The driver pins the real mouse cursor inside the window (`ClipCursor`)
  because the game scrolls the map toward an outside cursor, and pauses the
  run when the user takes the mouse back.

## Steps repeated across drivers

| Step | Where it is today | Copies |
| --- | --- | --- |
| Game lock, no running game, Lasso rule and exe hash, start with mask, close by verified identity, restarts, settings baseline | lab `LabProcess.psm1` + `GameLaunch.psm1`; Etch `game.ps1` calls `LabProcess.psm1` and re-does the baseline in TypeScript | shared for PowerShell; Etch re-implements the baseline and the launch order |
| Install Anvil and Tongs with `settings.toml` (`enabled`, `input`, `game_calls`) | `measure-year-tongs.ps1`, `anvil-play.ps1` (real game folder), Etch `game.ps1 -Action Prepare` (junction copy from release zips) | 3 |
| Connect until the pipe opens | 4 Python drivers, Etch `waitReady`, `measure-year-tongs` (`Wait-Until … ping`) | 6 |
| Pipe answers from the started process (`ping` `pid`, `process_start_ms`) | Etch (both fields), `measure-year-tongs` (`pid` only) | 2 of 6 |
| Wait for `in_game` and `paused`, then a fixed sleep for input | 4 Python drivers and `measure-year-tongs` (8 s), Etch (5 s / 3 s, after `frames`) | 6 |
| `speed 5` and its check | `measure-year-tongs` checks `speed_internal`; Python drivers only log it | 5 |
| `run_until` with `max_pause`, then `resume` or `key return` | `drive-years.py` and three copies | 4 |
| Reject a run on pauses, input or game calls | `SimulationTrial.psm1` (`paused_ms`); Etch (`game_calls_total`); no driver reads `inputs` | 2 |
| Load a save from the in-game menu by `ui click` (menu button, Load Game, the row by title, Load, confirm, Escape the menu) | the load-from-menu loop; agents by MCP in the long Bellows sessions | 2 |
| Open a province view | Etch recipes: outliner entry + click at the map centre, or Goto box + typed name + Return + click at a map point | 4 recipe variants |
| One JSON line per step, a result file per run | every driver, each in its own shape | 6 formats |

## Steps still done by screenshot or pixel coordinates

- **The UI path without Tongs.** `docs/ui-workflow.md` and `HANDOFF.md`
  (section 14) keep an atlas of 3440×1440 points: the menu button, Load Game,
  Show incompatible, a scroll-bar drag, the save row, Load, OK, the pause
  toggle, speed 5, the Terrain map mode. The procedure is fresh image, one
  action, fresh image, about 350 ms after a click because the first image can
  still show the previous frame. The old SIMULATION method (`measure-year.ps1`
  with a monitor of autosave dates) still depends on it; the Tongs path
  replaced it for new series.
- **Etch's province views.** 4 of the manifest's recipe variants end with a
  `point` or `map` step: a click at a point of the client area (as fractions)
  or of the map (as client heights from the centre), after the outliner or the
  Goto box moved the camera. There is no widget that opens a province, and the
  Anbennar save opens with three events over the middle of the screen.
- **Anvil's overlay and Manager.** The ImGui overlay is drawn outside the
  game's widget tree and reads input through the window procedure, so
  `tongs_key` does not reach it. The overlay tests posted window messages at
  client coordinates (`scripts/post-window-input.ps1`) and looked at
  `PrintWindow` or `tongs_screenshot` images. Posted messages carry no
  modifiers (the panel key was set to F11 for the test), and a "Review" click
  closed a notification without opening the panel, possibly because move and
  click came in the same frame. Clicks posted to the Manager did not register
  at all; the user clicked.
- **Agent sessions by computer use.** One gameplay session was played by
  clicks alone through another agent's computer use; the app's consent prompt
  expired after 5 minutes and the game stood paused until the 60-minute limit
  (lab `docs/anvil-gameplay.md`).
- **Screenshots as evidence, not control.** Etch's captures, the overlay
  tests and agents' `tongs_screenshot` calls look at images to check a result;
  no Tongs-driven step chooses a click from an image.

## Failures the docs record

| Failure | Recorded | Handling today |
| --- | --- | --- |
| An event pauses the game in the warm-up or the measured year | lab SIMULATION series: 1 of 5 (`measurement.md`), 1 of 7 (Anvil 1.0 RC), 1 of 7 (Anvil 1.3), 2 of 8 (Anvil 1.5), 6 of 21 (Bellows frame pacing), 5 of 25 + 9 of 42 + 6 of 32 + 14 of 66 (Bellows mimalloc): 45 of 213, roughly one in five | `max_pause=5` ends the stretch; the trial is rejected and the batch goes on. Before `max_pause`, a trial waited for the 300 s timeout on an event |
| An event that needs a choice in a coverage session | 2 resumes to 1727, 12 to 1770, 124 to 1821 (`simulation-profile.md`) | `resume`, else `key return` (the default option), so the campaign differs from a game without a player; `console observe` is the planned way out for late saves |
| Input ignored right after a load | presses handled 2–7 s late and piling up (`reverse-engineering.md`) | Tongs input commands wait up to 10 s; drivers add a fixed 8 s (lab) or 5 s (Etch) |
| The Esc menu left open after a load from the menu | a segment stopped with "resume: no change within 10000 ms" because Space does not resume while the menu is open (`bellows-mimalloc-startup.md`) | Escape, then `ui find menu/menu_panel` and a second Escape if needed |
| A confirm button refused as `covered` | the load confirmation was `covered` by `menu/menu`; confirmed with Return instead (`bellows-mimalloc-startup.md`) | Return as a fallback; Etch retries `covered` up to 4 times while a window grows in |
| The wanted save not among the load dialog's rows | 4 of 12 load attempts in the load-from-menu loop's logs failed with no row of that title, every one for a save below the autosaves at the top of the list: the list box builds widgets only for the 14 rows it shows; the 8 loads of the autosaves completed | none; the UI path dragged the scroll bar by coordinates (the wheel did not scroll the list) |
| Wrong speed after a reload | the UI path: a reload resets the speed and five clicks are no proof (`ui-workflow.md`) | Tongs `speed N` presses until the speed is N and answers `speed_internal`; only `measure-year-tongs` checks it |
| Stale frames and images | a screenshot right after a click may show the previous frame (`ui-workflow.md`) | wait about 350 ms and look again; Etch waits for `frames`, takes two or more shots and rereads the trees |
| The game re-pins its main thread | after loading, the main thread had the middle CPU of the mask in every trial of the first Tongs series (`measurement.md`) | `measure-year-tongs` pins it again and records what it found |
| A pipe answer from another process | none recorded; Etch stops the run when `ping` names another PID or start time | Python drivers do not check |
| Launcher state files and the broker | the launcher could not write its `status.json` while the measuring script read it, so a game ran unverified (`bellows-frame-pacing.md`, fixed by `LabJsonFile.psm1`); a broker stopped mid-batch, one trial timed out on its request and the next 13 did not start (`bellows-mimalloc-startup.md`) | atomic writes and tolerant reads; the measuring script gives up when the launcher reports a failure |
| The game restarts itself | after Exit to Menu → Quit, and from the in-game menu, EU4 starts a child `eu4.exe` (Anvil 1.0 RC report, Etch `live-game.md`) | `Stop-RestartedGame` closes verified children; Etch's `Stop` also lists `CrashReporter.exe` children |
| Dialogs outside the game | UAC for the broker (the user), the computer-use consent timeout, the Manager not taking posted clicks | the user acts by hand |
| A hang | a game with stale windows on screen deadlocked in `autosave` (Etch `live-game.md`) | Etch's own call timeout (30 s, `frames` 40 s); the lab's Python and `tongs.ps1` calls have no read timeout |

## What `tongs_mcp`'s client offers a Python runner

[`mcp/src/tongs_mcp/tongs.py`](../../mcp/src/tongs_mcp/tongs.py) is a verbatim
copy of [`scripts/tongs.py`](../../scripts/tongs.py) (`mcp/tests/test_vendored.py`
keeps them equal). The lab's drivers import the `scripts/` copy by putting the
lab's Tongs checkout on `sys.path`, not the installed package.

It offers, standard library only and Windows only:

- `Tongs(timeout_s=2.0)`: opens `\\.\pipe\tongs`, retrying every 50 ms while
  all 8 pipe instances are busy; a context manager.
- `call(command, check=False)`: one line out, one parsed JSON answer back;
  `check=True` raises `TongsError` (with `command` and `answer`) on
  `"ok": false`. `raw(command)` returns the line as text.
- Notifications that arrive while waiting for an answer are kept; `subscribe`,
  `unsubscribe` and `notifications(seconds, count)` turn a connection into a
  stream.

It does not offer:

- a read timeout: `call` waits for ever (`_line(None)`), so a hung game hangs
  the runner; a long `run_until` holds its connection, and `cancel` needs a
  second one;
- reconnecting, or any notion of which process answers (`pid` and
  `process_start_ms` are in the answers, but nothing compares them);
- typed helpers for `state`, `run_until`, `ui click` or `event choose`, or
  the waits every driver writes (pipe up, save loaded, input accepted);
- anything about the game lock, starting or closing the game, the setup
  checks or result records: those are PowerShell modules in the lab.

What [`server.py`](../../mcp/src/tongs_mcp/server.py) adds on top lives in
closures inside `build_server` (`exchange`, `send`, `late_notes`,
`report_dates`, `wait`), so a runner cannot import it: one connection per
call, notes on cancelled calls whose effect is unknown, `cancel` sent when a
wait is abandoned, progress from `subscribe date date=day` with a `ping`
heartbeat while paused, and refusing arguments with line breaks. Only
`game_days` (EU4 dates as days) is a module-level function.

The pipe protocol itself already has much of what the drivers rebuild by hand
([`docs/commands.md`](../commands.md)): `ping` and every state answer carry
`pid` and `process_start_ms`; `state` has `game_calls_total`; wait answers
have `paused_ms`, `paused_date`, `inputs` and `game_calls`; `run_until` has
`max_pause` and `events=first`; `frames` waits for a frame of the main loop;
`subscribe` streams `state`, `date`, `event` (with who chose an option) and
`activity`; `ui click` refuses with a reason instead of clicking blind.

## New questions this raises

1. Should the runner own the process ritual (lock, preflight, launch, close,
   baseline) by calling the lab's PowerShell modules, as Etch's `game.ps1`
   does, or stay a pipe client inside `anvil-play.ps1` like `drive-years.py`?
2. Can a ready predicate replace the fixed sleeps: pipe from the started
   process, `in_game` and `paused`, `frames`, and something that says input is
   accepted (Tongs has no such field)?
3. Does `run_until events=first` choose what `key return` chooses (the first
   enabled option against the game's default), so coverage sessions can drop
   the resume loop? Not checked.
4. Should the shared client get a per-call read timeout and a `pid` check,
   and should `server.py`'s cancel and progress logic move into the client so
   a runner can use it?
5. What must "load a save by name" cover beyond the click path: saves not in
   the visible rows, the confirm dialog (and Anbennar's achievements warning),
   the Esc menu the game leaves open, and the 2–7 s of ignored input after?
6. Will "open a province's view by ID" replace Etch's map-point steps, and
   what does it open for a province the game shows another panel for (an
   uncolonised one opens the colonisation panel, Etch spec v2)?
7. Which result record shape should the runner write, given six today, and
   should it read `inputs` and `game_calls` so a run that used the new
   commands can be rejected?
