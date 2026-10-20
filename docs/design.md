# Tongs — design

Status: draft 4 (2026-10-18). Layers 1 and 2 are implemented (0.3.0), together
with timing (`run_until`), worker pools and screenshots for measurements.
Layer 3 is implemented: the dispatcher (0.4.0), the widget tree (0.5.0), and
widget clicks and events (0.6.0). Layer 4, console game calls, is implemented
(0.7.0), and so are notifications (0.8.0), the last release of draft 4 (see
"Releases"). After draft 4, an MCP server in `mcp/` (0.9.0) offers the
pipe to agents as tools (see "MCP adapter"). The decisions behind them are on
the planning map [Map: game calls and the widget tree](https://github.com/happy-dwarf-team/tongs/issues/15).
Words such as game call, main-thread read, outer frame, widget path, event,
notification and marked run are defined in [`GLOSSARY.md`](../GLOSSARY.md).

Addresses marked **static** were found by static analysis only. Each release
PR checks them on a running game before it merges. Details, the method behind
each claim and the full byte patterns (now `plugin/symbols.toml`) are on the research branches
(never merged):
[`research/main-thread-dispatch`](https://github.com/happy-dwarf-team/tongs/blob/research/main-thread-dispatch/docs/research/main-thread-dispatch.md),
[`research/widget-tree`](https://github.com/happy-dwarf-team/tongs/blob/research/widget-tree/docs/research/widget-tree.md),
[`research/console-commands`](https://github.com/happy-dwarf-team/tongs/blob/research/console-commands/docs/research/console-commands.md),
[`research/event-ids`](https://github.com/happy-dwarf-team/tongs/blob/research/event-ids/docs/research/event-ids.md).

## Goal

Let a test agent control and observe a running game through commands with a
verifiable answer, instead of screenshots and pixel coordinates. Every command
returns the state read from the game after it ran.

Layers, cheapest first:

1. **State (0.1.0):** date, speed, pause. Read-only.
2. **Input (0.2.0):** inject keyboard/mouse events into the game's own SDL
   event queue (`SDL_PushEvent` is linked statically into `eu4.exe`), so
   shortcuts work without focusing the window. Commands such as `pause`,
   `resume`, `speed N`, `key`, `text`, `click`, each answered with the state
   after the action. Mouse positions also need the cursor hook (see "Mouse").
3. **Widgets by name (0.4.0–0.6.0):** a dispatcher runs work on the game's
   main thread after each outer frame. On top of it, main-thread reads give
   the widget tree, found by selectors. Widgets are clicked through layer 2,
   and events are chosen the same way.
4. **Game calls (0.7.0):** console commands run on the main thread, behind
   their own switch and the refusals of ADR 0001.

Beside the layers, **notifications (0.8.0)** report changes to a subscribed
client without being asked.

Actions in layers 2 and 3 go through the same path as a player's input, never
by writing game state directly, so the game behaves as it would for a human
player. Only layer 4 changes game state by running game code. It is off unless
`game_calls = true`, and every game call is counted, so a measurement can
reject a marked run.

Beside the layers, for measurements: `run_until` / `wait_until` time the
game between two dates, `threads` reads the worker pools and CPU masks the
lab checks before every simulation measurement, `screenshot` captures the
window for agents that need to look.

## Transport

- Named pipe `\\.\pipe\tongs`, 8 instances (4 before 0.8.0), each served by its own thread.
  The first is created with `FILE_FLAG_FIRST_PIPE_INSTANCE` (a second game
  cannot take the name over and logs an error instead); all with
  `PIPE_REJECT_REMOTE_CLIENTS` and a DACL that allows only the user running
  the game and SYSTEM.
- Up to 8 clients at once, one per instance; requests are lines, answers are
  single-line JSON. Input commands take turns through one lock, so presses of
  two clients never interleave; `run_until` holds it only while pressing.
- Off unless `enabled = true` in `anvil/data/tongs/settings.toml` (before
  Anvil 1.0: `config.toml`, see "Anvil 1.0 (plugin API 2)"); the input
  commands also need `input = true`, and game calls (0.7.0) need
  `game_calls = true`.
- From 0.8.0: 8 instances, because a subscription holds one. A connection
  that sends `subscribe KINDS` carries only notifications from then on, until
  `unsubscribe` (see "Notifications"). The server stays blocking, with one
  thread per instance.

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

While a save is read into the session (live, 2026-10-19: reloading the save
from the in-game menu), the session pointer is null for a second or two and
then the date is 1 January of year 1, hour 0, for a moment. Tongs answers
`in_game: false` for both: "no game loaded (session is null)" and "save still
loading (date not set)". In the main menu the pointer at `root + 0x1E00`
holds another class ("session object has an unexpected vtable").

`plugin/symbols.toml` declares the code that holds these offsets and takes
them from it as `values`, so Anvil skips the plugin when any of it differs.

### Threading

The pipe thread reads game memory while the main thread runs the game. Reads
are word-sized and guarded by SEH; while a save is loading or the game is
quitting, objects may be in transition and `state` answers `in_game: false`
or a read error rather than crashing. Calling `0x7E01B0` from the pipe thread
is read-only (it reads the pause byte and the session's multiplayer state).
Input commands also run on the pipe thread: they only queue SDL events
(see "Input"), and the main thread handles them.

Anything that walks the game's object graphs, such as the widget tree and
event lists, or that runs game code (game calls), runs on the main thread
through the dispatcher instead (0.4.0, see "Dispatcher").

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
game (below). RVAs relative to `eu4.exe`; `plugin/symbols.toml` checks the code.

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

### Marked runs and events during a run (0.6.0, 0.7.0)

`wait_until` and `run_until` also report `inputs` (0.6.0) and `game_calls`
(0.7.0): how many inputs and game calls any client sent during the measured
stretch. The presses `run_until` makes itself before and after the wait do
not count. A stretch with either count above 0 is a marked run, and a
measurement rejects it. `state` reports `game_calls_total` since the save was
loaded.

An input is one input command that queued at least one SDL event: `key`,
`text`, `move`, `click`, `scroll`, `pause`, `resume`, `speed`, `ui click`,
`event choose`, and inside `run_until events=first` each choice and each
resume after a choice. A counter shared by all clients is read at the start
and the end of the wait; `run_until` starts its wait after its own speed and
resume presses and ends it before its pause.

`run_until DATE events=first` (0.6.0) chooses the first enabled option of each
event that opens during the run, topmost window first, through `event choose`
(see "Events"). Each choice counts as an input, so the run is marked. The
answer lists the choices in `events`, each with the game date, `id`, title,
option number and text, `option_index`, `option_key` and widget path. The
default `events=stop` keeps today's behaviour, with `max_pause`. When an event
has no enabled option, the run ends with `no_enabled_option` and the event's
data.

How `events=first` works (0.6.0):

- Between the 10 ms polls of the wait, the pipe thread reads one word, the
  count of the session's event window list (`session+0x4A8`, written by
  `0x82B07F`). Only while it is above 0 does it read the events on the main
  thread, at most every 250 ms when none is waiting for a choice.
- It takes the input lock, picks the topmost event (the newest, see
  "Events") and waits up to 10 s for an option to become enabled (the game
  keeps them disabled for half a second after opening). Without one, the run
  ends with `no_enabled_option`.
- It clicks the first enabled option as `event choose` does, waits until the
  event is no longer waiting for a choice, and presses `resume` when the game
  is left with the player pause on. A click that is refused (`covered`) or
  has no effect within 3 s ends the run.

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
`48 89 5C 24 10 55 56 57 48 83 EC 40`; symbol `gui_cursor` since Anvil 1.0). `move`, `click` and `scroll X Y` set
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
Their static initializers (`0x8A6D6` and `0x8A750`, `0x8A7E6` and
`0x8A860`) and the store of the `threads` option (`0x876999` to `0x1FC23AC`)
are symbols, whose values give the pools, their vectors and the option. The
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

## Dispatcher (0.4.0)

Main-thread reads and game calls run on the game's main thread, after an
outer frame. Static findings:

- The frame function `0x14C25A0(app, outer)` has three callers:
  - the main loop `0x14C2D00`, with `outer` = 1;
  - the first-start language loop `0x2003D0`, with `outer` = 1;
  - `0x769EA0`, with `outer` = 0. It draws nested frames from inside game
    updates and while a save loads. Nesting goes at most one level deep.
- The event loop `0x1569C70` and the cursor query `0x14E5E50` also run in
  nested frames, and nothing there tells the two kinds apart, so neither is
  used as the dispatch point.
- Outer frames run in the main menu, while paused and while minimized. They do
  **not** run while a save loads, because the whole load happens inside one
  outer frame, nor during start-up before "Running application...".
- The frame counter `app+0x320` is incremented at `0x14C2A23`.

Tongs hooks `0x14C25A0` with `hook_function`. The 26-byte prologue is
`48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 70`,
with nothing RIP-relative in it. The detour calls the original, then runs the
queue only when all three hold:

- `outer` is non-zero;
- the call is on the main thread (its id is recorded on the first outer
  frame);
- the quit flags `app+0x83` and `app+0x2F8` are both 0.

The research proposes six `[[expect]]` entries: the prologue, the three call
sites, the counter and the session gate `0x813F95`.

The hook is installed whenever `enabled = true`. The 0.4.0 `SIMULATION` A/B
(below) showed no cost, so there is no opt-out.

Since Anvil 1.0 this hook is `anvil-game`'s, not Tongs' (see "Anvil 1.0
(plugin API 2)"); the conditions and the contract below are unchanged.

**Contract** (from "What does a call dispatched to the main thread
promise?"):

- **Synchronous.** The pipe thread queues a job and waits for its answer:
  one request line, one answer. The default limit is 10 s, and `timeout=S`
  changes it. Only that connection waits; other clients can still read
  `state` or send `cancel`.
- **Withdrawal.**
  - A job not started before its limit is withdrawn and never runs. The
    answer is `ok: false`, error `main thread busy`, with `queued_ms`.
  - `cancel` withdraws every queued job that has not started; their clients
    get `cancelled`.
  - A job that has started always finishes, and its answer waits for it.
- **Order.**
  - One FIFO queue for all clients.
  - After each outer frame, all queued main-thread reads run. A game call ends
    the batch, so the next job waits for the next outer frame and sees the
    call's effect.
  - Game calls take the input lock; main-thread reads do not.
- **Where.**
  - Never in nested frames.
  - Main-thread reads run wherever outer frames run.
  - Game calls run only with `in_game: true`.
  - While a save loads, jobs wait, and their limit applies.
- **Faults.**
  - Main-thread reads are guarded by SEH and answer `ok: false` on a fault.
  - A game call that faults answers an error and trips game calls until the
    game exits: later ones answer `game_calls_tripped`, while reads keep
    working.
- **Fields.** Every main-thread answer carries `queued_ms`, `run_ms` and
  `frame` (`app+0x320`).

`frames` (0.4.0, a main-thread read) answers with the counts of outer and
nested frames per second over the last second, the longest gap between outer
frames since the previous `frames`, and `frame`.

Live checks in the 0.4.0 PR, through the lab's launcher rules:

- outer and nested frames per second in each situation: the main menu, a save
  loading from the menu, in game paused, speed 1, speed 5, unfocused,
  minimized;
- the thread of every call compared with the main thread from `threads`;
- that no outer frame runs before "Running application...";
- the longest gap between outer frames at speed 5 over a simulated month;
- the date advance per second at speed 5 with a no-op job in every outer
  frame, against the same run without the hook.

### Live check (2026-10-18)

Setup:
- the original `eu4.exe` (`9AD3EFE1…771A`), Anvil 0.2.0 and the 0.4.0 branch;
- the Russia save of 17 March 1719, loaded with `continue=`;
- the lab's `anvil-play.ps1 -Scenario tongs -TongsInput` (`tongs-dispatch-1`) under Process Lasso's `1-7`;
- a script sent `frames` and the input commands.

Anvil reported "18 expected byte range(s) match" and "2 patch(es)" (the frame hook and the cursor hook). Afterwards:
- the game was closed by the script after the PID, path and start-time check;
- the save was unchanged;
- `settings.txt` differed only in `graceful_exit` and was restored.

Findings:

- **Start-up and loading.** For 79 s after start (start-up with the save
  loading inside it), `frames timeout=1` answered `main thread busy` every
  time, also for the first 6 s in which `state` already said `in_game: true`.
  When the first answer came, `outer_total` was 4. Outer frames do not run
  during start-up and loading, as the static analysis said.
- **Main thread.** `thread_id` was 62956 in every answer, the same as
  `threads` → `main_thread_id` and the window's thread.
- **Frame rates** (the window had focus; frames per second over the last
  complete second, and the longest gap between outer frames):

  | Situation | Outer/s | Nested/s | Longest gap | `queued_ms` |
  | --- | --- | --- | --- | --- |
  | 3 s after loading | 84 | 0 | 406 ms | 0 |
  | paused, 5 s | 63 | 0 | 107 ms | 12 |
  | speed 1, 5 s | 74 | 3 | 147 ms | 4 |
  | speed 5, 5 s | 9 | 13 | 564 ms | 82 |
  | speed 5, `run_until` one month | 9 | 13 | 386 ms | 320 |

  At speed 5 the game update fills most frames, nested frames keep the
  screen moving, and a main-thread job waits up to about half a second.
- **Jobs every frame.** A second client sent `frames` back to back during
  `run_until` 1719.5.12 → 1719.6.1: 21 jobs in 1.6 s, about one per outer
  frame, none failed. That stretch ran at 12.6 days/s; the next month without
  jobs ran at 11.9 days/s; 0.3.0 had 12.4 days/s for 17 March → 1 June in
  `tongs-stack-1`. No cost is visible at this resolution. This is not a
  `SIMULATION` measurement (CPU 1–7, no fixed main-thread pinning, one run
  each).
- **Cancel.** `cancel` answered `withdrawn: 0` (nothing queued).

**Cost under `SIMULATION` conditions** (2026-10-18). Runs `sim-hook-ab-1` …
`-8` used the lab's `measure-year-tongs.ps1` with these conditions:
- the original EXE;
- 8 workers in both pools;
- the process on CPU 0–15 (`0xFFFF`) and the main thread on CPU4 (`0x10`);
- the restricted broker;
- the Russia save, warm-up to 1720.1.1, then 1720.1.1 → 1721.1.1 at speed 5
  (365 days), measured by `run_until`.

Runs alternated 0.3.0 (no frame hook) and this branch, one trial per run. Two
trials were rejected because an event paused the game during the stretch
(`max_pause` 5 s), one of each build, and one more run of each replaced them.

| Build | Valid trials (s) | Mean | Days/s |
| --- | --- | --- | --- |
| 0.3.0 | 27.08, 30.82, 28.39 | 28.76 s | 12.7 |
| 0.4.0 branch | 25.89, 26.08, 26.79 | 26.25 s | 13.9 |

The branch was not slower; the difference lies within the spread between
runs that the lab has seen before (27.6–28.4 s for the same stretch). The
hook gets no opt-out. The Process Lasso rule `eu4.exe,0,1-7` was back after
every run, and the broker was stopped after the batch.

The main menu, a save loaded from it and an unfocused window were checked in
the hardening pass (2026-10-19, "Hardening pass (0.10.0)" below): outer frames
run in the menu (159/s unfocused) and stop while a save loads from it (3.5 s
of `main thread busy`). Still open: a minimized window. The game's borderless
window did not minimize from the taskbar (`IsIconic` stayed false), and
computer-use was not available to try another way.

## Widget tree (0.5.0)

`ui tree` and `ui find` are main-thread reads: `src/game_ui.cpp` copies the
tree on the main thread, and `src/ui_tree.cpp` matches selectors and builds
the answer on the pipe thread. The research found the layout statically; the
0.5.0 live check (below) corrected it where the running game differed, and
this section describes what holds.

- **Registry.** The GUI manager pointer is at `0x23494F0` (constructor
  `0x14E8640`, vtable `0x1D8DC40`).
  - `mgr+0x110/+0x118` is a vector of every live widget, about 16 000 in the
    lab's save.
  - `0x14E9FA0` deletes the entries marked dead (`+0xC4`) from that vector,
    which is why the tree is read only on the main thread.
  - `mgr+0x130` is the topmost widget under the pointer, and `mgr+0x140` the
    keyboard focus (not read in 0.5.0).
- **Widget fields** (offsets from the pointer the registry holds):

  | Field | Where |
  | --- | --- |
  | Name | vtable slot `0xA0`, a `std::string*`: the `.gui` name, case kept (`type+0x10` of the definition at `+0xB0`, or a window's own name); from 0.14.0 called only when that definition is live ("Stale widgets") |
  | Visible | `+0xC5` |
  | Dead | `+0xC4` |
  | Position | slot `0xD8`: two int32 in client pixels, read from the widget's render node |
  | Size | slot `0xE0` |
  | Text | button `+0x200`, instant text box `+0x170`, text box `+0x178`, edit box `+0xF8`; Windows-1252 |
  | Button state | `+0x124`; 3 = disabled |
  | Check box state | byte `+0x100` of `checkboxType`; 1 = ticked |
  | Render node | per kind (table below); the node holds its owner at `+0x118` and its parent node at `+0x110` |

  Slot `0x98`, which the research proposed for the position, gives the
  position relative to the parent and is not used.
- **Kinds.** GUI classes have no RTTI, so a widget's kind is told by its
  vtable. The live game showed that most buttons and icons are of the game's
  own classes (`guiButtonType`, `iconType`); the vtables the research gave for
  them belong to the older `buttonType` and to three icons of the music
  player.

  | Kind | Vtable | `type` | Node | Children |
  | --- | --- | --- | --- | --- |
  | `windowType` | `0x1D929D0` | `window` | `+0xA00` | window lists |
  | `eu3dialogtype` | `0x1D57510` | `window` | `+0xA00` | window lists |
  | `containerWindowType` | `0x1DA4090` | `window` | `+0xA50` | container lists |
  | `guiButtonType` | `0x1D58218` | `button` | `+0x290` | — |
  | `buttonType` | `0x1D9A340` | `button` | `+0x290` | — |
  | scroll bar slider / track button | `0x1D954B0` / `0x1DA2940` | `button` | `+0x168` / `+0x180` | — |
  | `iconType` and music player icons | `0x1D585C8`, `0x1D96D00` | `icon` | `+0x168` | — |
  | `instantTextBoxType` | `0x1D8FD10` | `text` | `+0x118` | — |
  | `textBoxType` | `0x1D99230` | `text` | `+0xD0` | — |
  | `editBoxType` | `0x1D91358` | `editbox` | `+0x1E8` | — |
  | `checkboxType` | `0x1D8FFC8` | `checkbox` | `+0x110` | — |
  | `listboxType` | `0x1D90FD8` | `listbox` | `+0x180` | items, scroll bar |
  | `smoothListboxType` | `0x1D96578` | `listbox` | `+0x218` | items, scroll bar |
  | `gridBoxType` | `0x1D97EF0` | `listbox` | — | not known |
  | `scrollbarType` | `0x1D93748` | `other` | — | its four buttons |

  Every other class (overlapping elements boxes, extended scroll bars, the
  minimap) answers `type: "other"`.
- **Children.** There is no generic child list; each class keeps its own
  containers. Tongs reads them in this order:
  - `windowType` windows (offsets from the full object, `0x30` before the
    widget pointer): ten vector pairs (`+0x5B8`…`+0x768`), seven named maps
    (`+0x798`…`+0x918`, an insertion list at `map+0x20`) and the sub-windows
    (`+0x958`, `{std::string name; window*}`);
  - container windows: fifteen vector pairs (`+0x680`…`+0x920`);
  - list boxes: an insertion list of items at `+0x100` (nodes
    `{item, prev, next}`, as in the named maps); the item holds the full
    object of its window at `+0x40` (`listboxType`) or `+0x60`
    (`smoothListboxType`); the scroll bar is at `+0x1E0` or `+0x228`;
  - scroll bars: the slider `+0x158`, track `+0x2F0`, down `+0x2F8` and up
    `+0x3E0` buttons.

  A pointer counts only when it is a live widget of the registry (as the
  widget pointer or as the full object of a window); the first holder wins.
- **Top-level windows.** Some widgets are held by the game's own code, not by
  a window: the court view's `abilities_listbox`, the menu bar's
  `chat_list`, the text scroll bars of tooltips. Tongs places each of them
  under the owner of the parent of its render node (climbing past nodes
  without an owner); a scroll bar, which has no node, goes where its
  buttons' nodes point. Whatever is left are the top-level windows, in
  registry order.
- **Scroll bars** have no render node, so slot `0xD8` gives their position
  relative to the list box (live: x 425, y 0 for a bar drawn at 444, 111).
  Their `rect` is the bounds of their buttons.

**Selectors** (from "What do selectors and the ui tree answer look like?",
with a throwaway prototype on `prototype/ui-selectors`):

- **Paths.**
  - `a/b`: `b` is a child of `a`.
  - `a//b`: `b` is anywhere below `a`.
  - A selector that starts with a name is anchored at the top-level windows;
    one that starts with `//` searches everywhere.
  - Names ignore case (ASCII letters) and allow `*` and `?`. A step made only
    of filters means any name.
- **Filters:**
  - `[n]`: the n-th match, counted from 1; `[0]` is an error;
  - `[type=…]`, with `window`, `button`, `icon`, `text`, `listbox`,
    `checkbox`, `editbox` or `other`;
  - `[text=…]`, `[text~=…]` (contains) and `[text^=…]` (starts with), which
    ignore case and compare the displayed text, so they depend on the game's
    language; names and `[n]` come first. Values may be quoted
    (`[text^="Accept the"]`); a widget without text never matches;
  - `[enabled]`, `[disabled]` (buttons only).
- **Hidden widgets**, and everything inside a hidden window, are left out
  unless the command ends with `all`.

**Answers.**

- Each widget has:
  - `path`: its widget path, which can be sent back as a selector with the
    same `all`. Siblings that share a name get `[n]`, counted among the
    widgets the command sees; a widget without a usable name gets a
    positional step such as `[3]`;
  - `type`, `visible` (the widget and all its ancestors);
  - `rect` (client pixels of the game window) and `click_point` (its centre);
  - `text` (displayed text: UTF-8, without the colour codes `§X`; icons stay
    as `£name`) and `enabled` where they apply.
- `ui find SEL [all] [limit=N]` returns `count`, `matches` (50 by default) and
  `truncated`.
- `ui tree [SEL] [depth=N] [limit=N] [all]` returns nested `widgets` (each
  with its `name`), up to 200 by default. `depth=N` replaces deeper levels
  with `children_count`. With a selector, each match is a root; a match inside
  another match is shown once, inside it.
- Limits go up to 5000, `depth` up to 64; both commands take `timeout=S` like
  `frames`.
- Errors: `bad_selector` and `bad_command` (with `detail`) before anything
  runs on the main thread, `not_found` when `ui tree SEL` matches nothing,
  and the dispatcher's own (`main thread busy`, `cancelled`).

### Live check (2026-10-19)

Setup:
- the original `eu4.exe` (`9AD3EFE1…771A`), Anvil 0.2.0 and the 0.5.0 branch;
- the Russia save of 17 March 1719, loaded with `continue=`; a 3440×1440
  window at DPI 96;
- the lab's `anvil-play.ps1 -Scenario tongs -TongsInput` under Process
  Lasso's `1-7`: `tongs-ui-1` with a temporary build that dumped the raw
  registry (not shipped), `tongs-ui-2` and `tongs-ui-3` with this branch;
- scripts sent `ui`, `state`, `click`, `key f1` and `screenshot`; read-only
  reads of the game's memory from outside (`ReadProcessMemory`) helped find
  the list box and render node layout during `tongs-ui-1`.

Anvil reported "66 expected byte range(s) match" and "2 patch(es)". Every
session was closed by the script after the PID, path and start-time check;
the save was unchanged, and `settings.txt` differed only in `graceful_exit`
and was restored.

Findings:

- **Registry and cost.** 16 111–16 210 live widgets; reading the whole tree
  took 6–7 ms (`run_ms`) on the main thread, with the game paused.
- **Coordinates.** Slot `0xD8` gives client pixels directly; the scale is 1
  (the render context's two floats `ctx+0x4D8/+0x4DC` were 1.0).
  - The speed controls report `button_speedup` at (3335, 18) 30×30,
    `button_speeddown` (3347, 49) 30×30 and `speed_indicator` (3359, 12)
    75×75; the clicks that worked in 0.3.0 (3350,32 / 3350,62 / 3407,27) lie
    inside them.
  - Clicking the reported `click_point` changed the speed 2 → 3 → 4
    ((3350, 33)) and back to 3 ((3362, 64)), and (3396, 49) paused and
    resumed. The corners (3336, 19) and (3364, 47) of `button_speedup` also
    worked, so the whole rectangle of slots `0xD8` + `0xE0` is clickable for
    this button.
  - In screenshots, `DateText` (3104, 27) 228×38 is centred on the drawn
    date, and the court view's close and abdicate buttons sit exactly in
    their rectangles.
  - Window positions are right, but window sizes come from the `.gui` size
    and need not cover the drawn panel: `countryview` and `countrycourtview`
    report 1229×120.
- **Top-level windows.** The first dump left 3 744 widgets without a holder
  (list items, scroll bars, check boxes); reading list boxes and scroll bars
  left 1 081, and render-node parents 394. Visible among them: `outliner`,
  `message_icons`, `topbar`, `speed_controls`, `menubar`, `minimap_panel`,
  `diplomessageicons_window` and map icons (`unit_mapicon`, `fort_mapicon`,
  …). F1 added `countryview` and `countrycourtview`, with
  `abilities_listbox` and its three advisor entries inside the latter.
- **Render nodes.** For 413 of 414 drawn nodes, the owner of the parent node
  (`+0x110`) was the parent known from the containers. A list box's own node
  has no owner set.
- **Hidden windows.** Of 11 254 children of hidden windows in the first
  dump, 878 still had `+0xC5` set, so `visible` takes the ancestors into
  account.
- **Text.** Windows-1252: "Münster", "Içel" and "Hüdavendigar" came through
  as UTF-8. The game's colour codes (`§G…§!`) are dropped, icons stay as
  `£attrition`, and `DateText` read "17 March 1719" as `state` did.
  Button labels are at `+0x200` for `guiButtonType` too ("Cancel", "Start
  Ironman"); 393 of 4 755 buttons had state 3 (e.g. `button_achievements`,
  `abdicate_ruler_button`).
- **Paths.** 150 random paths of visible widgets each resolved back to the
  same widget. Map icons come and go as the map moves, so their `[n]`
  changes between reads.

Check boxes answer `checked` and match `[checked]` / `[unchecked]`; edit boxes
answer their `text`, also when empty (both from the hardening pass, where the
offsets were found: `+0x100` flipped 0 → 1 → 0 → 1 over three clicks on the
lobby's `show_unsafe_checkbox` and was 1 only on the selected save row;
`+0xF8` followed typing and Backspace in `console_edit`, and the lobby's
`year` read "1444"). The main menu and a save loaded from it were read in the
same pass. `+0x8F` and `mgr+0x130` were checked in 0.6.0 (below).

Not checked: the children of grid boxes and overlapping elements boxes
(`OverlappingElementsBoxType` is vtable `0x1DA4910`; their containers were
not found, [#48](https://github.com/happy-dwarf-team/tongs/issues/48)), and
sizes that differ from the drawn widget
([#50](https://github.com/happy-dwarf-team/tongs/issues/50)).

## Stale widgets (0.14.0)

A reload of a `.gui` file (console `reload <file>`) deletes the file's old
definitions. Widgets the game rebuilds get the new ones, but some stay alive
with a pointer (`+0xB0`) to a freed definition: windows nobody rebuilds,
windows the game leaks when it rebuilds the HUD (`alerts_parent`,
`hotkey_assign_parent`, the in-game load and save menus, the outliner's rows),
and list rows built before the reload. Source: Etch research
["Which widgets keep freed definitions after a reload"](https://github.com/happy-dwarf-team/etch/issues/14)
(branch `research/stale-definitions`, `docs/research/stale-definitions.md`).

Until 0.13.0 the tree reader asked every widget for its name through slot
`0xA0`. For most classes that slot is a thunk (`0xFBF480`) that loads `+0xB0`
and jumps to the definition's virtual `+0x40`. During an Etch prototype run,
a `ui find` after `reload topbar.gui` hit a widget whose freed definition
block held an object with `_purecall` at `+0x40`. The game's pure-call handler
wrote a crash dump and ended the process. The `__try` guards catch only access
violations, and a pure call is not an exception, so no guard can catch it.

From 0.14.0 the reader calls slot `0xA0` only for widgets whose definition is
live (`src/ui_definitions.cpp`):

- **Live definitions.** Once per tree read, the reader collects every live
  definition:
  - **Registry.** It walks the type registry at `mgr+0xC8`, a
    case-insensitive ternary search tree of plain pointers. The root is at
    `registry+0x08`. Each `0x28`-byte node holds the value (`+0x00`), the
    lower-cased character (`+0x08`), the lower and higher subtrees (`+0x10`,
    `+0x18`) and the next character (`+0x20`). A reload replaces values in
    the nodes; it does not free nodes, and it runs on the main thread like the
    read. This walk reads no definition.
  - **Nested definitions.** Then it reads each definition found, which is safe
    because each one is live. When its class is one of five, it walks the
    nested map the class keeps, a map of the registry's class (vtable
    `0x1D8DC00`). The map counts only when that vtable is at the offset.

    | Definition class | Vtable | Nested map | Constructor |
    | --- | --- | --- | --- |
    | `windowType` | `0x1D94478` | `+0xE8` | `0x155B8E0` |
    | `eu3dialogtype` | `0x1D574A0` | `+0xE8` (built by the `windowType` constructor) | `0x10B3FE0` |
    | list boxes | `0x1D9E800` | `+0x1B0` | `0x15EAB80` |
    | `containerWindowType` | `0x1D9EB70` | `+0x260` | `0x15EC1F0` |
    | `scrollbarType` | `0x1D9E5E0` | `+0x208` | `0x15E8460` |

    The list box class is named by its count: 278 live definitions, the
    `listboxType` and `smoothListboxType` entries of vanilla. The others match
    their vanilla counts too (862 `windowType`, 54 `eu3dialogtype`, 30
    `containerWindowType`).

  An unreadable node or live definition fails the whole read rather than
  giving a smaller set.
- **A widget with a live definition** is named by slot `0xA0` as before (case
  kept). Every other widget is **stale**, and so is everything inside it. A
  stale widget is named by its holder's child list when a window or container
  holds it, by the window's own name (`windowType` and `eu3dialogtype`
  `+0xA28`, `containerWindowType` `+0x5F8`) when that is set, and otherwise
  not at all. The holder's lists are filled with copies when the window is
  built, so they are readable as long as the window is:
  - the vector pairs: the names vector follows the pointers vector (`+0x18`);
    the game stores the names in lower case;
  - the named maps: entries `{value, std::string name at +0x08}`;
  - the sub-windows: `{std::string name, window*}`.

A pointer in the set points at a live definition, so a slot `0xA0` call that
the reader makes is memory-safe. A stale widget can still read as live in one
case. A reload deletes each old definition before it parses the next entry,
so a later definition of the same reload may get the freed address. The
widget then reads as live and takes the other definition's name. A host that
reloads can rule this out by remembering, before the reload, which roots had
the file's old definitions.

The research proposed only the registry check, with names of children from
their holders' lists. The live check below showed why that is not enough:
most definitions are nested and never in the registry. With the registry
alone, a fresh game had 17 stale roots and about 8 900 stale widgets (list
boxes, check boxes and edit boxes of every country view). The research's
proposal for children would also have renamed them to the lower-case copies,
and it would have left scroll bar buttons without a name.

Answers:

- `ui tree` shows stale widgets with `"stale": true`; a stale root without a
  name has a positional path step (`[136]`). Live widgets have no `stale`
  member.
- `ui find` leaves stale widgets out; `ui find SEL stale` includes them.
- `ui click` never clicks one. When only stale widgets match, it refuses with
  `stale` and their paths. Hovering a stale button may make the game read its
  tooltip from the freed definition (research H4).
- The hovered widget in `covered_by` is named even when stale.

The offsets are in `symbols.toml`: `row_type_lookup`, `type_tree_insert`,
`type_tree_node`, the five definition classes (`window_type_definition`,
`eu3dialog_definition`, `listbox_definition`(`_map`),
`container_definition`(`_map`), `scrollbar_definition`(`_map`)),
`window_name_slot`, `container_name_slot`, and the `names` of the two
find-child symbols. They were checked with `anvil check plugin --exe` against
`eu4.exe` 1.37.5 (`9AD3EFE1…771A`). The map entry name (`+0x08`), the
sub-window name (`+0x00`) and the string size (`0x20`) are not in
`symbols.toml`. They come from the decompiled registry list (`0x14EC950`),
the sub-window lookup (`0x15481A0`) and the pair lookup (`0x154F650`). Unit
tests build fake definitions with nested maps the way the game's insert
(`0x14ED760`) builds them. Their memory reader refuses everything but the
registry, its nodes and live definitions. The tests also cover a reload (the
old definition and everything nested in it neither live nor read), a class
not listed, a map whose vtable is wrong, unreadable memory, a loop and the
naming rules.

### Live check (2026-10-20)

Setup:
- the original `eu4.exe` (`9AD3EFE1…771A`), Anvil 1.0.0 with `anvil-game`,
  and this branch;
- the lab's Russia save of 17 March 1719 (`continue=`), paused, with the
  vanilla interface: a temporary mod with unchanged copies of `outliner.gui`,
  `topbar.gui` and three more files, enabled alone, as in the Etch reload
  prototype (`dlc_load.json` restored afterwards);
- `anvil-play.ps1 -Scenario tongs -TongsInput -TongsGameCalls`, which checks
  the EXE hash and the Process Lasso rule and closes the game after the PID,
  path and start-time check.

The script read the tree, ran `console reload outliner.gui` twice and
`console reload topbar.gui` once, and read the tree after each reload. After
the topbar reload it also ran the read that crashed the prototype,
`ui find topbar/brown_bg all`, three times. Read-only `ReadProcessMemory`
from outside counted the same sets independently. Anvil reported "108
symbol(s) verified".

Session `tongs61-stale-1` used the first version of this change (registry
only). Session `tongs61-stale-2` used the version described above:

| After | Roots | Stale roots | Of them `{icon, text}` windows | Stale widgets | Tree read |
| --- | --- | --- | --- | --- | --- |
| loading | 385 | 0 | 0 | 7 | 14 ms |
| `reload outliner.gui` | 421 | 17 | 17 | 58 | 11 ms |
| `reload outliner.gui` | 470 | 34 | 34 | 109 | 10 ms |
| `reload topbar.gui` | 526 | 40 | 37 | 121 | 10 ms |

Findings:

- **No crash.** No crash in either session: all reads after the topbar
  reload answered, and the game kept running.
- **Research H1 holds.** The stale `{icon, text}` roots are outliner header
  rows (icon `outliner_header`, text `entry_text`): 17 per outliner rebuild,
  not 25 as the research guessed. After the topbar reload,
  `hotkey_assign_parent` (with `assign_hotkey_pick_button_hint`) and the
  childless `alerts_parent` follow.
- **Same count as an independent read.** From outside, 121 widgets had a
  definition that was neither in the registry nor nested in a live
  definition, the same 121 Tongs marked. The other 16 181 split into 2 084
  top-level and 14 097 nested.
- **Names unchanged.** `speed_controls/DateText` and `//downButton` (335
  matches before the reloads) resolved as in 0.13.0.
- **Cost.** The read cost 10–14 ms on the main thread, against 6–7 ms in
  0.5.0. The registry and nested maps hold about 8 800 definitions.
- **Seven false positives.** Seven hidden buttons were stale before any reload:
  the music player's controls (`musicplayer_window`,
  `musicminicontroller_window`) and the `pops_login_blocker` button. Their
  definitions (class `0x1D9A7B0`) are live, but they are held by plain
  pointer fields, not maps: class `0x1D9ED10` at `+0x140`…`+0x158` and
  `containerWindowType` definitions at `+0x3A0`. The walk does not follow
  those fields.
- **Settings and save.** The save was unchanged, and `settings.txt` differed
  only in `graceful_exit` (restored). No crash dump was written.

Known gap: when a definition class outside the table holds nested
definitions, their widgets read as stale (unnamed, left out of `ui find`).
That is a safe failure, not a crash, and the seven above are its only case
seen in the lab's save.

## Clicking widgets (0.6.0)

`ui click SEL` resolves the selector as a main-thread read, then clicks the
centre of the widget through the input path ("Mouse"). It refuses rather than
guess:

- `ambiguous`, with up to 10 matching paths, when the selector matches more
  than one widget;
- `disabled`;
- `not_visible`, when the widget exists but is hidden;
- `not_found`;
- `covered`, with `covered_by`. After `move`, Tongs reads `mgr+0x130` on the
  main thread; when it is neither the target nor inside it, the click is not
  sent.

The game delivers a click to the topmost visible owner under the pointer, and
a button reacts only when `+0x8F` (hovered this frame) is set, which is why
the cursor hook is needed. On success the answer has `clicked`, `point` and the
state after the click. `ui click` needs `input = true` and counts as an input.
When event windows are open, `ui click` reads the events with the tree; a
click on an option button answers `event_option` (`id`, `number`,
`option_index`) and is noted as a choice right after the click, before the
settle time, so the `closed` notification names it (`via: "ui click"`).

How it runs (`src/protocol.cpp`, `ClickWidget`; decisions in
`src/ui_tree.cpp`):

1. Under the input lock, the tree is read as a main-thread read and the
   selector resolved: visible matches first; when there is none, a match
   among hidden widgets gives `not_visible`. Paths and the `disabled` check
   come from the same read.
2. The virtual cursor is set to the centre of `rect` and a motion event
   queued; then 50 ms. A centre with a negative coordinate (a panel still
   sliding in) or outside the client area is refused without input.
3. An empty main-thread job runs, then a second one reads the tree again and
   `mgr+0x130`. The first job may follow a frame that began before the move;
   the second follows a whole frame that began after it, so the game has
   picked the widget under the new pointer.
4. The hovered widget must be the target or inside it (by the tree's
   parents); otherwise the answer is `covered` with `covered_by` (its path,
   or `null` when it is not in the tree or the pointer is 0). The click is not
   sent. When `mgr+0x130` is 0, steps 2–4 run again, up to three times in
   all, the later ones after a step one pixel aside (`hover_attempts`): live,
   the first move after a click on a scroll bar button sometimes left the
   game with no hovered widget (two of a few dozen clicks in `tongs-harden-1`,
   3 of 30 repeated `downButton` clicks in `tongs-harden-2`), and the same
   move again found the target.
5. Button down and up are queued; the answer has `hovered` (the path of the
   hovered widget) and the state read 0.1 s later.

Static findings checked for this: the hover callback `0x15528D0` writes
`mgr+0x130 = this`, and `0x14DEFD0` calls it for the picked owners from the
last to the first, so the first (topmost) one stays. Both are `[[expect]]`
entries.

### Live check (2026-10-19)

Setup:
- the original `eu4.exe` (`9AD3EFE1…771A`), Anvil 0.2.0 and the 0.6.0 branch;
- the Russia save of 17 March 1719, loaded with `continue=`; a 3440×1440
  window at DPI 96, focused;
- the lab's `anvil-play.ps1 -Scenario tongs -TongsInput` (`tongs-events-1`)
  under Process Lasso's `1-7`; Python scripts sent the commands.

Anvil reported "84 expected byte range(s) match" and "2 patch(es)". The
session was closed by the script after the PID, path and start-time check;
the save was unchanged, `settings.txt` differed only in `graceful_exit` and
was restored, and the Process Lasso rule was `eu4.exe,0,1-7` afterwards.

- **Success.** `ui click speed_controls/button_speedup` changed the speed
  2 → 3 and `button_speeddown` back to 2, 190–200 ms per command with the game
  paused; `mgr+0x130` was the button itself both times.
- **`covered`.** With an event window open, `ui click topbar` (whose
  rectangle centre lay under the window) and the menu bar's war enthusiasm
  icon were refused with `covered_by: "EventWindow[1]/Background"`. With two event windows at
  the same place, the older window's `Background` was `covered` by the newer
  one's (`EventWindow[3]/Background`): the newest event window is on top.
- **`disabled`.** `speed_controls/button_achievements` (state 3).
- **`not_visible`, `not_found`, `ambiguous`.** `IronmanSaveSelectDialog/show_cloud`
  (hidden), `//nothing_here` and `//button_speed*` (two paths) were refused
  without input.
- **`+0x8F`.** Read after the outer frame, the byte was 0 on the target even
  though `mgr+0x130` pointed at it: the game clears it per frame. Tongs does
  not report it.
- **Checked in the hardening pass:** with the virtual pointer over the map
  and nothing under it, `mgr+0x130` read 0 (five reads from outside), and
  `ui click topbar` (centre over the map) was refused with `covered_by:
  null`. At speed 5, running, `ui click` on the speed buttons worked (speed
  5 → 4 in `state` a second later), but the state in the answer, read 0.1 s
  after the click, did not show it yet at about 10 days/s.

## Events (0.6.0)

An event is chosen by clicking its option, as a player does. This is input,
not a game call: it works in ironman and needs `input = true` but not
`game_calls`. Static findings:

- **Open events.** Each event window has its own controller (constructor
  `0x10B9A50`, vtable `0x1D590F8`).
  - The session `*(root+0x1E00)` lists every open pop-up at `+0x78`, so event
    controllers are filtered by vtable.
  - Filled event windows are also listed as `{event, scope, window}` at
    `+0x498`.
- **Controller fields:** window `+0x38`, event `+0x48`, "waiting for a
  choice" `+0x124`, option buttons `+0x208`.
- **Event.** The script id as written in the file (e.g. `flavor_xxx.1`) is
  the `std::string` at `event+0x20`. The numeric id at `+0x14` is assigned at
  load and is not reported.
- **Options** are a vector at `event+0x790`/`+0x798`, in file order. The
  option `name` is at `option+0x88`.
- **Button to option.** Each option button stores `"<numeric id>_<index>"`
  in its widget at `+0x38`, and the click handler `0x10BC220` reads the index
  back from that string. Options hidden by their trigger get no button but
  still use up an index.
- **Option names are not unique:** 26 base-game events repeat a name, and 64
  options have none. Options are therefore keyed by (event id, option
  index).
- **Click delay.** The game keeps option buttons disabled for a short time
  (a float setting at `0x233E854`). Clicking respects that delay; calling the
  handler directly would skip it.

Commands:

- `event list` returns the open events, each with:
  - `id`, title, description;
  - its options: number on screen (from 1), text, `enabled`, `option_index`,
    `option_key` (may be empty or repeated) and widget path.
- `event choose N` or `event choose text^="…"` clicks one option.
  - With several events open, it needs `event=N` or `event=title^="…"`;
    without one it refuses `ambiguous` and lists the events.
  - A disabled option is refused with `disabled`.
  - The answer carries the game date, event and option, and the state after
    the click.

How it runs (`src/game_events.cpp` reads, `src/events.cpp` decides):

- **Main-thread read.** The session's pop-up list (`+0x78`) gives every event
  controller (vtable `0x1D590F8`) in opening order, with "waiting" `+0x124`,
  the window (`+0x38`, a full window object; the widget is `+0x30` into it),
  the event's script id, numeric id and option names, and the option buttons
  (`+0x208`) with their `+0x38` strings. The event window list (`+0x498`)
  gives the windows whose node is not marked as being removed. The define
  `0x233E854` is read as `option_delay`. The widget tree is read in the same
  job.
- **Events.** Only controllers that are waiting and have a window are
  reported, newest first (the newest window is drawn on top, see the live
  check). Title and description are the texts of the window's `Title` and
  `Description` widgets (`interface/eventwindow.gui`). Options are numbered
  in the order of the button list, which is the order on screen;
  `option_index` comes from the button's string when its numeric id matches
  the event's, `option_key` from `event+0x790[index]+0x88`, and text,
  `enabled` and path from the button's widget.
- **`event list`** also reports `controllers` (event controllers in the
  pop-up list, chosen ones included), `listed_windows` (live entries of
  `+0x498`) and `option_delay`.
- **`event choose`** resolves the request against that list, then clicks the
  button with the steps of `ui click` (including `covered`). A hidden option
  (outside the list box) is first scrolled in with the list's scroll bar
  buttons (`scrolled`). Afterwards it
  reads the events on the main thread until the controller has left the
  pop-up list and its window the event window list, up to 3 s: `closed`,
  `chosen_ms` (no longer waiting), `closed_ms` and `closed_frames` (outer
  frames after the hover check). The game date of the click is
  `choice_date`; the state after the choice follows. An event still waiting
  3 s after the click is an error.

### Live check (2026-10-19)

Same session as "Clicking widgets" (`tongs-events-1`). Events were opened with
the console command `event <id>`, typed through `key grave`, `text`,
`key return`, `key grave`; the command was accepted without
`enable_all_commands`.

- **Fields.** `event flavor_rus.129` opened "Catherine the Great" with its
  description and three options, `flavor_rus.129.a`–`.c` at indexes 0–2 (the
  option behind a Rights of Man trigger was shown; the DLC is installed).
  `event 4011` gave `id: "4011"` (a numeric id comes back as its digits) with
  `EVTOPTA4011`–`EVTOPTC4011`, and `event 9022` likewise. Texts matched the
  screenshot.
- **Lists.** With one, two and three events open, `controllers` and
  `listed_windows` were 1, 2 and 3, one window per event on screen; after the
  last choice both were 0.
- **Option delay.** `option_delay` was 0.5. Polled after `key return`, the
  options were disabled at 200–234 ms and enabled at 516–546 ms; an
  `event choose 1` sent right after the console closed was refused
  `disabled` five times and succeeded 485 ms later.
- **Choosing.** `event choose 1` with two events open was refused
  `ambiguous` with both events; `event choose text^="only reform the army"
  event=title^=military` and `event choose 2` (one event left) clicked the
  right options. `game.log` has "Selected event option N for event X" for
  each choice of the session, with the same indexes.
- **Closing.** `chosen_ms` was 12–13 and `closed_ms` 42–44, `closed_frames`
  3: a chosen event leaves both session lists within three outer frames
  (paused, about 63 frames/s).
- **`run_until events=first`.** 17 March → 1 June 1719 at speed 5, with a
  second client typing `event 9022` into the console 3 s in (1719.4.19): the
  run chose option 1 on 1719.4.25 and reached the target, `inputs` 5 (the
  other client's four commands and the choice). Then 1 June 1719 → 1 December
  1720: three events opened on their own (`trade.8` on 1720.7.29,
  `ideagroups.1309` on 1720.11.1, `patriarch_events.1` on 1720.11.27), each
  was chosen, `inputs` 3, `paused_ms` 0, 10.0 days/s. The game did not pause
  for any of them, so the resume after a pausing event was not exercised.
- **Final build.** A second session (`tongs-events-2`, same setup) ran the
  branch after two answer fields changed: both speed buttons by `ui click`,
  `event choose 1` on `event 4011` (refused `disabled` until 469 ms), and
  `run_until 1719.7.1 speed=5 events=first` across a console `event 9022`
  (chosen on 1719.6.14, `inputs` 5). In an earlier run of that session the
  console line was typed in the last half second before the target date;
  no event opened, during the run or after it, and `game.log` has no trace
  of it. The cause was not investigated; typing into the console at speed 5
  is outside what Tongs checks.
- **Hardening pass.** Events with more options than the list box shows:
  the game makes a button for every option and hides those outside the box
  (event 5060 with seven options for Russia: the seventh `visible: false`).
  `event choose` now clicks the list box's own `downButton` / `upButton`
  (with the steps of `ui click`) until the option is shown, at most 30 times,
  and answers `scrolled`; live, `event choose 7` answered `scrolled: 1` and
  closed in 3 frames. The auto-selection at the deadline was seen in 0.8.0
  ("Auto-selected event option 0", about four months).
- **Not checked:** an event that pauses the game. With the lab's message
  settings no event paused in two years of the Russia save; the pause that
  came (1721.4.25) was from message pop-ups (`EndOfLandCombatPopup`,
  `DefaultPopup`), which set the player pause flag. Making events pause needs
  the game's message settings changed, which the lab's rules do not allow.
  Also open: an event whose options are never enabled.

## Game calls (0.7.0)

The first and only kind in draft 4 is a console command. The research found
the console statically; the 0.7.0 live check (below) confirmed the parts
marked **live**.

- **Entry point:** `0x1721320(manager, result, tokens)`.
  - `manager` is the pointer at `0x2357B58`. A null manager, or one with an
    empty entry vector, is refused. Tongs does not call the getter `0x6E74C0`,
    because it would create a manager with its gate unset.
  - `tokens` is a `std::vector<std::string>` of the line split on spaces, which
    Tongs builds itself.
  - `result` is 0x28 bytes: a success byte and a `std::string` at `+8`, freed
    with `0x95660`. It is the hidden return value; the dispatcher builds it,
    so Tongs passes it zeroed. The success byte is not reliable (`speed`
    never sets it, **live**), so the answer carries the text and the state
    read afterwards.
- **No console window needed.** `run_commands`, `runyear`, `auto_run` and map
  clicks call the dispatcher the same way. History, `console_history.txt` and
  the log line are added only by the window path `0x166DE20` (**live**: no
  line in the window, `console_history.txt` unchanged).
- **Registry.** A static table of 378 records of `0xA0` bytes at
  `0x1FC23E0`, registered by `0x6E90F0`, plus engine commands. The manager
  holds entries of `0xA8` bytes: name `const char*` at `+0x08`, alias count
  at `+0x10`, up to three aliases from `+0x18`; the lookup `0x1721000`
  compares name, then aliases, entry by entry, with `_stricmp`. **Live:** 388
  entries in a loaded game.
- **Gates inside the dispatcher:**
  - manager `+0x18`, the developer gate on 54 commands, which only
    `enable_all_commands` opens (**live**: 1, and `debug_zoom` answered
    "Command available only for developers.");
  - manager `+0x19`, the multiplayer/ironman gate, computed at registration
    as `0x6EAA10(session)` or root byte `+0x24D0` (**live**: 0 on the lab's
    single-player save).
- `run <file>` (`0xE4F1D0`) runs script effects from a file the game itself
  resolves. Tongs does not write script files.

`console LINE` is a game call. The safety model is
[ADR 0001](adr/0001-game-calls-in-tongs.md):

- **Switches.**
  - Game calls need `game_calls = true`; main-thread reads need only
    `enabled`.
  - `plugin.toml` declares `affects_gameplay = true`; since Anvil 1.0 Anvil
    also suspends Tongs in ironman and multiplayer unless the player allows
    it (see "Anvil 1.0 (plugin API 2)").
- **Always refused:**
  - `enable_all_commands`;
  - commands that act on the user's account: `debug_achievements_clear`,
    `test_achievement`, `debug_achievements`, `social_*`,
    `CrashReporter.UploadCrash`.
- **Refused by default:** the other `danger` commands, which
  `console_allow = [...]` allows by name: `oos`, `startup_screen`,
  `reinitchat`, `disable_dlc`, `spawn_all_units`, `bearhaslanded`, `leak`,
  `leak_gpu`, `low_memory`, `map_random`, `debug_crash`,
  `set_achievements_ok`, `purge_mem`, `deiron`, `syntheticdawn`,
  `CrashReporter.SimulateCrash`. 0.7.0 adds `run_commands` and `runyear`:
  they run a file of console lines through the dispatcher directly, past
  every refusal here, so they would reopen `enable_all_commands`.
- **Ironman and multiplayer.**
  - Tongs refuses every game call in either mode, checked before each call.
  - `game_calls_ironman` and `game_calls_multiplayer` lift that refusal;
    both are off by default.
  - With either on, the answer carries `mode`.
- **Logging.** Every game call and every refusal is logged with the command,
  the pipe instance, the game date and the result.
- **Counts.** Game calls are counted in `game_calls` and `game_calls_total`.

How it runs (`src/protocol.cpp`, `ConsoleCall`; decisions in
`src/console.cpp`, memory and the call in `src/game_console.cpp`):

1. On the pipe thread: `game_calls` must be on; the line is split on runs of
   spaces and tabs (printable ASCII only, at most 1024 characters and 32
   words); the typed name is checked against the refusals; a tripped plugin
   refuses `game_calls_tripped`.
2. Under the input lock, a `Kind::GameCall` job is queued (limit 10 s). On
   the main thread it reads the state (`not_in_game` without a loaded game),
   the manager, its entries and both locks, the ironman byte and
   `0x6EAA10(session)`, and refuses by mode. It resolves the typed name in
   the manager's entries as the game's lookup does (`unknown_command` when
   none matches, also when the developer gate is open and the game would try
   its console variables) and checks the refusals again by the entry's own
   name, so an alias (`crash` names `debug_crash` first) is refused by the
   command it stands for.
3. The tokens are built by hand in the game's MSVC layout (a 32-byte string:
   inline buffer or pointer, size, capacity; a vector of three pointers),
   independent of the plugin's own STL settings. Short tokens are copied
   inline, long ones point into Tongs' strings; the dispatcher only reads
   them and copies the arguments it passes on.
4. The call runs under `__try`. A structured exception that leaves the
   game's code answers `faulted` with its code and trips game calls until the
   game exits; reads keep working. Otherwise the text is copied (guarded,
   up to 1 MiB) and freed with `0x95660`.
5. The answer has `name`, `text` (UTF-8 without colour codes),
   `success_hint`, `console` (`commands`, `developer_lock`, `mode_lock`),
   `game_calls_total`, `queued_ms` / `run_ms` / `frame` and the state read
   after the job. A call counts in `game_calls` and `game_calls_total` once
   the game's code has run, whatever its text says.

`game_calls_total` counts per loaded save. Reloading the same save from the
in-game menu keeps the session object at its address and the date where it
was (live, `tongs-harden-1`: the count stayed 1), so neither tells loads
apart. `src/load_watch.cpp` does: every state read is observed, and a read
that sees a loaded game after one that saw none starts a new load; another
session object or an earlier date also does. With `game_calls = true` a Tongs
thread reads the state every 20 ms, so the second without a session while a
save loads is always seen. Tongs keys the count by load, session object and
date, and starts again from 0 when any of them says another save.

### Live check (2026-10-19)

Setup:
- the original `eu4.exe` (`9AD3EFE1…771A`), Anvil 0.2.0 and the 0.7.0 branch;
- the Russia save of 17 March 1719, single player, not ironman, loaded with
  `continue=`;
- the lab's `anvil-play.ps1 -Scenario tongs -TongsInput -TongsGameCalls`
  (`tongs-console-1`) under Process Lasso's `1-7`. `-TongsGameCalls` is a
  new switch of the lab launcher that writes `game_calls = true` to the
  config; Python scripts sent the commands.

Anvil reported "93 expected byte range(s) match", "2 patch(es)" and "declares
gameplay changes"; Tongs logged "game calls enabled" as a warning. The session
was closed by the script after the PID, path and start-time check; the save
was unchanged, `settings.txt` differed only in `graceful_exit` and was
restored, the EXE hash was unchanged and the Process Lasso rule was
`eu4.exe,0,1-7` afterwards.

- **Before the save is in.** While the save loaded, `console echo early`
  answered `main thread busy` after 10 s (no outer frames). In the first
  outer frames (frame counter 2 and 4), while another class still sat at
  `root+0x1E00`, it was refused `not_in_game` on the main thread without
  calling the game. The first call after `in_game: true` worked.
- **Round trip.** `echo x` answered `text: "x"`, success byte 1, `run_ms`
  0–1, `queued_ms` about 14 while paused. `echo a b` answered an empty text
  and byte 0, as the research said (`echo` takes exactly one argument).
  `version` and `help speed` returned the game's text ("EU4 v1.37.5.0 Inca
  …", "speed [<0-5>] - Sets game speed").
- **`speed 3`.** Empty text, success byte 0; the state read afterwards, and
  `state` after it, had speed 3. The command also unpauses the game (speed
  1–5 resume through `0x7E0770`), so `pause` followed. `speed 2` the same.
- **Locks.** `console.commands` 388, `developer_lock` 1, `mode_lock` 0 in
  every answer.
- **Refusals.** `enable_all_commands` and `ENABLE_ALL_COMMANDS 1234`
  (always), `social_debuginfo` (always), `leak`, `oos` and `run_commands
  nothing.txt` (danger, with the `console_allow` hint) were refused on the
  pipe thread without a main-thread job; `nosuchcommand` answered
  `unknown_command` from the main thread; an empty line `bad_command`.
  `debug_zoom` went to the game, which answered "Command available only for
  developers." (byte 0); it counts as a game call.
- **No trace in the console.** After ten calls, `key grave` opened an empty
  console window (screenshot), and the SHA-256 of `console_history.txt` was
  the same as before the session. Typing `echo typed_check` into the window
  added it to `console_history.txt`; neither the typed line nor any direct
  call left a "Console command …" line in any file under `logs/`, so the
  research's H2 (`game.log`) does not hold.
- **Main thread.** `frames` and `threads` reported the same main thread
  (66480) that owns the window; game calls run in its outer frames by
  construction.
- **Marked runs.** `run_until 1719.5.1 speed=2` with `console echo
  during_run` from another client 2 s in: `game_calls` 1, `inputs` 0; the
  next `run_until 1719.5.15` had `game_calls` 0. During `run_until 1719.7.1
  speed=5`, three calls from another client waited 21–64 ms for a frame and
  the run reported `game_calls` 3. `game_calls_total` in `state` went from 1
  to 13 over the session, one per call that reached the game.
- **Log.** Every call and refusal was in `anvil.log` with the line, the pipe
  instance (1–4), the game date (or "no game") and the result; refusals as
  warnings. Multi-line texts broke the log line, so the final build escapes
  them.
- **Final build.** A second session (`tongs-console-2`, same setup and
  cleanup) ran the branch with that change: `version` was one log line;
  `echo final`, `speed 3` (speed 3 in `state`, then `pause`), refused
  `enable_all_commands` and `leak`; `crash` resolved on the main thread to
  `debug_crash` (the first entry with that alias) and was refused there; a
  call during `run_until 1719.7.1 speed=3` gave `game_calls` 1, `inputs` 0;
  `console_history.txt` was unchanged.
- **Hardening pass.** In the main menu `console echo menu` was refused
  `not_in_game` on the main thread. After a save loaded from the menu,
  `game_calls_total` was 0; reloading the same save from the in-game menu kept
  it at 1 until the load watch (above), then 0. `clear` with the console
  window open emptied its list (one entry to none), with it closed answered
  an empty text; `run` resolves its argument through the game's file system
  (it appends `.txt`): `run common/ai_army/01_ai_army_province_peace.txt` (a
  base-game file of comments only) answered "... executed" with `success_hint`
  true, `run tongs_no_such_file.txt` an empty text and false. No script file
  was created.
- **Not checked:** ironman and multiplayer (the lab has one ironman save,
  `Armenia.eu4`, but loading an ironman save makes the game write it; what
  the pre-command hook `0x8740B0` writes in ironman stays open),
  `game_calls_ironman` / `game_calls_multiplayer`, and a game call that faults
  (not forced).

## Notifications (0.8.0)

`subscribe KINDS [date=day|month|year]` turns the connection into a stream of
notifications. The connection still accepts `unsubscribe` and `ping`.

- **Format.** Each notification is one JSON line with:
  - `"notification": "<kind>"`;
  - `seq` (from 1 per connection);
  - the game `date`;
  - `wall_ms` since the subscription.

  Answers keep `"command"`.
- **Kinds:**

  | Kind | What | Source |
  | --- | --- | --- |
  | `state` | entering or leaving the game, pause, resume, speed | the state read every 10 ms from a Tongs thread, as in `wait_until`; works while a save loads |
  | `date` | a new day, month (default) or year | same |
  | `event` | an event opened or closed, with `id`, title and the chosen option when known | the event list compared as a main-thread read after outer frames |
  | `error_log` | new lines of `error.log` | a thread that follows the end of the file |
  | `activity` | every game call and input from any client | Tongs itself |
- **Back-pressure.** Each subscription has a bounded queue of about 1000
  notifications. On overflow the oldest are dropped and
  `{"notification": "overflow", "dropped": N}` follows. Nothing waits for a
  slow reader.

How it works (queue, diffs and formatting in `src/notifications.cpp` and
`src/notify_sources.cpp`, both platform-independent and unit-tested; the
stream in `src/pipe_server.cpp`; the file in `src/game_files.cpp`):

- **Connection.** `subscribe` is handled by the pipe thread of its
  connection. After the answer, the thread loops: it looks for a request with
  `PeekNamedPipe` (so `ReadFile` never blocks there), then waits up to 20 ms
  for notifications and writes what came, up to 256 lines at a time. The pipe
  stays synchronous, one thread per instance. While subscribed, `ping` and
  `unsubscribe` are answered and everything else is refused with
  `subscribed`. **`unsubscribe` returns the connection to ordinary
  requests**; what is still queued is discarded and counted in the answer
  (`sent`, `dropped`, `discarded`). Closing the connection ends the
  subscription as well.
- **Answer.** `subscribe` answers with `kinds`, `date_unit` (with `date`),
  `queue` (1000), `unavailable` (kinds this build cannot serve, e.g. `event`
  without main-thread reads), `error_log` (`path` and `source`, or `error`) and
  the fields of `state`: the state that the first `state` notification
  changes.
- **Queue.** Publishing copies the notification's members into the queue of
  every subscription that wants its kind and never waits for a reader; the
  date and `wall_ms` are taken then. `seq` is given when the line is written,
  so it has no gaps; after a drop the `overflow` notice (with the date and
  `wall_ms` of the last dropped notification) takes the next number. A pipe
  thread blocked in `WriteFile` on a client that does not read holds only its
  own instance.
- **Baselines.** A source that starts (because a subscription now wants its
  kind) first reads what is, without reporting it; only changes after that
  are sent. A source with no subscription for 1 s forgets its baseline.
- **`state` and `date`.** A Tongs thread reads the state every 10 ms (as
  `wait_until` does). `state` reports, in this order, `left_game` (with
  `reason`), `entered_game`, `paused` / `resumed` (the game's own `paused`)
  and `speed` (with `previous_speed`); each carries `speed`, `paused` and
  `paused_flag`. Another session object at the next read counts as leaving
  and entering. `date` is sent to each subscription whose step (`day`,
  `month` or `year`) the new date crosses, with `unit` and `previous`; one
  notification per read, however many days passed, and never for a date that
  goes back. While a save loads the thread keeps reading, so `entered_game`
  comes without waiting for outer frames.
- **`event`.** Another thread reads the count of the session's event window
  list (one word, as `run_until events=first` does) every 10 ms. When it
  changes, a main-thread read takes the event list and, while events are
  open, the widget tree (6–7 ms in 0.5.0), and compares it with the events
  known (by controller and script id). Once a second while events are open
  it reads the list alone and the tree only when the list differs. `opened`
  carries `id`, `title` and `options` (`number`, `text`, `option_index`,
  `option_key`); `closed` carries `id`, `title` and `chosen`. `chosen` is
  the option when Tongs clicked it (`event choose`, `run_until
  events=first`, or `ui click` on the option button, which adds `via: "ui
  click"`): Tongs notes the option right after the click, with `by:
  "tongs"`, the `client` and `choice_date`. A choice with the real mouse, a
  plain `click`, or the game's own choice at the deadline, is not known and
  gives `null` ([#49](https://github.com/happy-dwarf-team/tongs/issues/49)). Without a
  session (count −1), every known event closes. The main thread only runs the
  reads; it never waits for the thread.
- **`error_log`.** A third thread follows `error.log` every 100 ms (at once
  again after a full 1 MiB read). The file is opened for each read with every
  sharing mode, so the game can still write, rename or delete it, and closed
  again. Lines are split on `\n` (a `\r` before it dropped), kept when they
  are valid UTF-8 and read as Windows-1252 otherwise, and cut at 4096 bytes.
  A smaller size or another file identity (volume serial and file index)
  means the file was replaced: `restarted: true` is sent and reading starts
  at 0. A missing file is followed from its start once it appears.
- **Where `error.log` is.** The game writes its logs to `logs/` in PhysFS's
  write directory, which it sets at start-up from the Documents folder or
  from `userdir=` / `userdir.txt` (Bellows found `logs/error.log` there; the
  lab's `userdir.txt` is empty). `eu4.exe` exports `PHYSFS_getWriteDir`, so
  Tongs asks the game (under SEH; the function takes PhysFS's state lock for
  a moment) and uses `<write dir>\logs\error.log` (`source:
  "physfs_write_dir"`). Before the game has set it, the subscription answers
  an `error` and the thread asks again every second. Before Anvil 1.0, Tongs
  used `SHGetKnownFolderPath(FOLDERID_Documents)\Paradox Interactive\Europa
  Universalis IV` (`source: "documents"`) without the export; since then the
  export is the symbol `physfs_get_write_dir`, which Anvil resolves before
  loading, so that fallback is gone.
- **`activity`.** Published by the commands: an input when it counts in
  `inputs` (`what: "input"`, `command`, the running total `inputs`), a game
  call when it counts in `game_calls` (`what: "game_call"`, `line`, `name`,
  `game_calls`); both with `client`, the pipe instance. Refusals and the
  presses `run_until` makes for itself are not activity, as they are not
  counted.

Live checks in the 0.8.0 PR:

- every kind on a running game;
- overflow with a client that stops reading;
- `error_log` lines from what the game writes anyway (mods are not changed to
  provoke errors);
- 8 clients at once.

### Live check (2026-10-19)

Setup:
- the original `eu4.exe` (`9AD3EFE1…771A`), Anvil 0.2.0 and the 0.8.0 branch;
- the Russia save of 17 March 1719, single player, loaded with `continue=`,
  with the lab's usual ten mods (unchanged);
- the lab's `anvil-play.ps1 -Scenario tongs -TongsInput -TongsGameCalls`
  under Process Lasso's `1-7`: `tongs-notify-1` (a Python script with
  `scripts/tongs.py` as a module) and `tongs-notify-2` (the command-line
  clients).

Anvil reported "93 expected byte range(s) match", "2 patch(es)" and Tongs
"serving \\.\pipe\tongs, 8 clients at a time". Both sessions were closed by
the script after the PID, path and start-time check; the save was unchanged,
`settings.txt` differed only in `graceful_exit` and was restored, the EXE hash
was unchanged, the Process Lasso rule was `eu4.exe,0,1-7` afterwards and no
installed file was left.

Findings (`tongs-notify-1` unless noted):

- **8 clients.** Eight connections were open at once: subscribers to `all
  date=day`, `state,date`, `event`, `error_log` and `activity`, a sixth
  subscriber (`all date=day`) that stopped reading, and two ordinary clients.
  A ninth could not connect (the client gave up after 2 s); in
  `tongs-notify-2` a ninth connected as soon as one of eight closed. An
  unsubscribed connection stays open and keeps its instance. The second
  ordinary client read `state` every 0.2 s throughout: median 0.06 ms, at
  most 8.1 ms while the slow subscriber was blocked (258 answers), at most
  4.7 ms afterwards.
- **`state` while loading.** The first subscribers came in while the root
  object was still null. `entered_game` came 27.5 s after the pipe answered,
  4 s before the first main-thread answer (`frames` answered `main thread
  busy` until then), and `paused` 1.5 s later, when the game paused itself.
  Each `speed`, `resume`, `pause` and `run_until` step gave its own
  notification (11 in all, e.g. `speed` 3 → 4 → 5 and `resumed` when
  `run_until` started), and `tongs-notify-2`'s PowerShell stream had
  `resumed` and `paused` for the other client's commands.
- **`date`.** The `date=day` subscriber got 471 notifications from 1719.3.18
  to 1720.7.1: every day once, also at speed 5 (about 10 days/s), none
  skipped. The month subscriber got the 16 first days of month, each with the
  last day of the previous month as `previous`.
- **`event`** (8 notifications).
  - `parlaments.1` opened on 1719.4.26 during `run_until` at speed 5 and
    closed on 1719.8.27 with `chosen: null`; `game.log` has "Auto-selected
    event option 0 for event parlaments.1": the game chose itself after four
    months (the deadline that 0.6.0 left unchecked). `orthodox_flavor.4`
    opened on 1719.9.16 and stayed open.
  - `console event flavor_rus.129` and `console event 4011` opened their
    events about 30 ms after the call's answer, with titles and options.
  - `4011` was chosen with `ui click` on its first option: `closed` with
    `chosen: null`, as Tongs does not know that option. `flavor_rus.129` and
    `orthodox_flavor.4` were chosen with `event choose`: `chosen` named the
    option with `by: "tongs"`, `client` 4 and `choice_date`, and came 15 ms
    after the command's answer. `game.log` has "Selected event option 0" for
    all three.
- **`error_log`.** The file was `Documents\Paradox Interactive\Europa
  Universalis IV\logs\error.log` with `source: "physfs_write_dir"`, already
  in the first `subscribe` answer before the root object existed. The `all`
  subscriber got 178 lines: lines 49–226 of the 227 of the `error.log` copied
  after the session, in order, nothing missing or extra (the first 48 were
  written before it subscribed). They were what the mods write: duplicate GUI
  types, script errors, `GetRelativeStrength() is zero`. The `error_log`
  subscriber, which came later, got the last 136 of them. `restarted` did not
  occur (the game moves the file aside at start, before these subscriptions).
- **`activity`.** 1109 notifications: 6 inputs (`speed`, `resume`, `pause`,
  `ui click`, two `event choose`) and 1103 game calls (two `event`, 1100 `echo`
  and one more `echo`), each with `client` 4 and the running count;
  `game_calls_total` was 1103 at the end.
- **Overflow.** The slow subscriber read nothing for 54 s, while `run_until`
  ran 7 months and another client made 1100 `echo` game calls (18.0 s, one per
  outer frame, paused). Then it read 1031 lines: seq 1–30 (what the pipe's
  buffer took before the write blocked), `overflow` with `dropped` 397 as seq
  31 (date 1719.11.1, `wall_ms` 37 490), then the 1000 newest, with no gap in
  `seq`. `unsubscribe` answered `sent` 1031, `dropped` 397, `discarded` 0, and
  `state` worked on the same connection.
- **No waiting for the slow reader.** `run_until` 1719.3.23 → 1719.11.1 at
  speed 5 ran at 10.3 days/s with the slow subscriber blocked, and 1719.11.1 →
  1720.7.1 at 10.0 days/s after it had gone; one run each on CPU 1–7, not a
  `SIMULATION` measurement.
- **`unsubscribe`.** All five recorders unsubscribed at the end (`sent` equal
  to what they had read, `dropped` 0) and then read `state` on the same
  connection.
- **Clients** (`tongs-notify-2`). `tongs.py "subscribe event" --seconds 3` and
  `tongs.ps1 -Subscribe event -Seconds 3`, with no notification at all,
  unsubscribed and exited after 3.1 and 3.4 s. `tongs.ps1 state -Subscribe
  'state,date date=day' -Seconds 8` printed `resumed`, three days and
  `paused` while another client resumed and paused; `tongs.py "subscribe date
  date=day" --count 5 --seconds 20` stopped at 20 s with the three days there
  were. `subscribe nothing` answered `bad_command`.

Checked in the hardening pass: a save loaded from the main menu gave
`entered_game` (no `left_game` before it: the menu is not a game); reloading
the save from the in-game menu gave `speed` 2 → 1, `left_game` ("session is
null"), `entered_game` and `speed` 1 → 2. Before the fix, `entered_game` came
with the date 1.1.1 while the save was still being read; with it, at
1719.3.17. A choice by `ui click` on an option is named (`via: "ui click"`).

Not checked: `restarted` on a live game, an event that pauses the game (see
"Events"), a choice with the real mouse or the game's choice at the deadline
([#49](https://github.com/happy-dwarf-team/tongs/issues/49)), ironman and
multiplayer, and the cost of the event reads at speed 5 under `SIMULATION`
conditions.

## MCP adapter (0.9.0)

`mcp/` holds an MCP server (Python, MCP Python SDK 2.3.0, stdio only) that
turns the pipe into typed tools for agents: one tool per command family and
`tongs_raw` for any one line. It was left out of draft 4 as a separate thin
project; it lives in this repository instead, next to the client it reuses.

- No new power: each tool sends one command over a connection of its own and
  returns the answer line unchanged (and parsed, with `isError` for
  `"ok": false`). `input` and `game_calls` in the plugin's settings still decide.
  Arguments with line breaks are refused, so a tool cannot send two commands.
- `scripts/tongs.py` is copied verbatim into the package (a test keeps the two
  equal), so the server installs on its own with `uvx` or `uv sync`.
- Long waits: when the client asks for progress, `wait_until` / `run_until`
  open a second connection with `subscribe date date=day` and report the game
  days as MCP progress. Cancelling the tool call sends `cancel`, which ends
  every wait in progress.
- Releases: no new asset. release-please bumps `mcp/pyproject.toml` and
  `mcp/uv.lock` with `version.txt`; users run the server from a release tag.

Install, tools, client timeouts and the live check (2026-10-19, game driven
through the server over stdio by a Python MCP client): [`mcp/README.md`](../mcp/README.md).

## Hardening pass (0.10.0)

On 2026-10-19 the open points of the sections above were checked live, the
first time with Claude Code itself as the MCP client (`tongs_*` tools; see
[`mcp/README.md`](../mcp/README.md)). Fixes and additions went into the
release after 0.10.0; the sections above describe them.

Setup:
- the original `eu4.exe` (`9AD3EFE1…771A`), Anvil 0.2.0, Tongs from `main`
  (0.10.0) in `tongs-harden-1`, then the two hardening branches;
- the lab launcher `anvil-play.ps1 -Scenario tongs -TongsInput
  -TongsGameCalls` under Process Lasso's `1-7`, with the lab's new
  `-NoContinue` (no `continue=`: the game stops in the main menu) for
  `tongs-harden-1`, `-2` and `-4` (`-4` without game calls), and with
  `continue=` for `-3`;
- the Russia save of 17 March 1719, loaded from the menu by `ui click` alone
  (Single Player, the saved-games tab, "Show incompatible saved games", the
  list's scroll bar buttons, the row, PLAY) and reloaded from the in-game
  menu (Load Game, the row, Load, Ok);
- read-only reads of the game's memory from outside (`ReadProcessMemory`) to
  find the check box and edit box offsets and `mgr+0x130`.

Each session ended with the save unchanged, the EXE hash unchanged,
`settings.txt` differing only in `graceful_exit` (restored) and the rule
`eu4.exe,0,1-7`. In `tongs-harden-3`, "Exit to Menu" and then Quit on the
end-of-game summary made the game start itself again (`-skiplauncher`,
`continue=` of the original command line) as a child of the verified
session, with Anvil loaded; the launcher could not remove `version.dll`. The
child was closed with the lab's verified-session functions (PID, path and
creation time) and the files removed by hand.

| Area | Result |
| --- | --- |
| Main menu | ready about 37 s after start; `state` "session object has an unexpected vtable"; `ui tree` read `country_selection_panel/mainmenu` and the lobby; `console` refused `not_in_game` |
| Load from the menu | PLAY → 3.5 s `main thread busy`, then outer frames 16/s → 75/s; `entered_game`, then the game's own pause 1.5 s later; `game_calls_total` 0 |
| Reload of the same save | session object at the same address, date the same: `game_calls_total` stayed 1 (fixed: load watch); `entered_game` came at 1.1.1 (fixed) |
| Window | the whole of `tongs-harden-1` ran unfocused: `frames`, `ui find`, `ui click`, `screenshot` (`print_window`) all worked; minimizing did not happen (borderless window) |
| `mgr+0x130` | 0 with nothing under the pointer; `covered_by: null` |
| Speed 5 | `ui click` on the speed buttons worked while running |
| `ui click` | the first move after a click on a scroll bar button sometimes found nothing hovered (fixed: up to three tries, then a step aside); a row's own label covers the centre of its check box, so clicking the row window works |
| List boxes | rows are made only for what is shown and refilled as the list scrolls; the wheel did not scroll the save list ([#51](https://github.com/happy-dwarf-team/tongs/issues/51)); scroll bar rects were relative (fixed) |
| Widgets | check box `+0x100`, edit box `+0xF8` (added); overlapping elements boxes `0x1DA4910` and grid boxes not read ([#48](https://github.com/happy-dwarf-team/tongs/issues/48)); `.gui` sizes ([#50](https://github.com/happy-dwarf-team/tongs/issues/50)) |
| Events | seven options, one hidden: `event choose 7` refused `not_visible` (fixed: scrolls); `ui click` on an option gave `chosen: null` (fixed); no event paused the game |
| Game calls | `clear`, `run` with a base-game file; no faulting call forced |
| `move` | answered `in_game: false` with an empty reason (fixed) |
| `scripts/tongs.py` | failed on `£` when piped (fixed) |

Also seen: after a click on an incompatible save in the lobby, the main
thread stayed busy for minutes (`tongs-harden-4`, until the launcher's limit);
loading an incompatible save's summary was not investigated.

## Anvil 1.0 (plugin API 2)

Tongs moved to plugin API 2 for Anvil 1.0 and is released with it
([tongs#57](https://github.com/happy-dwarf-team/tongs/issues/57); the
Anvil side is Anvil's `docs/design.md`, §4, §5, §8, §9, §11, §13 and §22.2).
API 1 is gone: Anvil 1.0 skips an `api = 1` plugin, and Tongs 1.0 needs
Anvil 1.0.

### Manifest

`plugin/plugin.toml`: `api = 2`, `anvil = ">=1.0.0"`, `symbols`,
`settings`, `affects_gameplay = true`, `allow_ironman` and
`allow_multiplayer = "opt-in"`, and a dependency on `anvil-game` `^1`.
`game_builds` and the 93 `[[expect]]` entries are gone; the build comes from
`symbols.toml`.

### Symbols

`plugin/symbols.toml` declares every address Tongs uses, and the offsets the
code reads from them as `values` (the number inside the declared bytes, or
for `rip_relative` the RVA an instruction addresses). Anvil verifies each
symbol before it loads `tongs.dll` (bytes, memory kind, pointer targets,
values); `anvil gen` writes them into `symbols.gen.hpp`, and the layouts in
`src/game_*.hpp` take their numbers from there (`sym::root_and_date.date`),
not from a second copy in the code.

- 89 of the 93 `[[expect]]` entries became symbols with the same bytes (8
  of them longer by a few bytes, because `anvil check` requires whole
  instructions in declared code). Every value was checked against the
  constant the code used before.
- 4 were dropped: the frame function's outer flag (`0x14C25BA`), the main
  loop call (`0x14C2E0E`), the first-start language loop (`0x2006D0`) and the
  frame counter (`0x14C2A23`). They proved what Tongs' own frame hook relied
  on, which is now `anvil-game`'s, and `anvil-game` declares the frame
  function, the counter and the quit flags itself.
- New: the functions Tongs calls or hooks get a `signature`
  (`is_paused`, `sdl_push_event`, `gui_cursor` (the hooked cursor query,
  before only a prologue check of the hook), `console_dispatch`,
  `string_free`, `is_multiplayer`), `physfs_get_write_dir` by its export
  name (before `GetProcAddress`), the session vtable slot that tells the
  game's session class (`session_speed_slot`, as in `anvil-game`; before an
  unchecked constant), and the first store into each worker pool
  (`worker_pool_1`, `worker_pool_2`; the pool's RVA and its vector's offset
  were unchecked constants).
- The console registration's 63 bytes are two symbols, its first 40 bytes the
  same range as `anvil-game`'s `ironman_gate`, so `anvil check --game` sees one
  shared range instead of an overlap.
- Still in the code, marked "not in symbols.toml", because no declared
  instruction holds them: the widget vtable slots for position and size
  (`0xD8`, `0xE0`), the window's maps and sub-windows (`0x798`, `0x958`, their
  counts and strides), the vector-pair stride and counts, the render node's
  parent (`0x110`), the edit box's text (`0xF8`), the check box's byte
  (`0x100`), the console entry vector at the manager's start, a session list's
  head 8 bytes before its tail, and SDL 2.0.4's own event layout. They were
  checked live in their releases, as before.
- `plugin/symbols.lock` records the check against `eu4.exe`
  `9AD3EFE1…771A` (`anvil check plugin --exe`); CI and `ctest` fail when it
  does not match `symbols.toml`.

### Frames and the main thread

`anvil-game` hooks the frame function and publishes `anvil-game.frame` after
each frame of the main loop, on the main thread, while the game is not
quitting: the outer frame of the glossary, with the same three conditions as
Tongs' hook had. Tongs runs the dispatcher's batch in that event with the
event's frame number (`app+0x320`, as before) and counts the frame for
`frames`; its own hook of the frame function is gone.

`anvil-game` does not report nested frames. Tongs counts them with a mid hook
at their only call site (`nested_frame`, `0x769FA6`, the call of the frame
function with `outer` = 0 in `0x769EA0`), so `frames` keeps `nested_per_s`
and `nested_total`. The count now starts just before the nested frame instead
of after it, and outer frames are counted only while the game is not quitting.

Not used: `anvil-game`'s `run_on_main_thread`. Tongs keeps its dispatcher,
which withdraws jobs past their limit, cancels them, and ends a batch after a
game call; the work queue has none of that.

### Settings

`plugin/settings.toml` declares `enabled`, `input`, `game_calls`,
`console_allow` (a string: names separated by commas or spaces, since
settings have no lists), `game_calls_ironman` and `game_calls_multiplayer`,
all off and `restart = true`: Tongs reads them once while loading. The
player's values are in `anvil/data/tongs/settings.toml`;
`ANVIL_TONGS_<NAME>` variables override them for one start.

Migration: when `anvil/data/tongs/config.toml` exists and `settings.toml`
does not, Tongs writes the config's non-default values to `settings.toml`
(through a temporary file), renames the config to `config.toml.migrated`,
logs it, and uses the config's values already in that start (Anvil read the
defaults before the file existed), except where an `ANVIL_TONGS_<NAME>`
variable is set. With both files present, `config.toml` is ignored with a
warning.

### Ironman and multiplayer

Anvil suspends gameplay plugins in an ironman or multiplayer session unless
the player allows them in `anvil/anvil.toml` (`allow_ironman = ["tongs"]`,
`allow_multiplayer = ["tongs"]`; Tongs offers both as `"opt-in"`). Suspended,
Tongs' cursor hook and nested-frame counter are out of the game, and Tongs
gets `anvil.suspended`: until `anvil.resumed` its input commands (except
`release`) answer an error and `console` answers `suspended`, while reads
(`state`, `ui`, `event list`, `frames`, notifications) go on; `help` reports
`suspended`. Tongs' own
`game_calls_ironman` / `game_calls_multiplayer` stay a second layer: a game
call in ironman needs both the player's opt-in in Anvil and Tongs' switch.

### Build

Tongs builds with the Anvil SDK (`anvil_add_plugin` reads `plugin/plugin.toml`,
runs `anvil gen`, builds `tongs.dll` with the static CRT and lays out
`build/plugin/tongs/`). `cmake/AnvilSdk.cmake` takes the SDK archive of a
pinned Anvil release (URL and SHA-256); `-DTONGS_ANVIL_SDK` points at an
unpacked SDK instead. CI runs `anvil check plugin --format github`, which also
fails on a stale `symbols.lock`.

## Releases

Each release is one PR with its live checks done before it merges; the order
is read-only first, then input, then game state.

| Release | Content | Live checks |
| --- | --- | --- |
| 0.4.0 | dispatcher after the outer frame, `frames`, `queued_ms` / `run_ms` / `frame` | outer frames, gaps at speed 5, hook cost; decides the opt-out |
| 0.5.0 | `ui tree`, `ui find`: selectors, widget paths | coordinates and scale, top-level windows, text encoding |
| 0.6.0 | `ui click SEL`, `event list` / `event choose`, `run_until events=first`, `inputs` | hover check, event lists, option click delay |
| 0.7.0 | `console` game calls with ADR 0001, `game_calls`, `affects_gameplay = true` | handlers on the main thread, frontend, ironman/cheated flags |
| 0.8.0 | `subscribe` with five kinds, 8 pipe instances | every kind, overflow, `error_log`, 8 clients |
| API 2 | plugin API 2 with Anvil 1.0 (released with it): `symbols.toml`, `anvil-game` frames, `settings.toml` with migration, suspension in ironman/multiplayer | load and symbols, every command family, migration, `frames` outer/nested, suspension (see "Not verified yet") |

Not in draft 4:

- direct calls of game functions other than the console;
- scripts supplied by clients for `run`;
- text fields and focus beyond `ui click` + `text`;
- the map and provinces;
- notifications for arbitrary windows;
- `nowait` for main-thread jobs;
- an MCP adapter (added after draft 4 in `mcp/`, see "MCP adapter");
- a per-day tick profile;
- state queries through scripted triggers;
- screenshots of single widgets.

## Not verified yet

- Anvil 1.0 (plugin API 2): nothing of it ran in the game yet. To check:
  Tongs loads after `anvil-game` with 95 symbols verified; every command
  family answers as with 0.12; `frames` reports outer and nested frames (the
  nested count from the new mid hook); a 0.12 `config.toml` is moved into
  `settings.toml` and applies in that start; and in an ironman game without
  the opt-in Tongs is suspended (input and `console` refused, reads going on)
  and resumed after the session.
- Stale widgets (0.14.0): reloads of other files than `outliner.gui` and
  `topbar.gui`, list rows built before a reload, and `ui click` refusing a
  stale widget in the game (only unit-tested).
- The open points of each section, under its live checks.
- `paused` for pauses not caused by the pause button: message pop-ups set
  `paused_flag` (hardening pass); events that pause, menus and multiplayer
  were not seen.
- Input in text fields (Space types instead of pausing) and with keyboard
  layouts other than US (SDL key codes and the characters sent follow a US
  layout). Input in the main menu and with event pop-ups works (hardening
  pass).
- Whether the speed keys need `SDL_TEXTINPUT` alone or both events; both are
  sent, as SDL does for a real key.
- Mouse: dragging (no command yet), right and middle clicks, double clicks,
  clicks on the map (only interface buttons were tried), and the focused
  window path of `0x14E5E50` (the game was never focused in the checks).
- `run_until` across an event that pauses the game (no event paused with the
  lab's message settings), with `max_pause` and with `events=first`.
- A minimized game window.
