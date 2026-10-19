# Tongs

**Control Europa Universalis IV from scripts and agents.** An
[Anvil](https://github.com/happy-dwarf-team/anvil) plugin that lets test agents
and scripts talk to a running game through a local named pipe, instead of
reading pixels from screenshots. Like a smith's tongs, it holds the work from
outside the forge.

> Status: early. It reads game state, the interface's widget tree and the open
> events, takes screenshots and, when you allow it, uses the keyboard and mouse
> in the game (pause, speed, run to a date, keys, text, clicks on points or on
> widgets by name, event choices) through the game's own SDL input, so the
> window does not need focus. With `game_calls = true` it also runs console
> commands, which change game state; without it, it never does. A connection
> can also subscribe to notifications: state and date changes, events opening
> and closing, new `error.log` lines and other clients' input and game calls.
> Supported game build: EU4 1.37.5
> (Steam, Windows 64-bit); Anvil skips it on other builds. Design and the
> addresses it relies on: [docs/design.md](docs/design.md).

## Commands

The pipe is `\\.\pipe\tongs`. A request is one line of text; the answer is
one JSON object on one line. Up to 8 clients can be connected at once, so one
can wait in `run_until` while another reads `state` or sends `cancel`, and a
connection can instead carry notifications (see "Notifications").

| Command | Answer |
| --- | --- |
| `ping` | plugin version, game process id (`pid`) and its creation time (`process_start_ms`, Unix ms) |
| `state` | `in_game`; with a loaded game also `date` (`"1719.3.17"`), `date_raw`, `hour`, `speed` (1–5 as in the UI), `speed_internal` (0–4), `paused` (the game's own answer), `paused_flag` (the player pause byte) and `game_calls_total` (game calls since the save was loaded); always `pid` and `process_start_ms`, which every answer that carries the state repeats, so a client can tell a restarted game apart |
| `window` | the game window: client `width` / `height`, `screen_x` / `screen_y`, `focused`, `minimized`, `thread_id` (the game's main thread), `dpi` |
| `threads` | worker threads in each of the game's two pools, the `threads` option, process and main-thread CPU masks, ideal CPU and priority of the main thread, number of threads |
| `screenshot [NAME]` | writes a PNG of the game window to `anvil/data/tongs/screenshots/` (keeps the newest 100) and answers with its `path`, size and capture `method`; works while the window is covered |
| `frames [timeout=S]` | read on the game's main thread after a frame of its main loop: frames per second (`outer_per_s`, and `nested_per_s` for frames drawn inside a game update or a save load), counts and the longest gap between main-loop frames since the previous `frames`, and the main thread's `thread_id`; with `queued_ms`, `run_ms` and the game's `frame` counter. Waits up to 10 s (`timeout=` up to 600) for a frame; while a save loads none comes, and the answer is `main thread busy` |
| `ui find SEL [all] [limit=N]` | widgets that match the selector `SEL` (see "Widgets"): `count`, `truncated` and up to 50 `matches` (`limit=` up to 5000), each with `path`, `type`, `visible`, `rect` and `click_point` in client pixels, and `text` / `enabled` where they apply. Read on the main thread, like `frames` |
| `ui tree [SEL] [depth=N] [limit=N] [all]` | the widget tree from the top-level windows, or from the matches of `SEL`, as nested `widgets` (up to 200, `limit=` up to 5000); `depth=N` gives deeper levels only as `children_count` |
| `event list [timeout=S]` | the open events, topmost first (see "Events"), read on the main thread |
| `wait_until DATE [timeout=S] [max_pause=S]` | waits (without touching the game) until the date reaches `DATE` (`1720.1.1` or `1720.1.1.12`); answers with the timing below |
| `cancel` | ends every `wait_until` / `run_until` in progress and withdraws main-thread jobs still waiting for a frame (`withdrawn`) |
| `subscribe KINDS [date=day\|month\|year]` | turns the connection into a stream of notifications (see "Notifications"); `unsubscribe` ends it |
| `help` | list of commands and whether input commands are enabled |

With `input = true` in the config, also:

| Command | What it does |
| --- | --- |
| `pause` | presses Space unless the player pause is already on, then waits until it is |
| `resume` | presses Space if the player pause is on, then waits until the game runs; refuses when something else holds the game paused |
| `speed N` | presses keypad `+` / `-` one step at a time until the speed is `N` (1–5, as in the UI); works while paused |
| `run_until DATE [speed=N] [timeout=S] [max_pause=S] [events=first\|stop]` | sets the speed, resumes, waits for the date and pauses again; for measuring how fast the game runs. `events=first` chooses the first enabled option of every event that opens on the way |
| `key NAME` | presses one key: `a`–`z`, `0`–`9`, `f1`–`f12`, `space`, `return`, `escape`, `tab`, `backspace`, `delete`, `minus`, `equals`, `grave` (the console key on a US layout), `kp_plus`, `kp_minus`, `kp_enter`, arrows (`up`, `down`, `left`, `right`), `home`, `end`, `page_up`, `page_down` |
| `text STRING` | types the rest of the line into the focused text field (characters U+0020–U+007E and U+00A0–U+00FF) |
| `move X Y` | points the mouse at client coordinates `X Y` (hovering shows tooltips) |
| `click X Y [left\|right\|middle] [double]` | moves there and clicks |
| `scroll STEPS [X Y]` | turns the wheel; positive scrolls up (zooms in on the map), negative down |
| `release` | hands the pointer back to the real mouse |
| `ui click SEL [timeout=S]` | clicks the centre of the one widget the selector matches (see "Widgets") |
| `event choose N\|text^="..." [event=N\|event=title^="..."] [timeout=S]` | clicks an option of an open event (see "Events") |

With `game_calls = true` in the config, also (see "Console"):

| Command | What it does |
| --- | --- |
| `console LINE` | runs one console command line in the game, on its main thread, and answers with the game's text and the state afterwards. **Changes game state** |

Input commands answer once the game has reacted (or after 10 s) with
`command`, `presses`, `waited_ms` and the fields of `state`. When the game
does not react in time, `ok` is `false` with an `error`, and the state is
still included. `key`, `text` and the mouse commands do not expect a change;
they answer with the state read 0.1 s after the input. Input commands from
different clients take turns.

`wait_until` and `run_until` report `start_date`, `reached` and
`reached_date`, `game_days`, `wall_ms` (from the start of the wait until the
target date was first seen), `days_per_second`, `paused_ms` (time seen paused
while waiting, e.g. an event that pauses the game; with `paused_date`, the
date of the last pause), `first_change_ms`, `inputs` (input commands any
client sent during the wait; `run_until` does not count its own presses
before and after it) and `game_calls` (console commands any client ran during
the wait), so a measurement can reject a run with either above 0;
`run_until` also `start_speed` and `overshoot_hours` (game hours past the
target when the pause took hold, usually 0). Both wait at most 1 hour by
default (`timeout=` up to 14 400 s). `max_pause=S` ends the wait with an error
once the game has stayed paused for `S` seconds, e.g. on an event with a
choice, instead of waiting for the timeout.

The mouse commands also make the game's interface see the pointer at the given
point (a hook on the game's cursor query), until `release` or until the real
mouse moves; the real cursor itself does not move.

```powershell
.\scripts\tongs.ps1 state
# {"ok":true,"in_game":true,"date":"1719.3.17","date_raw":58860240,"hour":0,"speed":2,"speed_internal":1,"paused":true,"paused_flag":true,"pid":21384,"process_start_ms":1792418572041,"game_calls_total":0}
.\scripts\tongs.ps1 'run_until 1719.6.1 speed=5'
# {"ok":true,"command":"run_until","target":"1719.6.1","start_date":"1719.3.17","reached":true,"reached_date":"1719.6.1",
#  "game_days":76.000,"days_per_second":12.352,"wall_ms":6153,"paused_ms":0,...,"overshoot_hours":0,...,"paused":true,...}
.\scripts\tongs.ps1 'click 3350 32' screenshot
```

Right after a save loads, the game handles input only seconds later; the
commands wait for it. While the Esc menu is open, Space does not resume the
game.

## Widgets

`ui find` and `ui tree` read the game's interface: windows, buttons, icons,
text, list boxes, check boxes and edit boxes, by their names in the game's
`.gui` files. A selector is a path of names:

- `speed_controls/button_speedup`: a child of a top-level window;
  `EventWindow//event_option_button`: anywhere below it; `//DateText`:
  anywhere.
- Names ignore case and take `*` and `?`.
- Filters in brackets: `[2]` (the second match, from 1), `[type=button]`
  (`window`, `button`, `icon`, `text`, `listbox`, `checkbox`,
  `editbox`, `other`), `[text=...]`, `[text~=...]` (contains),
  `[text^=...]` (starts with), `[enabled]`, `[disabled]`, `[checked]`,
  `[unchecked]`. A step made only of filters means any name:
  `speed_controls/[type=button]`.
- Hidden widgets are left out unless the command ends with `all`.

Every widget in an answer has a `path` that finds it again, with `[n]`
where siblings share a name. `text` is what the game displays, in UTF-8
without its colour codes, so text filters depend on the game's language;
prefer names and `[n]`. Edit boxes have their `text` too, and check boxes
`checked`. `click_point` is the centre of `rect` and can be passed to `click`.
Window sizes come from the `.gui` files and may not match the drawn panel
(larger, or 0 for some shield buttons); positions are right. List boxes
create widgets only for the rows they show and refill them as they scroll,
so a row's path stays while its text changes.

`ui click SEL` resolves the selector, moves the pointer to the centre of the
widget, checks that the game sees that widget (or one inside it) under the
pointer, and only then clicks. It refuses without clicking: `not_found`,
`ambiguous` (with up to 10 `matches`), `not_visible` (only hidden widgets
match), `disabled`, or `covered` (with `covered_by`, the widget that would get
the click). The answer has `clicked`, `point` and the state after the click.
When the game reports no widget at all under the pointer, the move is tried up
to three times (`hover_attempts`). A click on an event's option button also
answers `event_option` and counts as a choice Tongs knows (see "Events").

```powershell
.\scripts\tongs.ps1 'ui find speed_controls/button_speedup'
# {"ok":true,"command":"ui find","selector":"speed_controls/button_speedup","count":1,"truncated":false,
#  "matches":[{"path":"speed_controls/button_speedup","type":"button","visible":true,
#  "rect":{"x":3335,"y":18,"w":30,"h":30},"click_point":{"x":3350,"y":33},"enabled":true}],...}
.\scripts\tongs.ps1 'ui tree depth=0'
```

## Events

`event list` reports each open event window, topmost (newest) first, with its
`number`, `id` (the script id, e.g. `flavor_rus.1`), `title`, `description`,
widget `path`, and its options: `number` on screen (from 1), `text`,
`enabled`, `option_index` (the option in the event file; options hidden by
their trigger are skipped on screen but keep their index), `option_key` (the
option's `name`, which may be empty or repeated) and `path`.

`event choose` clicks an option, as a player would: by number
(`event choose 2`) or by text (`event choose text^="Accept"`, also `text=` and
`text~=`). With several events open it needs `event=N` or
`event=title^="..."`, and refuses `ambiguous` otherwise. The game keeps the
options disabled for half a second after the window opens; meanwhile
`event choose` refuses `disabled`. An option outside the event's list box
(the box shows six rows) is scrolled in with the list's scroll bar buttons
first (`scrolled`, the number of clicks). The answer has `choice_date` (the game date
of the click), the `event` and `option`, `closed` (the event left the game's
lists within 3 s; it took 3 frames in the live check) and the state after.

`run_until DATE events=first` does the same for every event that opens during
the run: it waits for the first enabled option of the topmost event, clicks it
and lists the choices in `events`; when no option becomes enabled, the run
ends with `no_enabled_option` and the event. An event that paused the game is
resumed after the choice. Every choice counts in `inputs`.

## Notifications

`subscribe KINDS [date=day|month|year]` turns the connection into a stream:
Tongs sends a line whenever something changes, without being asked. `KINDS`
is `all` or a list such as `state,date,event`:

| Kind | Sent when |
| --- | --- |
| `state` | the game is entered or left (`change`: `entered_game`, `left_game` with `reason`), paused or resumed (`paused`, `resumed`) or the speed changes (`speed`, with `previous_speed`); each with `speed`, `paused`, `paused_flag`. Also while a save loads |
| `date` | a new day, month (the default) or year begins, with `unit` and `previous` |
| `event` | an event window opens (`opened`, with `id`, `title` and `options`) or closes (`closed`, with `chosen`: the option when Tongs chose it with `event choose`, `run_until events=first` or `ui click` on the option button (`via: "ui click"`); `null` for the player's own mouse and the game's choice at the deadline) |
| `error_log` | the game writes a `line` to its `error.log`; `restarted` when the file was replaced |
| `activity` | any client sends an input command (`what: "input"`, `command`) or a game call (`what: "game_call"`, `line`, `name`), with the `client` (pipe instance) and the running count (`inputs`, `game_calls`), so a marked run shows as it happens |

```powershell
.\scripts\tongs.ps1 -Subscribe 'state,date,activity date=day' -Seconds 30
# {"ok":true,"command":"subscribe","kinds":["state","date","activity"],"date_unit":"day","queue":1000,"in_game":true,"date":"1719.3.17",...}
# {"notification":"state","seq":1,"date":"1719.3.17","wall_ms":2210,"change":"resumed","speed":3,"paused":false,"paused_flag":false}
# {"notification":"date","seq":2,"date":"1719.3.18","wall_ms":2304,"unit":"day","previous":"1719.3.17"}
```

- Every notification has `notification` (its kind), `seq` (from 1 on this
  connection, without gaps), the game `date` (`null` outside a game) and
  `wall_ms` since the subscription. Answers always have `command`, so the two
  never mix up.
- The `subscribe` answer lists the `kinds`, kinds the plugin cannot serve as
  `unavailable`, for `error_log` the file it follows, and the current state,
  which the first notifications change. Only changes after the subscription
  are reported.
- While subscribed, the connection answers `ping` and `unsubscribe` and
  refuses everything else (`subscribed`). `unsubscribe` answers with `sent`,
  `dropped` and `discarded` (still queued, not sent) and the connection takes
  ordinary requests again.
- A client that reads slowly does not hold anyone up: each subscription keeps
  up to 1000 notifications, and when that is full the oldest are dropped and
  `{"notification":"overflow","dropped":N}` comes before the next one.
- A subscription holds one of the 8 pipe instances, so use a connection of its
  own for it.

## Console

`console LINE` runs a console command the way the game's console does, but
without the console window: the line is split on spaces and passed to the
game's command dispatcher on its main thread, after a frame of the main loop.
It needs `game_calls = true` and a loaded game (`in_game: true`), and takes
turns with the input commands.

```powershell
.\scripts\tongs.ps1 'console echo hello' 'console speed 3'
# {"ok":true,"command":"console","line":"echo hello","name":"echo","console":{...},"text":"hello","success_hint":true,"game_calls_total":1,...}
```

The answer has `name` (the command the game found, also for an alias),
`text` (the game's answer, UTF-8 without colour codes), `success_hint` (the
game's success byte; some commands never set it, so check the state instead),
`game_calls_total`, `queued_ms`, `run_ms`, `frame` and the fields of `state`
read after the call. Nothing is added to the console window, its history or
`console_history.txt`; Tongs writes each call to the Anvil log.

Tongs refuses, without calling the game:

- `refused`: `enable_all_commands`, `debug_achievements_clear`,
  `test_achievement`, `debug_achievements`, `social_*` and
  `CrashReporter.UploadCrash`, always; the other dangerous commands (crashes,
  memory leaks, desyncs, rewriting the map or the world, install and save
  rules, `run_commands` and `runyear`) unless the config lists them:
  `console_allow = ["leak"]`;
- `ironman`, `multiplayer`: every game call in an ironman or multiplayer game,
  unless `game_calls_ironman = true` or `game_calls_multiplayer = true`; with
  either switch on, answers carry `mode`;
- `game_calls_disabled`, `not_in_game`, `unknown_command`, `bad_command`
  (printable ASCII only), `console_unavailable`;
- `game_calls_tripped`: a game call that faulted answers `faulted`, and every
  later one is refused until the game exits. Reads keep working.

The game's own checks still apply: developer-only commands answer "Command
available only for developers." in `text`.

## Clients

- `scripts/tongs.ps1 CMD [CMD ...] [-AsObject] [-StopOnError] [-Subscribe KINDS [-Seconds S] [-Count N]]`:
  sends the commands over one connection and prints one answer line per
  command; with `-Subscribe`, then prints notifications for `-Seconds` (60 by
  default) or until `-Count` of them, and unsubscribes.
- `scripts/tongs.py` (Python 3, standard library): the same from the command
  line (`python tongs.py pause "speed 5"`), or as a module:

  ```python
  from tongs import Tongs
  with Tongs() as game:
      result = game.call("run_until 1720.1.1 speed=5", check=True)
      print(result["days_per_second"])

  with Tongs() as watch:  # notifications on a connection of their own
      watch.subscribe("state,event,activity")
      for note in watch.notifications(seconds=30):
          print(note["notification"], note.get("change"))
      watch.unsubscribe()
  ```

  From the command line, `python tongs.py "subscribe all" --seconds 30`
  prints the stream (`subscribe` comes last).
- [`mcp/`](mcp/README.md): an MCP server (stdio) with one tool per command
  family, for agents such as Claude Code:
  `claude mcp add --transport stdio tongs -- uv run --frozen --directory C:\path\to\tongs\mcp tongs-mcp`.

## Safety

- Off by default. The pipe opens only with `enabled = true` in
  `anvil/data/tongs/config.toml` (created on the first start), accepts
  keyboard and mouse input only with `input = true` as well, and runs console
  commands only with `game_calls = true`.
- Local clients of the Windows user running the game only (pipe DACL, remote
  clients rejected). No network listener.
- **Game state can change** with `game_calls = true`: `console` runs the
  game's own console commands, which change the simulation. Tongs refuses some
  commands always and others unless allowed (see "Console"), refuses every
  game call in ironman and multiplayer by default, logs every game call and
  refusal with the command, the pipe instance and the game date, and counts
  them (`game_calls`, `game_calls_total`) so a measurement can reject a run.
  The decisions are in [ADR 0001](docs/adr/0001-game-calls-in-tongs.md).
- Without `game_calls`, game state is never written. Keys, text and mouse
  buttons go into the game's SDL event queue, as a player's would; widget
  clicks and event choices are mouse clicks too.
- Patches: with `enabled = true` the plugin hooks the game's frame function
  to run main-thread reads such as `frames`, `ui` and `event list` after a frame of the main loop
  (the frame itself runs unchanged). With `input = true` it also hooks the
  interface's cursor query to answer with the pointer set by the mouse
  commands. `affects_gameplay = true`, because game calls can change the
  simulation, even though they are off by default.
- Screenshots are written only to the plugin's data folder.
- Notifications only read: the state, the event list (on the main thread,
  like `event list`) and the game's `error.log`, which is opened for each
  read with every sharing mode, so the game can still write, move or delete
  it.

## Install / uninstall

Install [Anvil](https://github.com/happy-dwarf-team/anvil#install--uninstall)
0.2.0 or newer first. Then download `tongs-windows-x64.zip` from the
[latest release](https://github.com/happy-dwarf-team/tongs/releases/latest)
and extract it into `<EU4>/anvil/plugins/`; it contains the `tongs` folder
(`SHA256SUMS.txt` next to the archive holds its checksum). Start the game once
to create `anvil/data/tongs/config.toml`, then set `enabled = true` (and
`input = true` for the input commands, `game_calls = true` for `console`) and
start the game again. Delete the
`tongs` folder to uninstall.

## Building

Visual Studio 2022 (or Build Tools) with the C++ workload, CMake ≥ 3.25.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --component plugin --prefix dist
```

Contributions, the commit convention and the release process: see
[CONTRIBUTING.md](CONTRIBUTING.md).

## License

MIT — see [LICENSE](LICENSE). Unofficial community project, not affiliated with
Paradox Interactive.
