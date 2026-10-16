# EU4 Lab Control — design

Status: draft 1 (2026-10-16), layer 1 (read-only state) implemented.

## Goal

Let a test agent control and observe a running game through commands with a
verifiable answer, instead of screenshots and pixel coordinates. Every command
returns the state read from the game after it ran.

Layers, cheapest first:

1. **State (0.1.0):** date, speed, pause. Read-only.
2. **Input:** inject keyboard/mouse events into the game's own SDL event queue
   (`SDL_PushEvent` is linked statically into `eu4.exe`), so shortcuts work
   without focusing the window. Commands such as `pause`, `resume`, `speed N`,
   `key`, each answered with the state after the action.
3. **Widgets by name:** find GUI elements by their `.gui` names, click them and
   read their text. Only if layer 2 is not enough.

Actions go through the same path as a player's input, never by writing game
state directly, so the game behaves as it would for a human player.

## Transport

- Named pipe `\\.\pipe\eu4-lab-control`, one instance, created with
  `FILE_FLAG_FIRST_PIPE_INSTANCE` (a second game cannot take it over and logs
  an error instead), `PIPE_REJECT_REMOTE_CLIENTS`, and a DACL that allows only
  the user running the game and SYSTEM.
- One client at a time; requests are lines, answers are single-line JSON.
- Off unless `enabled = true` in `anvil/data/eu4-lab-control/config.toml`.

## State reading (EU4 1.37.5)

Found with Ghidra and confirmed on a running game in the lab repository
(`docs/reverse-engineering.md`, "Stan pauzy"). RVAs relative to `eu4.exe`.

| Value | Where |
| --- | --- |
| root object | pointer at `0x233FE78` |
| current date | `int32` at `root + 0x1DD0` |
| speed | `int32` at `root + 0x1E08`, 0–4 (UI shows +1); keeps its value while paused |
| session | pointer at `root + 0x1E00`, vtable `0x1CB8460`; slot `0x100` (`0x8024D0`) returns `session + 0x418` |
| game-speed object | pointer at `session + 0x418` |
| player pause | byte at `game_speed + 0x48` |
| is paused | `0x7E01B0(game_speed)`: the pause byte, or in single player a non-empty list at `+0x00` |

The date is the number of hours since 1 January of year −5000 in 365-day years
(no leap days): `0x14C7890` formats it and `0xCFD70` computes the year as
`floor((raw − 43 800 000) / 24 / 365)`. `src/game_date.cpp` decodes it; the top
bar refreshes `DateText` whenever the value changes.

`plugin.toml` declares the code that relies on these offsets as `[[expect]]`
entries, so Anvil skips the plugin when any of them differs.

### Threading

The pipe thread reads game memory while the main thread runs the game. Reads
are word-sized and guarded by SEH; while a save is loading or the game is
quitting, objects may be in transition and `state` answers `in_game: false`
or a read error rather than crashing. Calling `0x7E01B0` from the pipe thread
is read-only (it reads the pause byte and the session's multiplayer state).
Layer 2 commands will run on the game's main thread instead.

## Live check (2026-10-16)

EU4 1.37.5 (original `eu4.exe`), Anvil 0.2.0, this plugin 0.1.0, Russia save
of 17 March 1719 loaded with `continue=`; the lab's `anvil-play.ps1 -Scenario
control`, `state` polled once a second:

- Anvil: all 5 `[[expect]]` ranges matched, plugin loaded with 0 patches, pipe
  served 43 ms after Anvil started.
- During loading: `in_game: false`, first "session is null", then for a few
  seconds "session object has an unexpected vtable" (another class sits at
  `root + 0x1E00` while loading).
- Loaded: `date_raw` 58 860 240 = `1719.3.17`; the game paused itself about a
  second after the first `in_game: true` answer.
- Unpaused at UI speed 4: the date advanced ~5 days per poll (24 per day), the
  month changed correctly (28 March → 1 April), `paused` and `paused_flag`
  switched together; after pausing the date stopped. Speed changes while
  paused were reported as well.

## Not verified yet

- `paused` for pauses not caused by the pause button (event popups that pause,
  menus, multiplayer); `paused_flag` alone was confirmed for the button.
- Behaviour in the main menu and during save loading beyond "not in game".
