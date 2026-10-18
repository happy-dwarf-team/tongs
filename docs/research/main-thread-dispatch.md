# Research: main-thread dispatch point

Ticket: happy-dwarf-team/tongs#16 (map #15). Static analysis only, EU4 1.37.5
Steam Win64, `eu4.exe` SHA-256 `9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A`
(checked on the analysed copy before and after). No game process was started.
RVAs are relative to the image base `0x140000000`.

Tools and how a claim was checked, as used below:

- **[G]** Ghidra 12.1.4 decompiler / reference manager on a copy of the lab's
  analysed project.
- **[D]** `dumpbin /disasm` of the copy (instruction bytes and operands).
- **[F]** bytes read from the file (`.rdata` / `.data` initial values, IAT
  order from `dumpbin /imports`).
- **[S]** SDL 2.0.4 sources, tag `release-2.0.4` of `libsdl-org/SDL`:
  `src/dynapi/SDL_dynapi_procs.h` (slot order, counted with the Win64
  `#if` branches: 553 entries, matching the table size `0x1148 / 8` already
  documented), `include/SDL_events.h`, `include/SDL_video.h`,
  `src/video/windows/SDL_windowsevents.c`.
- **[L]** an earlier live observation from the lab repository
  (`docs/simulation-profile.md`, `docs/reverse-engineering.md`).

## Question

Where can Tongs run queued work on the game's main thread once per frame, so
game calls and widget-tree reads never race the game? For each candidate
(event loop `0x1569C70`, cursor query `0x14E5E50`, or a better one): is it
called in the main menu, while a save loads, while paused, while minimized;
how often; is it reentrant; what state or locks the game holds there; what the
`[[expect]]` bytes are.

Short answer: hook the **frame function `0x14C25A0`** and run the queue
**after the original returns, only when its second argument is non-zero**
(outer frame). Both named candidates are called from inside that function,
once per frame, but neither can tell an outer frame from a nested one.

## Verified statically

### The frame function `0x14C25A0`

- `0x14C2D00` is the application loop (already known as the function that
  pins the main thread). After the pinning it loops while the bytes
  `app+0x83` and `app+0x2F8` are zero; each iteration applies a pending
  cursor (`SDL_GetCursor`/`SDL_SetCursor`, dynapi slots 218/217, and
  `SetCursor`) and calls `0x14C25A0(app, 1)`. [G] decompile of `0x14C2D00`;
  [D] `0x14C2E0E: B2 01` (`mov dl,1`), `0x14C2E13: call 0x14C25A0`,
  `0x14C2E18: cmp [rsi+83h],r14b`; slots [S].
- `0x14C25A0` has exactly three direct callers and no other code reference
  (the two other data references are chained unwind entries in `.rdata`,
  `0x1F4F40C`/`0x1F4F41C`, which repeat its `.pdata` range; the image has no
  CFG function table). [G] references; [D] grep of the whole disassembly for
  the address; [F] `.pdata`/`.rdata` words; `dumpbin /loadconfig`.

  | Call site | Caller | Second argument | Context |
  | --- | --- | --- | --- |
  | `0x14C2E13` | `0x14C2D00` application loop | `mov dl,1` | every iteration of the main loop |
  | `0x2006D5` | `0x2003D0` (`eu4application.cpp`, "Used RAM Map at start of Init") | `mov dl,1` | loop while `app+0x5E4` is set, around the language-selection screen (see below) |
  | `0x769FB2` | `0x769EA0` (`gamestate.cpp`) | `xor edx,edx` | nested frame from inside long game updates |

