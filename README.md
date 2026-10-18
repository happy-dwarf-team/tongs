# Tongs

**Control Europa Universalis IV from scripts and agents.** An
[Anvil](https://github.com/happy-dwarf-team/anvil) plugin that lets test agents
and scripts talk to a running game through a local named pipe, instead of
reading pixels from screenshots. Like a smith's tongs, it holds the work from
outside the forge.

> Status: early. It reads game state, takes screenshots and, when you allow
> it, uses the keyboard and mouse in the game (pause, speed, run to a date,
> keys, text, clicks) through the game's own SDL input, so the window does not
> need focus. It never writes game state. Supported game build: EU4 1.37.5
> (Steam, Windows 64-bit); Anvil skips it on other builds. Design and the
> addresses it relies on: [docs/design.md](docs/design.md).

## Commands

The pipe is `\\.\pipe\tongs`. A request is one line of text; the answer is
one JSON object on one line. Up to 4 clients can be connected at once, so one
can wait in `run_until` while another reads `state` or sends `cancel`.

| Command | Answer |
| --- | --- |
| `ping` | plugin version and game process id |
| `state` | `in_game`; with a loaded game also `date` (`"1719.3.17"`), `date_raw`, `hour`, `speed` (1–5 as in the UI), `speed_internal` (0–4), `paused` (the game's own answer) and `paused_flag` (the player pause byte) |
| `window` | the game window: client `width` / `height`, `screen_x` / `screen_y`, `focused`, `minimized`, `thread_id` (the game's main thread), `dpi` |
| `threads` | worker threads in each of the game's two pools, the `threads` option, process and main-thread CPU masks, ideal CPU and priority of the main thread, number of threads |
| `screenshot [NAME]` | writes a PNG of the game window to `anvil/data/tongs/screenshots/` (keeps the newest 100) and answers with its `path`, size and capture `method`; works while the window is covered |
| `frames [timeout=S]` | read on the game's main thread after a frame of its main loop: frames per second (`outer_per_s`, and `nested_per_s` for frames drawn inside a game update or a save load), counts and the longest gap between main-loop frames since the previous `frames`, and the main thread's `thread_id`; with `queued_ms`, `run_ms` and the game's `frame` counter. Waits up to 10 s (`timeout=` up to 600) for a frame; while a save loads none comes, and the answer is `main thread busy` |
| `wait_until DATE [timeout=S] [max_pause=S]` | waits (without touching the game) until the date reaches `DATE` (`1720.1.1` or `1720.1.1.12`); answers with the timing below |
| `cancel` | ends every `wait_until` / `run_until` in progress and withdraws main-thread jobs still waiting for a frame (`withdrawn`) |
| `help` | list of commands and whether input commands are enabled |

With `input = true` in the config, also:

| Command | What it does |
| --- | --- |
| `pause` | presses Space unless the player pause is already on, then waits until it is |
| `resume` | presses Space if the player pause is on, then waits until the game runs; refuses when something else holds the game paused |
| `speed N` | presses keypad `+` / `-` one step at a time until the speed is `N` (1–5, as in the UI); works while paused |
| `run_until DATE [speed=N] [timeout=S] [max_pause=S]` | sets the speed, resumes, waits for the date and pauses again; for measuring how fast the game runs |
| `key NAME` | presses one key: `a`–`z`, `0`–`9`, `f1`–`f12`, `space`, `return`, `escape`, `tab`, `backspace`, `delete`, `minus`, `equals`, `grave` (the console key on a US layout), `kp_plus`, `kp_minus`, `kp_enter`, arrows (`up`, `down`, `left`, `right`), `home`, `end`, `page_up`, `page_down` |
| `text STRING` | types the rest of the line into the focused text field (characters U+0020–U+007E and U+00A0–U+00FF) |
| `move X Y` | points the mouse at client coordinates `X Y` (hovering shows tooltips) |
| `click X Y [left\|right\|middle] [double]` | moves there and clicks |
| `scroll STEPS [X Y]` | turns the wheel; positive scrolls up (zooms in on the map), negative down |
| `release` | hands the pointer back to the real mouse |

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
date of the last pause) and `first_change_ms`;
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
# {"ok":true,"in_game":true,"date":"1719.3.17","date_raw":58860240,"hour":0,"speed":2,"speed_internal":1,"paused":true,"paused_flag":true}
.\scripts\tongs.ps1 'run_until 1719.6.1 speed=5'
# {"ok":true,"command":"run_until","target":"1719.6.1","start_date":"1719.3.17","reached":true,"reached_date":"1719.6.1",
#  "game_days":76.000,"days_per_second":12.352,"wall_ms":6153,"paused_ms":0,...,"overshoot_hours":0,...,"paused":true,...}
.\scripts\tongs.ps1 'click 3350 32' screenshot
```

Right after a save loads, the game handles input only seconds later; the
commands wait for it. While the Esc menu is open, Space does not resume the
game.

## Clients

- `scripts/tongs.ps1 CMD [CMD ...] [-AsObject] [-StopOnError]`: sends the
  commands over one connection and prints one answer line per command.
- `scripts/tongs.py` (Python 3, standard library): the same from the command
  line (`python tongs.py pause "speed 5"`), or as a module:

  ```python
  from tongs import Tongs
  with Tongs() as game:
      result = game.call("run_until 1720.1.1 speed=5", check=True)
      print(result["days_per_second"])
  ```

## Safety

- Off by default. The pipe opens only with `enabled = true` in
  `anvil/data/tongs/config.toml` (created on the first start), and accepts
  keyboard and mouse input only with `input = true` as well.
- Local clients of the Windows user running the game only (pipe DACL, remote
  clients rejected). No network listener.
- Game state is never written. Keys, text and mouse buttons go into the
  game's SDL event queue, as a player's would.
- Patches: with `enabled = true` the plugin hooks the game's frame function
  to run main-thread reads such as `frames` after a frame of the main loop
  (the frame itself runs unchanged). With `input = true` it also hooks the
  interface's cursor query to answer with the pointer set by the mouse
  commands. `affects_gameplay = false`.
- Screenshots are written only to the plugin's data folder.

## Install / uninstall

Install [Anvil](https://github.com/happy-dwarf-team/anvil#install--uninstall)
0.2.0 or newer first. Then download `tongs-windows-x64.zip` from the
[latest release](https://github.com/happy-dwarf-team/tongs/releases/latest)
and extract it into `<EU4>/anvil/plugins/`; it contains the `tongs` folder
(`SHA256SUMS.txt` next to the archive holds its checksum). Start the game once
to create `anvil/data/tongs/config.toml`, then set `enabled = true` (and
`input = true` for the input commands) and start the game again. Delete the
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
