# Tongs — design

Status: draft 3 (2026-10-17): layers 1 and 2 implemented, plus timing
(`run_until`), worker pools and screenshots for measurements.

## Goal

Let a test agent control and observe a running game through commands with a
verifiable answer, instead of screenshots and pixel coordinates. Every command
returns the state read from the game after it ran.

Layers, cheapest first:

1. **State (0.1.0):** date, speed, pause. Read-only.
2. **Input:** inject keyboard/mouse events into the game's own SDL event queue
   (`SDL_PushEvent` is linked statically into `eu4.exe`), so shortcuts work
   without focusing the window. Commands such as `pause`, `resume`, `speed N`,
   `key`, `text`, `click`, each answered with the state after the action.
   Mouse positions also need the cursor hook (see "Mouse").
3. **Widgets by name:** find GUI elements by their `.gui` names, click them and
   read their text. Only if layer 2 is not enough.

Actions go through the same path as a player's input, never by writing game
state directly, so the game behaves as it would for a human player.

Beside the layers, for measurements: `run_until` / `wait_until` time the
game between two dates, `threads` reads the worker pools and CPU masks the
lab checks before every simulation measurement, `screenshot` captures the
window for agents that need to look.

## Transport

- Named pipe `\\.\pipe\tongs`, 4 instances, each served by its own thread.
  The first is created with `FILE_FLAG_FIRST_PIPE_INSTANCE` (a second game
  cannot take the name over and logs an error instead); all with
  `PIPE_REJECT_REMOTE_CLIENTS` and a DACL that allows only the user running
  the game and SYSTEM.
- Up to 4 clients at once, one per instance; requests are lines, answers are
  single-line JSON. Input commands take turns through one lock, so presses of
  two clients never interleave; `run_until` holds it only while pressing.
- Off unless `enabled = true` in `anvil/data/tongs/config.toml`; the input
  commands also need `input = true`.

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
Input commands also run on the pipe thread: they only queue SDL events
(see "Input"), and the main thread handles them.

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

## Input (EU4 1.37.5, SDL 2.0.4)

Found with `dumpbin /disasm` in the lab repository and checked on a running
game (below). RVAs relative to `eu4.exe`; `plugin.toml` checks the code.

- `eu4.exe` links SDL 2.0.4 statically (source paths
  `SDL2-2.0.4\src\...`) with its dynamic API: each `SDL_*` function is a
  `jmp [slot]` into a table of 553 pointers at `0x1FB0000` (size `0x1148`),
  which `SDL_DYNAPI_entry` (`0x17320F0`) fills in the order of
  `src/dynapi/SDL_dynapi_procs.h`. Slot 80 is `SDL_PeepEvents` (`0x17467A0`,
  the only user of "The event system has been shut down"), slot 88
  `SDL_PushEvent` (`0x1746B90`, slot `0x1FB02C0`, thunk `0x1735420`), slot 444
  `SDL_GetTicks`. The game itself calls `SDL_PushEvent` once, to post
  `SDL_QUIT` (`0x156AC80`).
- `PressKey` checks that the slot points to `0x1746B90` (it does not before
  SDL starts, or with `SDL_DYNAMIC_API` set) and calls it. `SDL_PushEvent`
  locks the event queue, so the pipe thread may call it. The game's code calls
  none of the event filter and watch functions (slots 89–92); a filter
  would run on the pipe thread.
- The game's event loop `0x1569C70` polls with `SDL_PollEvent` and switches on
  the type. `SDL_KEYDOWN` (`0x156A043`) reads `keysym.sym` (+0x14) and
  `keysym.scancode` (+0x10), maps keypad Enter to Return and passes the key to
  the GUI; `SDL_TEXTINPUT` (`0x303`) decodes the first UTF-8 character and
  passes it on separately. `windowID` is not read.
- A key press is queued as a player's would be: `SDL_KEYDOWN`, then
  `SDL_TEXTINPUT` with its character for keys that have one (no modifiers,
  US layout), then `SDL_KEYUP`. `mod` is 0.