- What one frame does, in order ([G] decompile, [D] call sites):
  1. `SteamAPI_RunCallbacks` (`0x14C25C6`), `0x169A2F0`, `CPopsWrapper::Update`
     (`0x14ADA90`, identified by its own log strings in
     `pdx_pops_wrapper.cpp`; it has a reentry guard), `0x168DFF0`.
  2. **Event loop**: `call r10` at `0x14C2626`, where `r10` is slot `+0x08` of
     the object at global `0x142349580` (loaded at `0x14C2604`). That object
     is created by `0x155FC10` with vtable `0x1D95068`, whose slot `+0x08`
     (`0x1D95070`) is `0x1569C70`. Its return value is the quit flag, stored
     at `app+0x2F8`; on quit the frame sets `app+0x83` and returns.
  3. **Mouse device update**: on the object at `[0x1423494E8]+0x08`, slots
     `+0x78`, `+0x60`, `+0x68` (`0x14C2640`, `0x14C264A`, `0x14C2678`).
  4. Background gate (`0x14C2692`): if `app+0x2F9` is set and `app+0x32B` is
     not, `SDL_Delay(50)` and return without updating.
  5. Only when the second argument is non-zero: `SDL_Delay(100)` if the byte
     `+0x530` of the object at `0x1423494B8` is zero and `app+0x32B` is zero,
     otherwise `SDL_Delay(0)` (`0x14C26C6`–`0x14C26E7`; thunk `0x1733FA0` is
     dynapi slot 447, `SDL_Delay` [S]).
  6. Frame time from the timer at `0x1423494A8`, then the **current state's
     update**: slot `+0x20` of the object at `app+0x40`, called with the same
     second argument (`0x14C2815`).
  7. **State switch** when `app+0x48` is set: the object at `app+0x70`
     becomes `app+0x40`; the previous one is moved or released through its
     deleting destructor (`0x14C2859`–`0x14C28F6`). `0x14C2530` is the setter
     that queues a new state (`app+0x48 = 1`, object into `app+0x70`).
  8. A few subsystem updates, then `inc qword ptr [rdi+320h]`
     (`0x14C2A23`): `app+0x320` counts every completed frame, outer and
     nested, and is the last write before the epilogue.
- No mutex, critical section or spinlock is taken in `0x14C25A0` or
  `0x14C2D00` themselves. [G] decompiles of both (callees not exhaustively
  checked).

### Nested frames (`0x769EA0`) and reentrancy

- `0x769EA0(root, force, …)` calls `0x14C25A0(session+0x330 → app, 0)` when
  the session pointer `root+0x1E00` is set, the global byte `0x1FC23D8` is
  set, `root+0x1E10` is set or `force` is non-zero, and the time since the
  last nested render exceeds `setting(+0x284 of [0x14242BDE8]) × table[speed]`.
  The table at `0x1C33E18` holds `0.02` for internal speeds 0–3 and `0.05`
  for speed 4 (UI speed 5). It logs "Delta: … (rendered)". [G], [D]
  `0x769F02`–`0x769FB2`, [F] table values.
- `0x1FC23D8` is 1 in the file and is toggled by the console handler
  `0xE84220` ("Asserts(and smoothing) are now turned on/off.") and cleared by
  `0x8740B0`. So nested frames are on by default. [F], [G].
- Callers of `0x769EA0` include `0x75BBE0`, `0x75D690`, `0x758430`,
  `0x75B0D0`, `0x75B2D0`, `0x75B4B0`. Two chains reach them:
  - game update: `0xB79520` (a vtable slot of a timer command,
    `timercommands.cpp`; the lab profile reaches it from `0x20F810`) →
    `0x758430` → `0x75BBE0` / `0x75D690` / … → `0x769EA0`; [G] call tree.
  - save loading: `0x81AB20` → `0x773B20` → `0x75BBE0` / `0x75D690` →
    `0x769EA0`; `0x81AB20` is called from the in-game session constructor
    `0x803270` (stores vtable `0x1CB8460`, loads `interface/main.gui`,
    `topbar.gui`, …) at `0x80B185`, and from `0x5D0580`; `0x803270` is called
    from `0x10DA520` (`frontend.cpp`, "LOAD_INIT"), which is reached from the
    frontend update `0x10D2E80`. [G] call tree, [D] string references.
