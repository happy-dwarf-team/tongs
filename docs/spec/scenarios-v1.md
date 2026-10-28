# Scenario support v1 specification

Tests of Europa Universalis IV 1.37.5 today find a province on a screenshot,
click a pixel, click through the Load dialog by coordinates and sleep a fixed
number of seconds before the next key. Scenario support v1 replaces that with
commands that have a verifiable answer: world reads (provinces, countries, the
player, the selection), navigation (open a province's view by ID, move the
camera to it), loading a save by name, and a client that waits until a session
is ready. The lab's scenario runner is their first user; mod authors and Etch
use the same public commands.

This document says what v1 contains and in what order it ships. It collects
the decisions of the
[wayfinder map](https://github.com/happy-dwarf-team/tongs/issues/94) in one
place; each section links the ticket that decided it, where the reasons and
the evidence are. Terms are those of [GLOSSARY.md](../../GLOSSARY.md) and
[ADR 0003](../adr/0003-client-calls.md). Addresses are those of the research
notes on the `research/*` branches; the implementation moves them into
`plugin/symbols.toml`.

| Decision | Where it lives |
| --- | --- |
| Where provinces, countries and the player live | [#95](https://github.com/happy-dwarf-team/tongs/issues/95) ([research](https://github.com/happy-dwarf-team/tongs/blob/research/world-objects/docs/research/world-objects.md)) |
| Which game functions open a province and move the camera | [#96](https://github.com/happy-dwarf-team/tongs/issues/96) ([research](https://github.com/happy-dwarf-team/tongs/blob/research/province-navigation/docs/research/province-navigation.md)) |
| How the game loads a save | [#97](https://github.com/happy-dwarf-team/tongs/issues/97) ([research](https://github.com/happy-dwarf-team/tongs/blob/research/save-load/docs/research/save-load.md)) |
| What today's game drivers repeat | [#98](https://github.com/happy-dwarf-team/tongs/issues/98) ([research](https://github.com/happy-dwarf-team/tongs/blob/research/game-drivers/docs/research/game-drivers.md)) |
| Client calls beside input and game calls | [#99](https://github.com/happy-dwarf-team/tongs/issues/99), [ADR 0003](../adr/0003-client-calls.md) |
| World-read commands | [#100](https://github.com/happy-dwarf-team/tongs/issues/100) |
| `province open` and `province goto` | [#101](https://github.com/happy-dwarf-team/tongs/issues/101) |
| `save load` | [#102](https://github.com/happy-dwarf-team/tongs/issues/102) |
| Where the client and the runner live | [#103](https://github.com/happy-dwarf-team/tongs/issues/103) |
| What a scenario looks like | [#104](https://github.com/happy-dwarf-team/tongs/issues/104) ([prototype](https://github.com/happy-dwarf-team/tongs/tree/prototype/scenario/prototype/scenario)) |
| The fields read in a running game | [#107](https://github.com/happy-dwarf-team/tongs/issues/107) |
| A load by file name, tried in the game | [#106](https://github.com/happy-dwarf-team/tongs/issues/106) |
| The ready wait | [#109](https://github.com/happy-dwarf-team/tongs/issues/109) |
| Releases and lab steps | [#105](https://github.com/happy-dwarf-team/tongs/issues/105) |

Until the release that ships `save load` (section 9), this specification is a
living document: when implementation learns something that changes it, a pull
request changes it. After that release it is frozen, and the next version gets
its own file.

## 1. Purpose and principles

- **Verifiable answers, no pixels, no sleeps.** Every new command answers only
  after Tongs has checked its effect on the next outer frame; a client waits
  for a predicate, never for a fixed time.
- **Tongs adds reads and client calls only.** Changing game state stays with
  game calls ([ADR 0001](../adr/0001-game-calls-in-tongs.md)): the console, and
  now `save load`. No new command writes owners, cores or buildings.
- **Public and versioned.** The commands and their answers are part of the pipe
  protocol and the MCP server and follow Tongs' semver: new fields in a minor
  release; a renamed or removed field, or a changed meaning, is breaking.
  Clients ignore unknown fields. `ping` gives the version; answers carry no
  schema number.
- **A measurement can reject a run.** Input, client calls and game calls are
  each counted, so a client can tell an untouched stretch from a marked run.
- **Reads on the main thread.** An off-thread reader sees half-applied days
  (#107), so every read runs on the main thread after an outer frame, between
  ticks.
- **No further than a player.** A client call refuses what a player could not
  do in the interface; a call that would change state is a game call.

## 2. Kinds of calls and their counts

Decided in #99, recorded in [ADR 0003](../adr/0003-client-calls.md).

| Kind | Switch | Counted in | Examples |
| --- | --- | --- | --- |
| Main-thread read, world read | `enabled` | not counted | `ui find`, `province ID`, `selection` |
| Input | `input` | `inputs` | `speed`, `key`, `ui click` |
| Client call | `input` | `client_calls` | `province open`, `province goto` |
| Game call | `game_calls` | `game_calls` | `console`, `save load` |

- `wait_until` and `run_until` answer `inputs`, `client_calls` and
  `game_calls` for their stretch; `state` carries `client_calls_total` beside
  `game_calls_total`; `activity` notifications carry `what: "client_call"`.
- A marked run is one with any input, client call or game call. A client that
  rejects marked runs checks all three counts, and treats a missing count (a
  Tongs older than these fields) as unknown, which rejects the stretch.
- A client call that would change game state is checked before calling and
  promoted: refused with `needs_game_calls` without `game_calls = true`,
  otherwise counted once, as a game call only, with ADR 0001's ironman and
  multiplayer refusals. The first case is section 6's discover case.
- Client calls follow input in ironman and multiplayer (Anvil's allow lists
  apply, Tongs adds no refusal) and in logging (no line per call; refusals and
  promotions go to the Anvil log).
- The `input` setting's description becomes "keyboard, mouse and navigation".

## 3. The Python client

Decided in #103. The pipe client stays one standard-library module,
`scripts/tongs.py`, vendored as `mcp/src/tongs_mcp/tongs.py` (`test_vendored.py`
keeps them equal). It is public; `server.py` builds on it instead of its own
closures and keeps its async layer and its MCP notes on cancelled calls.

- **Read timeout** per call: 30 s by default; for `wait_until` / `run_until`
  their own `timeout` plus a margin; for `save load` its `timeout` plus a
  margin.
- **Process pinning:** `expect_pid` / `expect_start_ms`, checked against `ping`
  on open and against every answer that carries them. A mismatch ends the
  connection with `process_mismatch`.
- **Cancel:** on a timeout or an interrupt during a wait, the client sends
  `cancel` on a second connection and waits a bounded time for the late
  answer, which it returns marked "effect unknown" or "answered late".
- **Progress:** an optional game-date callback (`subscribe date date=day` with
  the `ping` heartbeat), and `game_days`, moved out of `server.py`.
- **Typed steps** arrive with their commands: `ready` (section 4),
  `province` / `country` / `player` / `selection` (section 5), `open_province`
  / `goto_province` (section 6) and `load_save` (section 7). Each returns the
  plugin's answer unchanged and raises on `ok: false`.

Etch keeps its own TypeScript client (`tongs.ts`); `docs/commands.md` gives
the recipes it repeats (section 11).

## 4. Sessions and the ready wait

### The session field

Decided in #102.

- **`session`** in `state` and in every answer that carries the state: the
  number of the session in this process, 1 for the first game after start and
  +1 at every load, by Tongs or by the player. A client pins it like `pid`, so
  it sees a replaced session even when the date comes back the same.
- The `state` notification with `change: "entered_game"` carries `session`, and
  from the `save load` release also **`loaded_by`**: `"tongs"` after
  `save load`, `null` after a load by the player or a start.
- A load starts the counts kept "since the save was loaded" again.
  `wait_until` and `run_until` end when the game leaves the session, as today,
  so a load inside a measured stretch always breaks it.

### `ready()`

Decided in #109. A client-side composition; nothing changes in the plugin.

`ready(session=…, in_game=True|False, speed=None, timeout=…)` waits, with no
fixed sleep, until all of these hold at once:

1. the pipe answers from the pinned process;
2. `state` answers `in_game: true` with the expected `session`;
3. a `frames` answer has come whose state shows that session, so an outer
   frame of it has run (after a start, `state` says `in_game` about 6 s before
   the first outer frame);
4. `paused: true`.

| After | `session` | Default timeout |
| --- | --- | --- |
| a start with `continue=` | 1 (the wait for `paused` covers the game's own pause ~1 s after entering) | 180 s |
| `save load` | from its answer (its `frame` already shows an outer frame of the new session) | 30 s |
| a start to the main menu | none: pinned process, `in_game: false` and a `frames` answer | 60 s |

- **Proof by effect:** `speed=N` sets the speed with `speed N`; the confirmed
  change, with `waited_ms`, proves that input is handled (`input_proven: true`).
  When the speed already is N no key is pressed and `input_proven` is false.
  The press counts in `inputs` as a setup step. `ready` alone promises an outer
  frame, not handled input.
- **Fails at once** on `process_mismatch`, `unexpected_session` (a session
  above the expected one) and `left_game`. On a timeout the error names the
  first unmet condition (`pipe`, `in_game`, `session`, `frames`, `paused`,
  `speed`) and carries the last `state`.
- Returns the last `state`, `session`, `frame`, `waited_ms` and the `speed`
  answer. No window checks in v1.
- MCP: `tongs_ready`, built on the client. `docs/commands.md` describes the
  recipe for other clients.

## 5. World reads

Decided in #100, with the fields checked in the game in #107. Four top-level
commands; a number as the second token of `province` is the read, a word is a
subcommand (section 6).

| Command | MCP | Answer |
| --- | --- | --- |
| `province ID` | `tongs_province(id)` | `id`, `name`, `owner`, `controller`, `cores`, `base_tax`, `base_production`, `base_manpower`, `development`, `land`, `wasteland` |
| `country TAG` | `tongs_country(tag)` | `tag`, `exists`, `province_count`, `provinces`, `capital`, `observer` |
| `player` | `tongs_player()` | `tag`, `player_tag`, `observed_tag`, `observer` |
| `selection` | `tongs_selection()` | `last_selected`, `view: {province, visible, path}` |

- **IDs.** A province ID is a decimal integer, valid when `1 ≤ id < count`,
  with `count = (root+0x1C98 − root+0x1C90)/8` read live on every call (4942 in
  vanilla; the vector includes the dummy entry 0). Tongs checks the bound
  itself and never calls the game's getter. A tag is three characters
  `[A-Za-z0-9]`, case-insensitive, resolved through the game's 64 hash
  buckets; answers give tags in upper case; special tags are accepted when the
  game knows them. No names as input.
- **Values as the game stores them.** Development parts are raw ÷ 1000 without
  the capital bonus; `owner` / `controller` are `null` for `---`; `land` is
  false for seas and lakes; `name` is the displayed name, which follows the
  owner's culture, so never for matching. A dead country answers `ok` with
  `exists: false`.
- **The traps of `selection`:** `last_selected` survives closing the view and
  selecting an army or fleet, and after a load may still point at the province
  selected before it; only `view.visible` answers "is a province view open".
  The observed-tag path of `player` is not confirmed in the game.
- **Every answer** carries the fields of `state` and `queued_ms`, `run_ms`,
  `frame`. Each read re-derives everything from the root after an outer frame,
  so it cannot be stale within itself.
- **Errors:** `not_in_game`, `unknown_province`, `unknown_country`,
  `main thread busy` (no outer frame within `timeout=`, default 10 s, up to
  60), `read_failed` (a guarded read faulted or a guard such as `+0x20 = id`
  failed; later reads keep working), `unavailable` (the world-read symbols
  failed their check at start; the rest of Tongs works).
- **When they run:** under `enabled` alone, not counted, never marking a run;
  also during another connection's `run_until`, in ironman and multiplayer,
  and while Anvil has suspended Tongs.
- One object per command; country display names are not read in v1.

## 6. Navigation

Decided in #101. Both commands are client calls behind `input`.

- **`province goto ID`** (`tongs_province_goto`) moves the camera only, through
  session slot `0x278` (`0x80FD30`), as the game's alert clicks do; no map
  highlight. It takes any valid ID, sea, wasteland and terra incognita
  included.
- **`province open ID`** (`tongs_province_open`) follows the religion view's
  province link (`0xFF6350`): the terra incognita check, close other views,
  the camera as `goto`, then select and show (`0x139C830(session+0x1220,
  province)`). It always moves the camera.
- **What `ok` promises for `open`:** on the next outer frame the selection
  holder is the province, the province view shows it and the view's root
  widget is visible. The answer carries `province`, `view` (the widget path of
  the root that shows it, `"provinceview"` in vanilla), `covered_by` (the
  widget path over the view's centre or `null`; being covered is not a
  refusal), `camera` and the fields of `state`. A failed check answers
  `not_shown` with what was seen (`selected`, `view_province`, `visible`).
- **Camera (both):** wait until the current position reaches the target within
  a tolerance, or the timeout; answer
  `camera: {"settled": true|false, "target": {"x": …, "z": …}}`.
  `settled: false` at the timeout is still `ok`. With the camera locked,
  `goto` refuses `camera_locked`; `open` still opens and answers
  `"locked": true`.
- **`timeout=S`:** default 10 s, up to 60, for the whole command;
  `main thread busy` when no frame comes.
- **Refusals before calling** (nothing called, nothing counted):

  | Code | Commands | When |
  | --- | --- | --- |
  | `not_in_game` | both | no in-game session |
  | `input_disabled` | both | `input = false` |
  | `unknown_province` | both | ID outside `1 ≤ id < count` |
  | `not_selectable` | `open` | sea, wasteland or no land bit |
  | `terra_incognita` | `open` | the link's terra incognita test fails (an observer passes) |
  | `selection_blocked` | `open` | session mode `0x25` or the tutorial guard |
  | `camera_locked` | `goto` | `debug_lockcamera` is on |
  | `needs_game_calls` | `open` | the discover case without `game_calls = true` |

- **The discover case:** a foreign province whose owner's capital the player
  has not discovered makes the view post a discover command. With
  `game_calls = true` the call counts as one game call only and the answer
  carries `"game_call": true`.
- **Counting:** each `open` or `goto` that called the game is one client call,
  `not_shown` included.
- An uncolonised province is accepted by `open` with the same promise; the
  client reads its owner with `province ID`.

## 7. Loading a save

Decided in #102, with the load function tried in the game in #106.

- **`save load NAME [timeout=S]`** (`tongs_save_load`) is a game call: it needs
  `game_calls = true`, takes ADR 0001's refusals and counts once when Tongs
  called `0x5D0580`. In game only; a scenario that starts from a save uses the
  launcher's `continue=`.
- **NAME** is a file at the top level of `save games`. A trailing `.eu4` (any
  case) is optional and stripped (the loader appends it). Names with spaces
  are quoted. `bad_name` for an empty name, `/`, `\`, `..`, or characters
  outside printable ASCII and Latin-1. No display names, subfolders or cloud
  saves.
- **One answer, after the load and its check.** `timeout=` default 120 s, up
  to 600. A timeout before the job started withdraws it (`main thread busy`);
  a timeout during the load answers `error: "timeout"`, `started: true`.
- **What `ok` promises:** `0x5D0580` returned 0, and on the next outer frame
  the game is in game, the session's loading flag is clear, and the date and
  the player equal those Tongs read from the save's metadata before calling.
  The answer carries `save`, `save_date`, `player`, `compatible`, `load_ms`,
  `session`, `covered_by` and the fields of `state`; a failed check answers
  `not_loaded`.
- **Pause and speed:** refuses `not_paused` before calling; afterwards changes
  neither and only reports them (the save's speed).
- **Refusals:** `not_in_game`, `not_paused`, `bad_name`, `no_such_save`,
  `ironman_save` (unless `game_calls_ironman = true`) before calling; after
  calling, `bad_name` / `no_such_save` / `rejected` for the game's return codes
  1 / 2 / 3 (`called: true`), and `not_loaded`. A save from another version or
  with missing DLC or mods is not refused: `compatible` says so.
- **What it leaves:** the running game is discarded without saving and without
  a warning, as the in-game Load dialog does; Tongs never saves the game.
  Windows are closed as the dialog does (`0x824810`). `game_calls_total` is 1
  afterwards, `client_calls_total` 0, and `session` +1.
- Readiness for input is `ready()`'s (section 4).

## 8. Scenarios in the lab

Decided in #103 and #104. The runner is the lab's, not Tongs'; this section
fixes only what it asks of Tongs and the shape other test runners can copy.
"Scenario", "scenario run" and "measured stretch" are lab terms: they go into
the lab's glossary, not into Tongs'.

- **A scenario** is a Python file with literal `LAUNCH` needs (a save file name
  or the main menu, plugins and their switches, a CPU profile, `workers`),
  `PARAMS` with defaults, and a `run(s)` function. The runner reads the
  literals with `ast` and hands them to the launch script as JSON; `s` is a
  thin lab layer over the client of section 3, and a scenario never opens the
  pipe itself.
- **Process ritual in the lab's PowerShell:** one game lock per batch; every
  launch wrapped in the lab's verified start and close; the Python runner runs
  as its step with the verified PID and start time, and never starts or kills
  a process. One launch per scenario in v1.
- **Client and plugin from one Tongs tree:** `ping`'s version must equal that
  tree's `version.txt`, or the run stops before its first step.
- **Measured stretches:** a wait is setup unless it names a stretch
  (`measure=`). A stretch is valid when it reached its date unpaused, answered
  `inputs`, `client_calls` and `game_calls` all 0 (a missing field rejects),
  and `client_calls_total` / `game_calls_total` did not move. A stretch that
  drives the game on purpose declares `drives=` and is marked.
- **Verbs and verdicts:** `check` failing gives `failed`; `require` and lab
  checks failing give `rejected`; runner or process failures give `error`;
  otherwise `passed`. The first false one ends the run. `read(cmd, expect=…,
  within_s=…)` reads once or until `expect` holds.
- **Record:** one `eu4-lab-scenario-run.v1` per launch (the launch verdict,
  scenario hash, EXE hash, CPU masks, Tongs version, commit and client hash,
  switches, steps, stretches with counts and verdicts, the marked flag, the
  verdict), plus `steps.jsonl` with one line per pipe exchange.

## 9. Releases

Decided in #105. Four minor releases, each one or more `feat` pull requests.
The versions assume no other minor release comes first. Every release
pull request that changes addresses, the UI tree or input is checked in the
game as `AGENTS.md` asks, under the lab's game lock and procedures, by the
session that implements it, and its pull request says how; the checks below
are the minimum. No separate task tickets.

Every release also updates `docs/commands.md`, the commands table and
"Clients" in the README, and the MCP tools.

### 0.19: client, sessions, counts and ready

No new game addresses.

- The client of section 3 (timeouts, pinning, cancel, progress, `game_days`),
  with `server.py` built on it.
- `session` in `state`, in every answer with the state and in `entered_game`
  (section 4).
- The counts of section 2: `client_calls`, `client_calls_total`,
  `what: "client_call"`. They stay 0 until 0.21, but a client judging a
  stretch no longer meets a missing field.
- `ready()`, `tongs_ready` and the recipe in `docs/commands.md`.

**Checked in the game:** `session` is 1 after a start and +1 after a load by
the player through the UI; the counts across that load; the first `speed`
after `ready` takes about one frame, not 2–7 s (#109's hypothesis; if it
fails, only the docs and the default `speed` change).

### 0.20: world reads

- `province`, `country`, `player`, `selection` and their MCP tools
  (section 5).
- The world-read symbols move from `docs/research/world-objects.md` into
  `plugin/symbols.toml` with the bound `1 ≤ id < count`, and the lock is
  rewritten; a failed check makes only the world reads answer `unavailable`.
- `docs/commands.md` states the traps of `name` and `last_selected`.

**Checked in the game:** the values #107 saw (295 Moskva owned by `RUS`,
1754 Murom 4 / 7 / 4, `TUR` with 32 provinces, `SWE` with `exists: false`,
4941 answered and 4942 `unknown_province`), and a read during another
connection's `run_until`.

Ships before navigation, which needs its bound, its land bit and its view
visibility test, and has more facts left to check in the game.

### 0.21: navigation

- `province open`, `province goto` and their MCP tools (section 6), the
  promotion of the discover case, and the new description of `input` in
  `plugin/settings.toml`.

**Checked in the game:** whether the camera glides or snaps and the tolerance
for `settled`; whether the discover command fires in single player; whether
`small_provinceview` is active on the default interface; the root widget of
an uncolonised province; the selection after closing the view or selecting an
army; one `not_shown`.

**Copies:** "Settings" in the README and "Configuration" on the Tongs page of
anvil.happydwarf.dev get the new `input` description;
`workshop/description.md` ("What it doesn't touch") says that navigation moves
the camera, and is published to the Workshop.

### 0.22: loading a save

- `save load`, `tongs_save_load`, `loaded_by` in `entered_game`, and
  `game_calls_total` 1 after a load by Tongs (section 7).

**Checked in the game** (#102, "Left for implementation"): a direct
`0x5D0580` call with a name of its own, and `NAME` → `NAME.eu4`; the return
codes 1 and 3 read directly; `0x824810` outside the dialog and which windows
survive; reading the metadata before the call for text, compressed and binary
saves; the load time of a large late-game save against the 120 s default; the
counts and `session` across a load by the player.

**Copies:** `workshop/description.md` says that a save load discards the
running game without saving; the Tongs page of anvil.happydwarf.dev lists the
new commands.

Navigation and `save load` do not depend on each other and may be built in
parallel; the order above is the order of their version numbers. This
specification freezes with 0.22.

### Blocking, in short

- 0.19 blocks `ready()` in the lab's runner and every valid measured stretch.
- 0.20 blocks 0.21 (the shared bound, land bit and visibility test).
- 0.19 blocks 0.22 (`session` in its answer and `ready` after it).
- Nothing blocks 0.19.

## 10. Lab steps

The lab's own plan is in the lab; this section only fixes the order and what
each step takes from Tongs.

1. **Runner skeleton, now, on today's commands:** the runner, the launch script
   with parameter overrides, a SIMULATION year scenario with a stub `ready()`,
   and the lab glossary terms. Until 0.19 every measured stretch is rejected
   for the missing `client_calls`, as section 8 says.
2. **After 0.19:** the real `ready()`; a comparison batch that alternates the
   SIMULATION year scenario with the current SIMULATION driver under the same
   conditions; then that driver is retired in favour of the scenario, and its
   result rows are derived from the scenario record in the same format.
3. **Then:** the years driver that answers events becomes a scenario whose
   stretch declares `drives=`, so its runs are marked.
4. **After 0.20 and 0.21:** a scenario that opens province 295 and reads its
   owner.

## 11. Other consumers

Not blocking any release:

- **Etch** after 0.19: its ready wait follows the `ready` recipe in
  `docs/commands.md` instead of a fixed sleep.
- **Etch** after 0.21: the province-view recipes that click a map point use
  `province open`.
- **Etch's `tongs.ts`** ignores unknown fields, so new answer fields need no
  change there.

## 12. Out of scope for v1

- New commands that change game state (owner, cores, buildings): the console
  covers them.
- World reads offered to other plugins as an in-process service, like
  `tongs.ui`: no consumer yet, and it needs its own ADR.
- Several scenarios sharing one game launch by loading saves by name: v1 keeps
  one launch per scenario.
- Loading a save from the main menu without a restart: v1 starts from a save
  with `continue=`.
- Saves in subfolders of `save games` and Steam cloud saves: v1 takes
  top-level local names.
- Other views by ID (country view, outliner, ledger): no consumer in v1, and
  the outliner and ledger handlers are not identified.
- Armies (listing a country's armies, selecting one): where the game keeps the
  selected army was not found statically.
- Country display names: `0x2892A0` may allocate and cache, and needs a
  read-only path first.
- Batch world reads and a map scan: `country TAG` → `provinces` covers the
  nearest need.
- A per-session outer-frame counter in `state`: `ready()` works with `frames`.
- Whether `run_until events=first` chooses what `key return` does: a separate
  check in the game, outside this specification.

## 13. Open points to check during implementation

These are hypotheses or untested paths, not decisions; section 9 assigns each
to the release that checks it.

- Input handled late only until the first outer frame of a session (#109).
- The camera's motion and the `settled` tolerance (H1), the discover command
  in single player (H4), `small_provinceview` (H7), the root of an uncolonised
  province's view (#101).
- The observed-tag path of `player`, never seen written in #107.
- Whether a worker thread writes province fields (H8 in #107); reads stay on
  the main thread either way.
- A direct `0x5D0580` call by a name of Tongs' choosing, its return codes and
  the metadata read before it (#102, #106).
