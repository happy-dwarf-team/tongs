# Tongs

[![build](https://github.com/happy-dwarf-team/tongs/actions/workflows/build.yml/badge.svg?branch=main)](https://github.com/happy-dwarf-team/tongs/actions/workflows/build.yml)
[![Latest release](https://img.shields.io/github/v/release/happy-dwarf-team/tongs)](https://github.com/happy-dwarf-team/tongs/releases/latest)
[![Steam Workshop](https://img.shields.io/badge/Steam%20Workshop-Tongs-1b2838?logo=steam)](https://steamcommunity.com/sharedfiles/filedetails/?id=3826695093)
[![License: MIT](https://img.shields.io/github/license/happy-dwarf-team/tongs)](LICENSE)

**Control Europa Universalis IV from scripts and agents.** An
[Anvil](https://github.com/happy-dwarf-team/anvil) plugin that lets test agents
and scripts talk to a running game through a local named pipe, instead of
reading pixels from screenshots. Like a smith's tongs, it holds the work from
outside the forge.

**Install guide:
[anvil.happydwarf.dev/plugins/tongs](https://anvil.happydwarf.dev/plugins/tongs/).**

> Status: early. It reads game state, the interface's widget tree and the open
> events, takes screenshots and, when you allow it, uses the keyboard and mouse
> in the game (pause, speed, run to a date, keys, text, clicks on points or on
> widgets by name, event choices) through the game's own SDL input, so the
> window does not need focus. With `game_calls = true` it also runs console
> commands, which change game state; without it, it never does. A connection
> can also subscribe to notifications: state and date changes, events opening
> and closing, new `error.log` lines and other clients' input and game calls.
> Other Anvil plugins can read the widget tree through the service `tongs.ui`
> and select a province through `tongs.navigation`
> (see [For other plugins](#for-other-plugins-tongsui)).
> Supported game build: EU4 1.37.5
> (Steam, Windows 64-bit); Anvil skips it on other builds. It needs Anvil
> 1.0 or newer with its standard plugin `anvil-game`. From 0.16.0 it is also
> published as a Steam Workshop mod for Anvil 1.3 or newer (see
> [Steam Workshop](#steam-workshop)). Design and the
> addresses it relies on: [docs/design.md](docs/design.md) and
> [plugin/symbols.toml](plugin/symbols.toml).

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
| Game state | `ping`, `state`, `window`, `threads`, `frames`, `screenshot`, `help` | `enabled` |
| Interface and events | `ui find`, `ui tree`, `event list` | `enabled` |
| World reads | `province`, `country`, `player`, `selection` | `enabled` |
| Waiting and notifications | `wait_until`, `cancel`, `subscribe`, `unsubscribe` | `enabled` |
| Keyboard and mouse | `pause`, `resume`, `speed`, `run_until`, `key`, `text`, `move`, `click`, `scroll`, `release`, `ui click`, `event choose` | `input` |
| Navigation (client calls) | `province open`, `province goto` | `input` |
| Game calls | `console`, `save load` (**change game state**) | `game_calls` |

Answers that carry the state name the game process (`pid`,
`process_start_ms`) and the `session` (1 for the first game, +1 at every
load). Waits count the input, client calls and game calls of every client
(`inputs`, `client_calls`, `game_calls`), so a measurement can reject a
touched run.

## Clients

- `scripts/tongs.ps1 CMD [CMD ...] [-AsObject] [-StopOnError] [-Subscribe KINDS [-Seconds S] [-Count N]]`:
  sends the commands over one connection and prints one answer line per
  command; with `-Subscribe`, then prints notifications for `-Seconds` (60 by
  default) or until `-Count` of them, and unsubscribes.
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

  Every call has a read timeout (30 s, or the command's own `timeout=` and a
  margin). A wait that times out or is interrupted with Ctrl+C is ended with
  `cancel` on a second connection, and the error says whether its answer still
  came (`answered_late`) or not (`unknown`). With `expect_pid` /
  `expect_start_ms`, an answer from another game process raises
  `ProcessMismatch`. The typed steps (`state`, `wait_until`, `run_until`,
  `ready`, the world reads `province`, `country`, `player`, `selection`, and
  the navigation `open_province`, `goto_province`, and `load_save`)
  return the plugin's answer unchanged and raise on `"ok": false`.
  From the command line, `python tongs.py "subscribe all" --seconds 30`
  prints the stream (`subscribe` comes last), and
  `python tongs.py --ready --session 1 --speed 5` waits for a started game.
- [`mcp/`](mcp/README.md): an MCP server (stdio) with one tool per command
  family, for agents such as Claude Code:
  `claude mcp add --transport stdio tongs -- uv run --frozen --directory C:\path\to\tongs\mcp tongs-mcp`.

## For other plugins: `tongs.ui`

Another Anvil plugin in the game's process can read the widget tree without
the pipe, through the service `tongs.ui` version 1: `find` (a selector, as
`ui find`) and `hovered` (the widget under the game's pointer and its
ancestors). Its header is [include/tongs/tongs_ui.h](include/tongs/tongs_ui.h),
also in the release zip (`tongs/include/`); vendor it, declare a dependency on
`tongs` and call it on the game's main thread, from `anvil-game.frame`. The
service only reads, and Tongs registers it on every load, also with
`enabled = false` (no pipe then). Design:
[docs/design.md](docs/design.md#the-tongsui-service), decision:
[ADR 0002](docs/adr/0002-widget-tree-as-in-process-service.md).

From 0.24.0 the service `tongs.navigation` version 1 gives other plugins the
province selection: `province_view` (the selected province, the one the
province view shows, and whether the view, the colonisation panel or a view of
the session is on screen) and `open_province`, the client call of
`province open` with its refusals, also with `enabled = false` and
`input = false`. It never makes a game call: the case that would make the
player discover a capital is refused. Header:
[include/tongs/tongs_navigation.h](include/tongs/tongs_navigation.h); design:
[docs/design.md](docs/design.md#the-tongsnavigation-service-0240), decision:
[ADR 0004](docs/adr/0004-navigation-as-in-process-service.md).

## Safety

- Off by default. The pipe opens only with `enabled = true` in
  `anvil/data/tongs/settings.toml`, accepts keyboard and mouse input and
  navigation (camera and province views) only with `input = true` as well, and runs console commands only with
  `game_calls = true` (see "Settings").
- Local clients of the Windows user running the game only (pipe DACL, remote
  clients rejected). No network listener.
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
- Screenshots are written only to the plugin's data folder.
- Notifications only read: the state, the event list (on the main thread,
  like `event list`) and the game's `error.log`, which is opened for each
  read with every sharing mode, so the game can still write, move or delete
  it.

## Install / uninstall

Install [Anvil](https://github.com/happy-dwarf-team/anvil#install--uninstall)
1.0.0 or newer first, with its standard plugin `anvil-game` (part of Anvil's
package). Then download `tongs-windows-x64.zip` from the
[latest release](https://github.com/happy-dwarf-team/tongs/releases/latest)
and extract it into `<EU4>/anvil/plugins/`; it contains the `tongs` folder
(`SHA256SUMS.txt` next to the archive holds its checksum). Turn it on in
`anvil/data/tongs/settings.toml` (see "Settings") and start the game. Delete
the `tongs` folder to uninstall.

With Anvil 1.3 or newer, Tongs can also come from its Steam Workshop mod
(see [Steam Workshop](#steam-workshop)): Anvil loads it from the enabled mod
once you approve that version in its panel or in Anvil Manager.

## Settings

`plugin/settings.toml` (in the plugin folder) declares the switches and their
defaults, all off. Your values go into `anvil/data/tongs/settings.toml`, one
`name = value` per line, only those that differ from the default:

```toml
enabled = true      # open the pipe
input = true        # keyboard, mouse and navigation commands
game_calls = true   # console commands (they change game state)
console_allow = "leak"          # danger commands game calls may run, separated by commas
game_calls_ironman = false      # game calls in ironman (Anvil must also allow Tongs there)
game_calls_multiplayer = false  # game calls in multiplayer (likewise)
```

Tongs reads them when the game starts. `ANVIL_TONGS_<NAME>` environment
variables (e.g. `ANVIL_TONGS_ENABLED=true`) override a value for one start,
without changing the file. An `anvil/data/tongs/config.toml` from Tongs 0.12
or older is moved into `settings.toml` on the first start (and renamed to
`config.toml.migrated`); its values apply already in that start.

## Building

Visual Studio 2022 (or Build Tools) with the C++ workload, CMake ≥ 3.25, Git.
Tongs builds with the [Anvil SDK](https://github.com/happy-dwarf-team/anvil/blob/main/sdk/README.md)
(`anvil_add_plugin`, `anvil gen`, `anvil check`). The first configure downloads
the SDK archive of the Anvil release that `cmake/AnvilSdk.cmake` pins (1.0.0,
checked against its SHA-256) into `build/_deps/anvil_sdk-src`;
`-DTONGS_ANVIL_SDK=<sdk>` uses an unpacked SDK instead, for example one
installed from a local Anvil checkout (`cmake --install <anvil build> --component sdk`).

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
cmake --install build --config Release --component plugin --prefix dist
```

The plugin folder is `build/plugin/tongs/` (`plugin.toml`, `symbols.toml`,
`settings.toml`, `tongs.dll`). `ctest` includes `anvil check plugin`, which
fails when `plugin/symbols.lock` does not match `plugin/symbols.toml`; after a
change of the symbols, verify them against the game and rewrite the lock:

```powershell
cmake -S . -B build -DEU4_EXE="C:/Program Files (x86)/Steam/steamapps/common/Europa Universalis IV/eu4.exe"
cmake --build build --config Release --target tongs-check
```

Contributions, the commit convention and the release process: see
[CONTRIBUTING.md](CONTRIBUTING.md).

## Steam Workshop

Tongs can also be published as a Steam Workshop mod for Anvil 1.3 or newer,
which loads plugins from enabled mods after the player approves each version.
The mod is the `workshop/` folder ([`workshop.toml`](workshop.toml)):
`descriptor.mod`, `thumbnail.png` and `anvil/tongs/`, the plugin folder of a
release, which is not in Git. [`workshop/description.md`](workshop/description.md)
is the item's description: every publish replaces the description on Steam with
it. `checksum = "keep"` makes the publish fail if the mod would change the game's
checksum.

- Every pull request runs a dry run of
  [paradox-workshop-publish](https://github.com/happy-dwarf-team/paradox-workshop-publish)
  (`pdx-ws`) in `build.yml`: the job summary shows the files, the descriptor and
  the rendered description. It needs no secrets.
- [`workshop-publish.yml`](.github/workflows/workshop-publish.yml) runs when a
  release is published. It downloads that release's `tongs-windows-x64.zip`,
  checks its build attestation, unpacks `tongs/` into `workshop/anvil/tongs/`
  and publishes the mod, with the release notes as the change note.
- To update the Workshop page between releases, run it by hand on `main`
  with the latest release:
  `gh workflow run workshop-publish.yml --ref main -f tag=vX.Y.Z`. It publishes
  that release's package again with `main`'s description; Steam keeps the
  mod's version when the files are unchanged. The `steam-workshop` environment
  must allow the branch `main` for this.

Setting it up, once (maintainers):

1. **Install `pdx-ws`** from its
   [releases](https://github.com/happy-dwarf-team/paradox-workshop-publish/releases)
   ([setup guide](https://github.com/happy-dwarf-team/paradox-workshop-publish/blob/main/docs/guide/setup.md)).
2. **Create the Workshop item** on a computer with Steam running and logged in to
   the account that owns EU4 and will own the item. Build the plugin package, put
   its `tongs/` folder into the mod and check the plan first:

   ```powershell
   cmake -S . -B build -G "Visual Studio 17 2022" -A x64
   cmake --build build --config Release
   cmake --install build --config Release --component plugin --prefix dist/package
   Remove-Item workshop/anvil -Recurse -Force -ErrorAction Ignore
   New-Item -ItemType Directory workshop/anvil -Force | Out-Null
   Copy-Item dist/package/tongs workshop/anvil/tongs -Recurse
   pdx-ws plan
   pdx-ws init
   ```

   `pdx-ws init` (client mode, through the Steam client) creates a **private**
   item with the descriptor's name and tags, the preview and the description,
   uploads the mod and writes `remote_file_id` into `workshop/descriptor.mod`.
   Commit that line through a pull request. Make the item public on Steam when it
   is ready: publishing never changes its visibility.
3. **Share the Steam session** with this repository. The session is kept once,
   as the organization secrets `STEAM_SESSION` and `STEAM_USERNAME`, for the
   Happy Dwarf Team repositories that publish to the Workshop (read the
   [security notes](https://github.com/happy-dwarf-team/paradox-workshop-publish/blob/main/docs/guide/security.md)
   and the [organization setup](https://github.com/happy-dwarf-team/paradox-workshop-publish/blob/main/docs/guide/setup.md#several-repositories-in-one-organization)
   first). Add this repository to both secrets, and create a `steam-workshop`
   environment without secrets of its own:

   ```sh
   id=$(gh api repos/happy-dwarf-team/tongs --jq .id)
   gh api --method PUT orgs/happy-dwarf-team/actions/secrets/STEAM_SESSION/repositories/$id
   gh api --method PUT orgs/happy-dwarf-team/actions/secrets/STEAM_USERNAME/repositories/$id
   gh api --method PUT repos/happy-dwarf-team/tongs/environments/steam-workshop
   ```

   In **Settings → Environments → steam-workshop**, add a required reviewer and
   allow only the tags `v*` and the branch `main`: the environment gates every
   publish job.
4. **Turn publishing on:** `gh variable set WORKSHOP_PUBLISH --body true`.

From then on, publishing a release's draft (see
[CONTRIBUTING.md](CONTRIBUTING.md#versions-and-releases)) also publishes the
Workshop mod. When the Steam session expires, the job fails with exit code 3:
export a new one into the organization secret, for every repository that uses
it (Git Bash or PowerShell 7.4+, so the pipe passes the session unchanged):

```sh
pdx-ws session export --user my_steam_name | gh secret set STEAM_SESSION --org happy-dwarf-team --visibility selected --repos bellows,tongs,eu4-anbennar-widescreen-loading-screens
```

## License

MIT — see [LICENSE](LICENSE). Tongs is an unofficial community project, not affiliated
with or endorsed by Paradox Interactive.