- The in-game session's update is `0x8137C0` (vtable `0x1CB8460` slot
  `+0x20`, data reference `0x1CB8480`); the frontend's is `0x10D2E80`
  (vtable `0x1D5A8C8`, slot `+0x20` at `0x1D5A8E8`; the vtable is stored by
  `0x10D24E3`/`0x10D2C1F`, the function references `mainmenu`,
  `nudge_button`, `CALCULATING_CHECKSUM`). Both call the game update
  `0x20F810` **only when their second argument is non-zero**
  (`0x8137E8: movzx r15d,dl` … `0x813F95: 45 84 FF 74 27`, `test r15b,r15b;
  je`; frontend `0x10D2EA8: movzx r12d,dl` … `0x10D3AF5: test r12b,r12b`). A nested
  frame (second argument 0) therefore renders and handles input but cannot
  start the game update again, so it cannot nest further through this path.
  [D], [G].
- Result: `0x14C25A0` **is reentrant**. A nested call runs while an outer
  call is still on the stack, in the middle of a game update (days passing) or of loading a
  save. The second argument tells them apart: `1` for both outer callers,
  `0` for the only nested caller.

### Which thread

- `0x14C2D00` is called once, from the application start-up `0x8757E0`
  (`0x8781B4`), on the thread that runs it and that `0x14C2D00` pins with
  `SetThreadAffinityMask(GetCurrentThread(), …)`; `0x2003D0` is called from
  the same function (`0x878024`). [G], [D].
- [L] The lab's sampled main-thread stacks during a simulated year show
  `0x14C2D00 → 0x14C25A0 → 0x8137C0 → 0x20F810 → … → 0xB79520 → 0x758430`
  in 98% of samples, so the game update, and with it `0x769EA0`, runs on the
  main thread inside an outer frame.

### Main menu, start-up, loading, pause, minimized

- **Normal start (a language is set):** `0x8757E0` tests the qword at
  `+0x58` of the settings object `[0x14242BDE8]` (`0x878012`). When it is
  non-zero, it creates `LoadingThread` with `SDL_CreateThread` (thunk
  `0x17320A0`, slot 11 [S]) on `0x874010` and then waits in a loop at
  `0x878050`–`0x878412`: lock the mutex at `0x142358078`, check
  `0x14242BBCB`, poll SDL events directly (`SDL_PollEvent`, thunk
  `0x1735400`, slot 85 [S]; on `SDL_QUIT` it terminates its own process with
  `OpenProcess`/`TerminateProcess`), call `0x169A2F0(0)`, `Sleep(10)`. When
  loading ends it joins the thread (`SDL_WaitThread`, slot 439), logs
  "Running application..." and enters `0x14C2D00`. **No frame runs during
  this phase**: neither `0x14C25A0` nor `0x1569C70` is called; events that
  arrive are consumed by that loop. [D], [S], [F] IAT names `Sleep`,
  `OpenProcess`, `GetCurrentProcessId`, `TerminateProcess`.
- **First start (no language):** `0x2003D0` runs the init on the main thread;
  when `[0x14242BDE8]+0x58` is zero it queues a state built by `0x1179130`
  (strings `languageidler.cpp`, `LanguageSelection`, `language_list`) and
  runs outer frames `0x14C25A0(app, 1)` while `app+0x5E4` is set, then
  reloads localisation ("Reloading Localisation..."). [G], [D].
- Earlier, during window set-up, `0x14C3DD0` calls slot `+0x10` of the
  event object, `0x156AA60`, which drains and discards all pending SDL
  events (`0x156AA64`–`0x156AA70`). [D].
- **Main menu:** the frontend is a state driven by the same frame function
  (its update `0x10D2E80` is a state slot `+0x20`), so outer frames run.
- **Loading a save from the menu:** the load runs synchronously on the main
  thread inside the frontend's update, i.e. inside one outer frame
  (chain above). Until it returns, only nested frames (second argument 0)
  run, and only while the nested-render conditions hold.
- **Paused:** `0x14C2D00` and `0x14C25A0` read no pause state; the loop
  conditions are only `app+0x83` and `app+0x2F8`. Outer frames continue while
  paused.