- Keys: pause and resume press Space, speed presses keypad `+` / `-`
  (`KP_PLUS` / `KP_MINUS` are names in the game's key table in
  `keyevent.cpp`). No `.gui` file binds the speed buttons; the bindings are in
  the game's code.

`pause` waits for the player pause byte, `resume` for the game's own "is
paused" answer to become false. `resume` refuses without pressing anything
when the game is paused but the player pause is off: Space would add a
player pause. `speed N` presses one key at a time and waits for each step.

### Live check (2026-10-16)

Original `eu4.exe` (`9AD3EFE1…771A`), Anvil 0.2.0, this branch, Russia save
of 17 March 1719 loaded with `continue=`; the lab's `anvil-play.ps1 -Scenario
tongs -TongsInput`, three sessions (`tongs-input-1` … `-3`). Anvil: all 9
`[[expect]]` ranges matched. Each session ended by the script after checking
PID, path and start time; save unchanged, `settings.txt` differed only in
`graceful_exit` and was restored.

- Space as `SDL_KEYDOWN`/`SDL_KEYUP` alone pauses and resumes; `pause` and
  `resume` reacted within 15–50 ms, `waited_ms` included.
- Speed: `-` sent as `SDL_KEYDOWN`/`SDL_KEYUP` alone did nothing while the
  game ran; with `SDL_TEXTINPUT`, keypad `+` / `-` and `-` change the speed,
  `=` does not. Keypad keys without `SDL_TEXTINPUT` were only tried right
  after loading (see below), so that case is open. `speed 5`, `speed 1`, `speed 3` worked
  paused and running (2–4 presses, 78–281 ms); the pause stayed on.
- With Notepad in the foreground (the game window without focus), `resume`,
  `speed 2` and `pause` worked the same.
- Right after the save loaded (first `in_game: true` with `paused: true`), a
  speed key was handled between 2 and 7 s later, not dropped: the first
  `speed 3` timed out at 2 s, and 5 s later the speed was 3. Key presses sent
  then stack up, so commands wait up to 10 s.
- Esc opened the game menu, which set the player pause; with the menu open,
  Space did not resume (timeout), and a second Esc left the pause on. After
  that, `resume` worked.
- After `pause`, the date may still advance by one day (the day in progress);
  then it stays.

## Run timing

`wait_until` polls the state every 10 ms (`std::chrono::steady_clock`, i.e.
QueryPerformanceCounter; `GetTickCount64` would add ~16 ms steps) until the
date reaches the target, a `cancel`, a timeout or the game is left.
`run_until` sets the speed and resumes under the input lock, waits without
it, and pauses again. `wall_ms` runs from the start of the wait (just after
the resume took hold) to the first poll that sees the target date; the error
is one poll interval. `paused_ms` adds up the polls that saw the game paused,
so a measured stretch with an event pause can be rejected; `paused_date` is
the date of the last pause that began during the wait. With `max_pause=S` the
wait ends once one pause has lasted `S` seconds. In the lab's first
simulation series an event with a choice ("Tariffs on Fish", 3 October 1720)
paused the game with the player pause flag set; without `max_pause` the trial
waited for the whole timeout.

## Mouse

The game's event loop (`0x1569C70`) passes SDL mouse events to the interface:
motion reads `x`/`y` (+0x14/+0x18) and skips touch (`which` = -1); button
down/up (`0x401`/`0x402`) read `button` (+0x10) and `x`/`y`; the wheel
(`0x403`) reads `y` (+0x14) and uses its sign. That alone was not enough: in
the first live check clicks queued with `SDL_PushEvent` did nothing.

The interface asks for the pointer once per frame in `0x14E5E50` (two
callers, `0x162DDE2` and `0x162E30A`): with focus through `GetCursorPos` and
the window rectangle, without focus through `SDL_GetMouseState` (dynapi slot
209), i.e. SDL's own mouse state, which `SDL_PushEvent` does not update. The
other `GetCursorPos` calls in the executable are inside SDL.

With `input = true` Tongs hooks `0x14E5E50` (Anvil `hook_function`, prologue
`48 89 5C 24 10 55 56 57 48 83 EC 40`). `move`, `click` and `scroll X Y` set
a virtual pointer in client coordinates; while it is set, the hook writes it
to the result instead of calling the game's code. It is dropped by `release`
or as soon as the real cursor is somewhere else than when the pointer was
set, so a player's mouse takes over again. `click` sets the pointer, sends
the motion event, waits 50 ms for the interface to update its hover state,
then sends button down and up.

## Worker pools

From the lab repository (`docs/reverse-engineering.md`, "Inicjalizacja
puli"): two global pools at `0x242B8E0` and `0x242B950`, each with a vector of
8-byte worker handles at +0x38 (begin), +0x40 (end), +0x48 (capacity end).
Their static initializers (`0x8A750`, `0x8A860`) and the store of the
`threads` option (`0x876999` to `0x1FC23AC`) are `[[expect]]` entries. The
main thread is the thread of the game window. The pools are sized once from
the process affinity at start-up; a mask set later (Process Lasso) does not
change them.

## Screenshots

`PrintWindow(PW_CLIENTONLY | PW_RENDERFULLCONTENT)` into a 32-bit DIB, with a
copy from the screen as fallback when the result is all black; PNG through
WIC, into `anvil/data/tongs/screenshots/`, the newest 100 kept.

### Live check (2026-10-17)

Same setup as above, the stack of this release (`tongs-stack-1` and `-2`,
Python client). Anvil: all 12 `[[expect]]` ranges matched; with the hook
"1 patch(es)". Both sessions closed by the script after the PID, path and
start-time check; save unchanged, `settings.txt` differed only in
`graceful_exit` and was restored.

- `window`: 3440x1440 client area at 0,0, DPI 96, not focused.
- `threads`: 31 workers in each pool (capacity 42), process mask `0xFE`
  (Process Lasso's `1-7`), main thread mask `0xFE`, `threads` option -1. The
  pools were created before Process Lasso narrowed the mask.
- `screenshot`: `print_window` worked with the window unfocused and the
  cursor on another monitor; 3440x1440 PNG, 3-7.5 MB.
- `run_until 1719.6.1 speed=5` from 17 March 1719: reached 1719.6.1 after
  6 153 ms (76 days, 12.35 days/s at this mask), paused exactly on the
  target (`overshoot_hours` 0), `paused_ms` 0. A second client read `state`
  every 0.5 s meanwhile. Event pop-ups opened during the run without pausing.
- `cancel` from a second client 2 s into `run_until 1730.1.1` ended it with
  `cancelled`; the game was paused again.
- `wait_until` with the game paused timed out after the given 2 s, with
  `paused_ms` 2 001.
- `key grave` opened the console, `text tongs typed this` appeared in its
  input line (seen in a screenshot), `key grave` closed it again; Esc did not
  close it.
- Clicks without the hook did nothing; with it: speed `+` (3350,32) twice,
  `-` (3350,62) once and the pause button (3407,27) twice changed speed
  2 to 3 to 4 to 3 and paused / resumed, as read from the state.
- `move 1500 800` stopped the map's edge scrolling (the unfocused game had
  taken the real cursor, on another monitor, as being at the right edge) and
  showed the province tooltip at that point; `scroll -6 1500 800` zoomed the
  map out.

## Not verified yet

- `paused` for pauses not caused by the pause button (event popups that pause,
  menus, multiplayer); `paused_flag` alone was confirmed for the button.
- Behaviour in the main menu and during save loading beyond "not in game".
- Input in the main menu, in text fields (Space types instead of pausing),
  with event popups, and with keyboard layouts other than US (SDL key codes
  and the characters sent follow a US layout).
- Whether the speed keys need `SDL_TEXTINPUT` alone or both events; both are
  sent, as SDL does for a real key.
- Mouse: dragging (no command yet), right and middle clicks, double clicks,
  clicks on the map (only interface buttons were tried), and the focused
  window path of `0x14E5E50` (the game was never focused in the checks).
- `run_until` across an event that pauses the game (the mod set used does not
  pause on events).
