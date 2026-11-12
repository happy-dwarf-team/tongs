# Tongs

[![build](https://github.com/happy-dwarf-team/tongs/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/happy-dwarf-team/tongs/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/happy-dwarf-team/tongs)](https://github.com/happy-dwarf-team/tongs/releases/latest)
[![Steam Workshop](https://img.shields.io/badge/Steam%20Workshop-Tongs-1b2838?logo=steam)](https://steamcommunity.com/sharedfiles/filedetails/?id=3826695093)
[![License: MIT](https://img.shields.io/github/license/happy-dwarf-team/tongs)](LICENSE)

**Control Europa Universalis IV from scripts and agents.** Tongs is an
[Anvil](https://github.com/happy-dwarf-team/anvil) plugin. Test agents and
scripts talk to a running game through a local named pipe, instead of reading
pixels from screenshots. Like a smith's tongs, it holds the work from outside
the forge.

**Install guide:
[anvil.happydwarf.dev/plugins/tongs](https://anvil.happydwarf.dev/plugins/tongs/).**

Status: early. Everything is off until you turn it on (see [Safety](#safety)).

- **Reads:** game state, the interface's widget tree, open events, provinces
  and countries, and screenshots.
- **Keyboard and mouse,** with `input = true`: pause, speed, run to a date,
  keys, text, clicks on points or on widgets by name, and event choices. They
  go through the game's own SDL input, so the window does not need focus.
- **Console commands,** with `game_calls = true`. These change game state.
  Without that setting Tongs never does.
- **Notifications:** a connection can subscribe to state and date changes,
  events opening and closing, new `error.log` lines and other clients' input
  and game calls.
- **Other plugins** can read the widget tree and select a province (see
  [For other plugins](#for-other-plugins-tongsui)).

It needs EU4 1.37.5 (Steam, Windows 64-bit) or 1.37.5.2 or 1.37.5.0 (GOG, Windows 64-bit),
and Anvil 1.7 or newer (Anvil 1.15 on 1.37.5.0) with its standard plugin `anvil-game`. Anvil skips Tongs
on other builds. Design and the addresses it relies on:
[docs/design.md](docs/design.md) and [plugin/symbols.toml](plugin/symbols.toml).

## Quick start

1. Install Tongs (see [Install / uninstall](#install--uninstall)) and turn it
   on in `anvil/data/tongs/settings.toml`: `enabled = true`, and
   `input = true` for the keyboard, mouse and navigation commands (see
   [Settings](#settings)).
2. Start the game, load a save and send commands with one of the
   [clients](#clients):

   ```powershell
   .\scripts\tongs.ps1 state
   # {"ok":true,"in_game":true,"date":"1719.3.17","speed":2,"paused":true,...}
   .\scripts\tongs.ps1 'run_until 1719.6.1 speed=5' screenshot
   ```

## Commands

The pipe is `\\.\pipe\tongs`: a request is one line of text, the answer one
JSON object on one line, and up to 8 clients can be connected at once. Every
command, its answer and its options are in
**[docs/commands.md](docs/commands.md)**.

| What | Commands | Needs |
| --- | --- | --- |
| Game state | `ping`, `state`, `window`, `threads`, `frames`, `screenshot`, `journal`, `help` | `enabled` |
| Interface and events | `ui find`, `ui tree`, `event list` | `enabled` |
| World reads (with flags and variables from Anvil 1.20, a country's pending events from 1.21) | `province`, `country`, `player`, `selection`, `units`, `cursor` | `enabled` |
| Waiting and notifications | `wait_until`, `cancel`, `subscribe`, `unsubscribe` | `enabled` |
| Keyboard and mouse | `pause`, `resume`, `speed`, `run_until`, `key`, `text`, `move`, `click`, `scroll`, `release`, `ui click`, `event choose`, `unit drill`, `unit split`, `unit merge` | `input` |
| Navigation (client calls) | `province open`, `province goto`, `map_mode` | `input` |
| Game calls | `console`, `save load` (**change game state**) | `game_calls` |

Answers that carry the state name the game process (`pid`,
`process_start_ms`) and the `session` (1 for the first game, +1 at every
load). Waits count the input, client calls and game calls of every client
(`inputs`, `client_calls`, `game_calls`), so a measurement can reject a
touched run.

## Clients

- `scripts/tongs.ps1 CMD [CMD ...] [-AsObject] [-StopOnError] [-Subscribe KINDS [-Seconds S] [-Count N]]`:
  sends the commands over one connection and prints one answer line per
  command. With `-Subscribe` it then prints notifications for `-Seconds` (60 by
  default) or until `-Count` of them.
- `scripts/tongs.py` (Python 3, standard library), the public client: the
  same from the command line (`python tongs.py pause "speed 5"`), or as a
  module:

  ```python
  import tongs
  # After starting the game: no fixed sleep (docs/commands.md, "Sessions and the ready wait").
  tongs.ready(session=1, speed=5, expect_pid=pid, expect_start_ms=start_ms)
  with tongs.Tongs(expect_pid=pid, expect_start_ms=start_ms) as game:
      result = game.run_until("1720.1.1", on_progress=lambda p: print(p.date))
      print(result["days_per_second"], result["inputs"], result["client_calls"], result["game_calls"])
      print(game.province(295)["owner"], game.country("RUS")["province_count"])  # world reads
      game.open_province(295)  # the province view, as the game's own links open it
      loaded = game.load_save("mp_Russia1719_03_17")  # a game call: the running game is discarded
      game.ready(session=loaded["session"])

  with tongs.Tongs() as watch:  # notifications on a connection of their own
      watch.subscribe("state,event,activity")
      for note in watch.notifications(seconds=30):
          print(note["notification"], note.get("change"))
      watch.unsubscribe()
  ```

  Every call has a read timeout: 30 s, or the command's own `timeout=` and a
  margin. A wait that times out or is stopped with Ctrl+C is ended with `cancel`
  on a second connection, and the error says whether its answer still came
  (`answered_late`) or not (`unknown`). With `expect_pid` / `expect_start_ms`, an
  answer from another game process raises `ProcessMismatch`. The typed methods
  return the plugin's answer unchanged and raise on `"ok": false`.

  From the command line, `python tongs.py "subscribe all" --seconds 30` prints
  the stream, and `python tongs.py --ready --session 1 --speed 5` waits for a
  started game.
- [`mcp/`](mcp/README.md): an MCP server (stdio) with one tool per command
  family, for agents such as Claude Code:
  `claude mcp add --transport stdio tongs -- uv run --frozen --directory C:\path\to\tongs\mcp tongs-mcp`.

## For other plugins: `tongs.ui`

Another Anvil plugin in the game's process can use two services, without the
pipe. Both also work with `enabled = false`.

**`tongs.ui` version 1** reads the widget tree: `find` takes a selector, as
`ui find` does, and `hovered` gives the widget under the game's pointer and its
ancestors. It only reads. Call it on the game's main thread, from
`anvil-game.frame`. Header: [include/tongs/tongs_ui.h](include/tongs/tongs_ui.h),
also in the release zip (`tongs/include/`). Vendor it and declare a dependency
on `tongs`. Design: [docs/design.md](docs/design.md#the-tongsui-service),
decision: [ADR 0002](docs/adr/0002-widget-tree-as-in-process-service.md).

**`tongs.navigation` version 1** (from 0.24.0) gives the province selection:
`province_view` says which province is selected, which one the province view
shows, and whether the view, the colonisation panel or a view of the session is
on screen. `open_province` is the client call of `province open`, with its
refusals, also with `input = false`. It never makes a game call: the case that would make the player
discover a capital is refused. Header:
[include/tongs/tongs_navigation.h](include/tongs/tongs_navigation.h), design:
[docs/design.md](docs/design.md#the-tongsnavigation-service-0240), decision:
[ADR 0004](docs/adr/0004-navigation-as-in-process-service.md).

## Safety

- Off by default. The pipe opens only with `enabled = true` in
  `anvil/data/tongs/settings.toml`. Keyboard, mouse and navigation (camera and
  province views) also need `input = true`, and console commands need
  `game_calls = true` (see [Settings](#settings)).
- Local clients of the Windows user running the game only (pipe DACL, remote
  clients rejected). No network listener. The Python client and the MCP server
  also refuse a pipe that another Windows user created under this name.
- Text from the game and its mods reaches a client as it is. An agent should
  treat it as data, not instructions: the MCP server says so in its
  instructions.
- **Game state can change** with `game_calls = true`: `console` runs the
  game's own console commands, which change the simulation. Tongs refuses some
  commands always and others unless allowed (see [Console](docs/commands.md#console)), refuses every
  game call in ironman and multiplayer by default, logs every game call and
  refusal with the command, the pipe instance and the game date, and counts
  them (`game_calls`, `game_calls_total`) so a measurement can reject a run.
  The decisions are in [ADR 0001](docs/adr/0001-game-calls-in-tongs.md).
- Without `game_calls`, game state is never written. Keys, text and mouse
  buttons go into the game's SDL event queue, as a player's would; widget
  clicks and event choices are mouse clicks too.
- Changes to the game: with `enabled = true` main-thread reads such as
  `frames`, `ui` and `event list` and game calls run after a frame of the main
  loop, which Anvil's standard plugin `anvil-game` reports; Tongs adds a mid
  hook that only counts the frames drawn inside a game update. With
  `input = true` it also hooks the interface's cursor query to answer with the
  pointer set by the mouse commands. Every address it uses is declared in
  `plugin/symbols.toml`, which Anvil verifies against `eu4.exe` before it loads
  Tongs.
- **Ironman and multiplayer.** `affects_gameplay = true`, because input and
  game calls can change the simulation, even though they are off by default.
  In an ironman or multiplayer game Anvil suspends Tongs: its hooks are out of
  the game, input commands (except `release`) and game calls answer an error,
  reads go on. The player can allow Tongs there in `anvil/anvil.toml`
  (`allow_ironman = ["tongs"]`, `allow_multiplayer = ["tongs"]`); game calls
  then still need Tongs' own `game_calls_ironman` / `game_calls_multiplayer`.
- Screenshots and the command journal are written only to the plugin's data folder
  (`anvil/data/tongs/screenshots/`, `anvil/data/tongs/journal/`), never into the
  game, its saves or what multiplayer compares.
- Notifications only read: the state, the event list (on the main thread,
  like `event list`) and the game's `error.log`, which is opened for each
  read with every sharing mode, so the game can still write, move or delete
  it.

## Install / uninstall

[Install Anvil](https://anvil.happydwarf.dev/docs/install/) 1.7.0 or newer
first, with its standard plugin `anvil-game` (part of Anvil's package). Then
[install the plugin](https://anvil.happydwarf.dev/docs/install-plugins/) one of
these ways:

- click **Install** next to Tongs on the **Catalog** tab of Anvil Manager (Anvil
  1.9 or newer);
- subscribe to the
  [Steam Workshop mod](https://steamcommunity.com/sharedfiles/filedetails/?id=3826695093)
  and approve it, or
- extract `tongs-windows-x64.zip` from the
  [latest release](https://github.com/happy-dwarf-team/tongs/releases/latest)
  into `<EU4>/anvil/plugins/`. `SHA256SUMS.txt` next to the archive holds its
  checksum.

Turn it on on the Manager's **Settings** tab or in
`anvil/data/tongs/settings.toml`, and start the game. To uninstall, click
**Remove** on the **Plugins** tab, or delete the `tongs` folder.

## Settings

`plugin/settings.toml` (in the plugin folder) declares the switches and their
defaults, all off but the journal. Your values go into `anvil/data/tongs/settings.toml`, one
`name = value` per line, only those that differ from the default:

```toml
enabled = true      # open the pipe
input = true        # keyboard, mouse and navigation commands
game_calls = true   # console commands (they change game state)
console_allow = "leak"          # danger commands game calls may run, separated by commas
game_calls_ironman = false      # game calls in ironman (Anvil must also allow Tongs there)
game_calls_multiplayer = false  # game calls in multiplayer (likewise)
journal = true      # the command journal in anvil/data/tongs/journal/ (on by default)
```

Tongs reads them when the game starts. `ANVIL_TONGS_<NAME>` environment
variables (for example `ANVIL_TONGS_ENABLED=true`) override a value for one
start, without changing the file. An `anvil/data/tongs/config.toml` from Tongs
0.12 or older is moved into `settings.toml` on the first start.

With Anvil 1.20 or newer, the Anvil panel and Anvil Manager show these settings and Tongs' description in the player's
language: French, German, Spanish, Simplified and Traditional Chinese, Japanese, Korean or
Polish, from [`plugin/translations/`](plugin/translations/). The translations are drafts that native speakers
have not checked yet; a correction is welcome in an issue or a pull request.

## Help and bug reports

Questions about Tongs, its clients or the MCP server go to the `#help-and-bugs`
forum on the team's [Discord server](https://discord.happydwarf.dev/) (English;
Tongs tag). Bugs go to an
[issue](https://github.com/happy-dwarf-team/tongs/issues/new/choose) with
`anvil/anvil.log` of that start and the commands that were sent; what the team
confirms on Discord is tracked in an issue too. Security problems: report them
privately, as [SECURITY.md](SECURITY.md) says.

## Building

Visual Studio 2022 (or Build Tools) with the C++ workload, CMake ≥ 3.25, Git.
Tongs builds with the [Anvil SDK](https://github.com/happy-dwarf-team/anvil/blob/main/sdk/README.md)
(`anvil_add_plugin`, `anvil gen`, `anvil check`). The first configure downloads
the SDK archive of the Anvil release that `cmake/AnvilSdk.cmake` pins (1.20.0,
checked against its SHA-256) into `build/_deps/anvil_sdk-src`;
`-DTONGS_ANVIL_SDK=<sdk>` uses an unpacked SDK instead, for example one
installed from a local Anvil checkout (`cmake --install <anvil build> --component sdk`).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release --parallel
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --component plugin --prefix dist
```

The plugin folder is `build/plugin/tongs/` (`plugin.toml`, `symbols.toml`,
`settings.toml`, `translations/`, `tongs.dll`). `ctest` includes `anvil check plugin`, which
fails when `plugin/symbols.lock` does not match `plugin/symbols.toml`; after a
change of the symbols, verify them against the game and rewrite the lock:

```powershell
cmake -S . -B build -DEU4_EXE="C:/Program Files (x86)/Steam/steamapps/common/Europa Universalis IV/eu4.exe"
cmake --build build --config Release --target tongs-check
```

Contributions, the commit convention and the release process: see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Steam Workshop

Tongs is also a Steam Workshop mod. Anvil 1.3 and newer loads plugins from
enabled mods after the player approves each version. The mod is the `workshop/`
folder, and a published release publishes it. How that works, and how to set it
up: [docs/workshop.md](docs/workshop.md).

## License

MIT, see [LICENSE](LICENSE). Tongs is an unofficial community project, not affiliated
with or endorsed by Paradox Interactive.