- **Minimized / unfocused:** the event loop handles `SDL_WINDOWEVENT`
  (`0x200`) sub-events by setting the focus byte `0x1FD10C5`: 1 for shown
  (1), moved (4), resized (5), maximized (8; also `SDL_RestoreWindow`),
  restored (9), focus gained (12) and mouse button down; 0 for minimized
  (7) and focus lost (13). Enter/leave (10/11) call a slot of the mouse
  device. Nothing there stops the loop. [G] decompile of `0x1569C70`,
  event ids [S]. The background gate `app+0x2F9` is written only by the
  event loop, on `SDL_APP_WILLENTERBACKGROUND` (`0x103`, set) and
  `SDL_APP_WILLENTERFOREGROUND` (`0x105`, clear) (`0x1569D75`, `0x1569DA0`;
  the only writes in the disassembly), and SDL 2.0.4's Windows event code
  posts no `SDL_APP_*` event. [D], [S]. So on Windows the 50 ms background
  path is not taken through SDL.
- The `+0x530` byte behind the 100 ms delay is set by `0x14DF050` when a
  device-status call through the graphics function table returns 0, and
  cleared after `Sleep(1000)` when it stops returning 0 (`0x14DF0A4`–
  `0x14DF0CA`); `0x14DDC40` also refreshes it. [D]. What the call is, and
  whether minimizing changes it, is not known statically (see Hypotheses).

### Event loop `0x1569C70`

- Only caller: the frame, through the one slot `+0x08` load at `0x14C2619`.
  The other code reads of `0x142349580` call slots `+0x10`,
  `+0x20`, `+0x28`, `+0x30`, `+0x38` or write `+0x08` (a data field), never
  slot `+0x08`. Its only other references are the vtable slot `0x1D95070`
  and `.pdata`. [G], [D].
- So it runs once per frame, in outer **and** nested frames, before the
  state update; its arguments are the event object, the current state
  (`app+0x40`) and three input devices from `[0x1423494E8]` (`+0x08` mouse,
  `+0x10`, `+0x18`), with nothing that marks a nested frame. [D]
  `0x14C2604`–`0x14C2626`.
- On `SDL_QUIT` it may call `SDL_Quit` and returns 1 before the rest of the
  queue is handled. [G].
- It handles a custom event type `0x1000` (below `SDL_USEREVENT` `0x8000`
  [S]): it calls the function pointer at `event_object+0x30` with
  `event_object+0x28` and the pointer in the event at `+0x08`, then
  `SDL_free`s that pointer. That is one fixed callback slot of the game's own,
  not a general queue. [G].
- Prologue (33 bytes, [D]): `48 89 5C 24 08 4C 89 4C 24 20 55 56 57 41 54
  41 55 41 56 41 57 48 8D 6C 24 D0 48 81 EC 30 01 00 00`.

### Cursor query `0x14E5E50`

- Two callers ([G] references, [D]):
  - `0x162DDB0` at `0x162DDE2`: slot `+0x60` of vtable `0x1DA3530`, the
    mouse device that the factory `0x15CCCF0` creates (size `0x238`,
    vtables stored at `0x15CCD20`–`0x15CCD55`) and that `0x8757E0` stores at
    `[0x1423494E8]+0x08` (`0x875F71`, global written at `0x875EB0`). It
    reads the position, stores the delta and notifies listeners. The frame
    calls exactly this slot at `0x14C264A`, so this path runs **once per
    frame, outer and nested**.
  - `0x162E2D0` at `0x162E30A`: slot `+0xB8` of the same vtable, a getter
    that returns `(-1000, -1000)` when the byte `0x233FF4F` is set and
    otherwise calls `0x14E5E50`. Its callers are virtual; how often they ask
    is not known.
- So `0x14E5E50` runs at least once per frame plus any number of getter
  calls, from inside the frame between event handling and the state update.
- Prologue (12 bytes, already used by Tongs): `48 89 5C 24 10 55 56 57 48
  83 EC 40`.

