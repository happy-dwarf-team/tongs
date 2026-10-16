# Tongs

**Control Europa Universalis IV from scripts and agents.** An
[Anvil](https://github.com/happy-dwarf-team/anvil) plugin that lets test agents
and scripts talk to a running game through a local named pipe, instead of
reading pixels from screenshots. Like a smith's tongs, it holds the work from
outside the forge.

> Status: early. It reads game state and, when you allow it, presses keys in
> the game (pause, resume, speed, any key) through the game's own SDL input,
> so the window does not need focus. It does not click and does not write game
> state. Supported game build: EU4 1.37.5 (Steam, Windows 64-bit); Anvil skips
> it on other builds. Design and the addresses it relies on:
> [docs/design.md](docs/design.md).

## Commands

The pipe is `\\.\pipe\tongs`. A request is one line of text; the
answer is one JSON object on one line.

| Command | Answer |
| --- | --- |
| `ping` | plugin version and game process id |
| `state` | `in_game`; with a loaded game also `date` (`"1719.3.17"`), `date_raw`, `hour`, `speed` (1–5 as in the UI), `speed_internal` (0–4), `paused` (the game's own answer) and `paused_flag` (the player pause byte) |
| `help` | list of commands and whether input commands are enabled |

With `input = true` in the config, also:

| Command | What it does |
| --- | --- |
| `pause` | presses Space unless the player pause is already on, then waits until it is |
| `resume` | presses Space if the player pause is on, then waits until the game runs; refuses when something else holds the game paused |
| `speed N` | presses keypad `+` / `-` one step at a time until the speed is `N` (1–5, as in the UI); works while paused |
| `key NAME` | presses one key: `a`–`z`, `0`–`9`, `f1`–`f12`, `space`, `return`, `escape`, `tab`, `backspace`, `delete`, `minus`, `equals`, `kp_plus`, `kp_minus`, `kp_enter`, arrows (`up`, `down`, `left`, `right`), `home`, `end`, `page_up`, `page_down` |

Each input command answers once the game has reacted (or after 10 s) with
`command`, `presses`, `waited_ms` and the fields of `state`. When the game
does not react in time, `ok` is `false` with an `error`, and the state is
still included. `key` does not expect a change; it answers with the state
read 0.1 s after the press.

```powershell
.\scripts\tongs.ps1 state
# {"ok":true,"in_game":true,"date":"1719.3.17","date_raw":58860240,"hour":0,"speed":2,"speed_internal":1,"paused":true,"paused_flag":true}
.\scripts\tongs.ps1 'speed 5'
# {"ok":true,"command":"speed","presses":3,"waited_ms":78,"in_game":true,"date":"1719.3.17",...,"speed":5,...}
.\scripts\tongs.ps1 resume
# {"ok":true,"command":"resume","presses":1,"waited_ms":16,...,"paused":false,"paused_flag":false}
```

Right after a save loads, the game handles a key press only seconds later;
the commands wait for it. While the Esc menu is open, Space does not resume
the game.

## Safety

- Off by default. The pipe opens only with `enabled = true` in
  `anvil/data/tongs/config.toml` (created on the first start), and accepts
  key presses only with `input = true` as well.
- Local clients of the Windows user running the game only (pipe DACL, remote
  clients rejected). No network listener.
- No game code is patched and no game state is written: key presses go into
  the game's SDL event queue, as a player's would. `affects_gameplay = false`.

## Install / uninstall

Install Anvil 0.2.0 or newer, then copy the `tongs` folder produced
by `cmake --install` into `<EU4>/anvil/plugins/`. Delete it to uninstall.

## Building

Visual Studio 2022 (or Build Tools) with the C++ workload, CMake ≥ 3.25.

```powershell
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
cmake --install build --component plugin --prefix dist
```

## License

MIT — see [LICENSE](LICENSE). Unofficial community project, not affiliated with
Paradox Interactive.
