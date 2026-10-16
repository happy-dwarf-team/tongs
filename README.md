# EU4 Lab Control

An [Anvil](https://github.com/happy-dwarf-team/anvil) plugin that lets test
agents and scripts talk to a running **Europa Universalis IV** game through a
local named pipe, instead of reading pixels from screenshots.

> Status: 0.1.0, skeleton. It only **reads** game state; it does not pause,
> click or change anything in the game. Supported game build: EU4 1.37.5
> (Steam, Windows 64-bit); Anvil skips it on other builds. Design and the
> addresses it relies on: [docs/design.md](docs/design.md).

## Commands

The pipe is `\\.\pipe\eu4-lab-control`. A request is one line of text; the
answer is one JSON object on one line.

| Command | Answer |
| --- | --- |
| `ping` | plugin version and game process id |
| `state` | `in_game`; with a loaded game also `date` (`"1719.3.17"`), `date_raw`, `hour`, `speed` (1–5 as in the UI), `speed_internal` (0–4), `paused` (the game's own answer) and `paused_flag` (the player pause byte) |
| `help` | list of commands |

```powershell
.\scripts\eu4ctl.ps1 state
# {"ok":true,"in_game":true,"date":"1719.3.17","date_raw":58860240,"hour":0,"speed":5,"speed_internal":4,"paused":true,"paused_flag":true}
```

## Safety

- Off by default. The pipe opens only with `enabled = true` in
  `anvil/data/eu4-lab-control/config.toml` (created on the first start).
- Local clients of the Windows user running the game only (pipe DACL, remote
  clients rejected). No network listener.
- Read-only: no game code is patched. `affects_gameplay = false`.

## Install / uninstall

Install Anvil 0.2.0 or newer, then copy the `eu4-lab-control` folder produced
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