### Frame-function prologue and the bytes the gate relies on ([D])

```
0x14C25A0  48 89 5C 24 08        mov [rsp+8],rbx
0x14C25A5  48 89 6C 24 10        mov [rsp+10h],rbp
0x14C25AA  48 89 74 24 18        mov [rsp+18h],rsi
0x14C25AF  48 89 7C 24 20        mov [rsp+20h],rdi
0x14C25B4  41 56                 push r14
0x14C25B6  48 83 EC 70           sub rsp,70h
0x14C25BA  0F 29 74 24 60        movaps [rsp+60h],xmm6
0x14C25BF  44 0F B6 F2           movzx r14d,dl
0x14C25C3  48 8B F9              mov rdi,rcx
0x14C25C6  FF 15 44 51 6A 00     call [SteamAPI_RunCallbacks]
```

The first 26 bytes (to `sub rsp,70h`) contain no RIP-relative operand and no
branch; the first instruction boundary at or past 14 bytes is at 15.

## Hypotheses (need a live check)

- H1. Outer frames run continuously in the main menu, in game (paused and
  running) and with the window unfocused, at the display's or the
  renderer's pace; nothing in the frame limits the rate besides
  `SDL_Delay(0)`. The rate itself is not visible statically.
- H2. At speed 5 one outer frame contains a whole game-update call
  (`0x20F810`, possibly several days), so outer frames are rarer than at
  low speeds; the gaps are filled with nested frames every ~50 ms × the
  `+0x284` setting.
- H3. `[0x1423494B8]` is the graphics device and `+0x530` means "device
  usable"; with exclusive full screen, minimizing loses the device and the
  loop slows to about one outer frame per 100 ms (plus a 1 s sleep on the
  transition). In windowed or borderless mode minimizing does not change
  the rate.
- H4. `app` is also reachable through the global `0x14242BF50`: the event
  loop writes `[0x14242BF50]+0x2F9` and reads `[0x14242BF50]+0x348 → +0x188`
  with the same offsets the frame uses on its `rcx`. Plausible, not proven.
- H5. `app+0x40` holds the frontend object (vtable `0x1D5A8C8`) in the
  menu and the session (vtable `0x1CB8460`, same object as `root+0x1E00`)
  in game. Known statically only that both classes have their update at
  slot `+0x20` and that the session has a back-pointer to `app` at `+0x330`.
- H6. Worker-pool jobs started by the game update are joined before
  `0x20F810` returns, so at the end of an outer frame no game-update work
  runs on other threads. Other threads (Steam, PDX online services, audio,
  file tasks) can still run; a game call is only as safe as the game's own
  main-thread code at that point.
- H7. Hooking `0x14C25A0` from Anvil's `main` phase is not racy in
  practice: the first call is either the language screen (first start only)
  or the main loop after `LoadingThread` finished, both later than plugin
  load. Not checked how SafetyHook treats a thread inside the prologue.

## Proposed hook and `[[expect]]` bytes

Hook `0x14C25A0` with `hook_function` (expected prologue = the 26 bytes
above). Detour:

```cpp
void Detour(App* app, bool outer) {
    g_original(app, outer);              // the game's whole frame
    if (!outer) return;                  // nested frame: inside a game update or a load
    if (GetCurrentThreadId() != g_main_thread) return;  // set on the first outer frame
    if (byte(app + 0x83) || byte(app + 0x2F8)) { FailQueue("game is quitting"); return; }
    RunQueued(app, budget);              // bounded: N jobs or a few ms per frame
}
```

Frame end rather than frame start, because at that point:

- this frame's events were handled and its state update and rendering are
  done, so a read sees what the player sees;
- a pending state switch (`app+0x48`) was applied and the old state may have
  been destroyed; queued work must look up widgets from the current state
  each time and never keep pointers across frames;
- `app+0x320` was already incremented, so "frame N done" is well defined for
  `wait_for` (the counter alone can also be polled from the pipe without a
  hook, but it counts nested frames too);
- a quit request from this frame is already visible (`app+0x83`).

The difference to frame start is small: between the end of one outer frame
and the start of the next, the loop only checks the quit flags and applies a
pending cursor. Input that the work pushes with `SDL_PushEvent` is handled
by the next frame's event loop either way.

Neither `0x1569C70` nor `0x14E5E50` is a good dispatcher: both run in nested
frames without an argument that says so (a depth counter would need a frame
hook anyway), both run mid-frame before the state update, and `0x14E5E50`
also runs an unknown number of times per frame through the getter.

Proposed manifest entries (all bytes from [D]):

```toml
[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x14C25A0
bytes = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 48 89 7C 24 20 41 56 48 83 EC 70 0F 29 74 24 60 44 0F B6 F2 48 8B F9"
name = "frame function: prologue, second argument (outer frame) kept in r14b"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x14C2E0E
bytes = "B2 01 48 8B CE E8 88 F7 FF FF 44 38 B6 83 00 00 00"
name = "application loop: frame(app, 1), then quit flag at app+0x83"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x769FA6
bytes = "48 8B BF 30 03 00 00 33 D2 48 8B CF E8 E9 85 D5 00"
name = "nested frame from game updates: frame(session->app, 0)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x2006D0
bytes = "B2 01 48 8B CF E8 C6 1E 2C 01"
name = "language-selection loop: frame(app, 1)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x14C2A23
bytes = "48 FF 87 20 03 00 00"
name = "frame function: frame counter at app+0x320"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x813F95
bytes = "45 84 FF 74 27"
name = "session update: game update only in outer frames"
```

The first entry overlaps the hooked range; whether Anvil accepts a manifest
`[[expect]]` and a `hook_function` on the same bytes of one plugin needs a
check in Anvil (the call itself already verifies its prologue). The three
call-site entries are what makes "outer = second argument non-zero" a checked
dependency: a build that adds a caller would still pass them, so a release
should also log a counter of outer and nested calls.

## Open questions

- The frame rate and what paces it (present with vsync, driver, or nothing).
- The value of the `+0x284` setting that scales the nested-render interval,
  and whether the lab's mod set changes it.
- What `app+0x32B` is (written at `0xB7AE06`, `0xB7AE19`, `0xE4C4B4`; copied
  at `0x15087D7`); it keeps frames running through the background gate and
  removes the 100 ms delay.
- What `0x169A2F0` and `0x168DFF0` do at frame start, and what the
  byte `0x1FD10C4` means (many functions set it around their work; the
  session update clears it around `0x20F810`).
- Whether any game code waits for the main thread from another thread (a
  long dispatched job would then stall that thread, not only the frame).
- Whether console commands, the first kind of game call, are normally run
  from within the event loop (GUI input) or from the state update; running
  them at frame end is close to, but not the same as, the game's own place.

## Live checks a later `task` ticket should run

Through the lab's launcher rules, with a small Tongs build that hooks
`0x14C25A0` and only counts (no queued work):

1. Count outer and nested calls per second and read `app+0x320` in: main
   menu, loading a save from the menu, in game paused, speed 1, speed 5,
   window unfocused, window minimized (windowed/borderless and, if allowed,
   exclusive full screen). Confirms H1–H3 and gives the latency a game call
   can expect.
2. Record the thread id of every call and compare it with the window thread
   and the main thread Tongs already reports in `threads`.
3. Record the first outer call relative to "Running application..." and the
   end of `LoadingThread`; confirm no call happens during start-up loading.
4. Compare `rcx` of the hook with `[0x14242BF50]` (H4) and log the vtable of
   `app+0x40` in menu and game (H5).
5. During a save load from the menu, confirm that outer calls stop until the
   game is loaded and nested calls continue.
6. Time the longest gap between outer frames at speed 5 over a simulated
   month; that is the worst-case wait of a queued job.
7. With a no-op job queued every outer frame, check that the date advance
   per second at speed 5 is unchanged within the lab's measurement noise.
