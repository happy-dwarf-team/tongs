# Research: how the game loads a save, and loading one by file name

Ticket: happy-dwarf-team/tongs#97 (map #94). Static analysis only, EU4 1.37.5
Steam Win64, `eu4.exe` SHA-256
`9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A` (checked on
the analysed copy before and after). No game process was started; no save,
mod or `eu4.exe` was changed. RVAs are relative to the image base
`0x140000000`.

How each claim was checked:

- **[G]** Ghidra 12.1.4 decompiler and reference manager (pyghidra, a private
  copy of the lab's analysed project `eu4-1.37.5`).
- **[D]** Ghidra's instruction listing (address, bytes, operands).
- **[S]** a string and the code that references it ([G] references).
- **[F]** bytes read from the file (`.data` initial values).
- **[L]** an earlier live observation in the lab repository or in Tongs'
  `docs/design.md`, named where used.
- **[UI]** the game's own `interface/*.gui` files (read only).

## Question

How does EU4 1.37.5 load a save, from the main menu and from a running game,
and can that path be driven by a save's file name without clicking through
the Load dialog? What does the Load button call and check, which confirmation
dialogs are on the way, what about the launcher's continue path, what is the
state between the call and the map, which signal says the load is finished,
and which steps of the lab's path in `docs/ui-workflow.md` break?

## Short answer

There are **two load paths**, and both can be driven by a file name.

- **In a running game** the Load dialog ends in one function,
  **`0x5D0580(LoadRequest*)`**. The request holds the file name without
  `.eu4`, an optional list of subfolders, the session `*(root+0x1E00)`, a
  cloud flag, and one helper object. The function resolves
  `save games/<subfolders>/<name>.eu4`, checks that the file exists, checks
  the save's metadata (player, ironman in a modded game), then reads the whole
  gamestate synchronously on the main thread. It returns 0 (loaded), 1 (empty
  name), 2 (no such file) or 3 (metadata rejected). The console command
  `check_save` (`0xE95610`) already builds the same request from a file name
  and calls `0x5D0580` without any dialog. The checks in the dialog
  (`Show incompatible`, the version warning, the confirmation with the file
  name) all run **before** the call, in the UI code, and `0x5D0580` itself does
  not repeat them.
- **From the main menu** the frontend does not use `0x5D0580`. A save is read
  into the **lobby** when it is selected (`0x1130F40` → `0x1132CA0` →
  `0x110D760` → `0xD84B80`), and `Play` (`0x1120740`) starts the game. The
  launcher's `continue=<file>` option, which the lab's `anvil-play.ps1` uses,
  drives exactly this path by name: it sets a flag and a path at start-up,
  the frontend picks up the flag on its first frame and runs the menu's
  `Continue` action (`0x10EF410`), which selects the file in the lobby, loads
  it and presses `Play`. The lab has verified this live.

A Tongs command that loads by name changes game state, so it is a **game
call** (ADR 0001) and marks the run; it is not a client-side action. In game,
the cheapest form is calling `0x5D0580` on the main thread the way
`check_save` does. In the menu, the form that needs the least game code is the
game's own continue flow fed with a new name (a hypothesis, see H3). Both are
"finished" when the main-thread job returns and `state` answers
`in_game: true` with the save's date; the load runs inside a single outer
frame.

## Verified statically

### In game: the Load dialog (`menu_load`)

- The in-game load dialog is `menu_load` in `interface/ingameloadmenu.gui`
  with the list `games`, `show_unsafe_checkbox`, `show_unsafe_text`, the
  buttons `load` and `cancel`, and the tabs `localgames_tab` /
  `cloudgames_tab`. [UI] The menu's `menu_load_button` opens it
  (`0x8318F0`, string refs at `0x8320CA` and `0x8322A0`). [S]
- `0x8318F0` builds the dialog with `0x1172790` and binds its callbacks as
  functors whose code pointer sits two slots after the functor
  ([G], `puVar6[...]` stores):
  - view `+0x288` → `0x1175EA0` (`show_unsafe_checkbox`, bound in
    `0x1175C90`),
  - view `+0x2C8` → `0x11756B0` (the `load` button),
  - view `+0x340` → `0x1176020` (load without the confirmation, see below).
- **`Show incompatible` only filters the list.** `0x1175EA0` reads the check
  box's state (`+0x100` == 1, the offset Tongs already uses) into view
  `+0x3B8` and rebuilds the list with `0x1172E30(view, 0)`. Nothing on the
  load path reads `+0x3B8`. [G]
- A row's "unsafe" mark is computed per entry in `0x11BFBC0`: missing DLC
  (`SAVEITEM_MISSING_DLC`), a missing mod (`SAVEITEM_MISSING_MOD`) or another
  version (`SAVEITEM_UNSAFE_VERSION`, via `0xB29E10`) set entry `+0x318` and
  add a tooltip line. [S][G] It is a display flag, not a load check.
- **The `load` button, `0x11756B0`**, walks the rows of `games`
  (child `"games"`, list slot `0x58`) and takes the first selected row
  (row slot `0x30`) that is not disabled (row byte `+0xA0`). Then [G][D]:
  - an ironman row (`row+0x1A8`) with cloud saves unavailable
    (`*(0x242BF50)+0x328 == 0`) returns without any dialog;
  - if `0xB29CA0(row meta)` is true (the running version is listed in the
    save's `savegame_versions`, compared with the four integers at
    `*(0x242BF50)+0x400…0x40C`), it opens the **confirmation**
    `0x11BE0B0(dialog, view, &view+0x3C0)` (call at `0x11757F2`);
  - otherwise it opens a `DefaultConfirm` with
    `SAVEGAME_WARNING_OLD_VERSION_TITLE` / `_DESC`, whose `AgreeButton` is
    bound to view `+0x340`, that is `0x1176020` directly.
- **The confirmation `0x11BE0B0`** is a `DefaultConfirm` (vtable
  `0x1D65AA0`) titled `CONFIRMLOADSAVETITLE`, with `CONFIRMLOADSAVETEXT`
  formatted with one parameter: the string at view `+0x3C0`, the selected
  file name. [S][G] This is the lab's "OK with the original file name". Its
  accept `0x11BE2D0` calls `0x1176020(dialog+0x1B8 = view)` (call at
  `0x11BE2E0`). [D]
- So exactly one confirmation stands between `load` and the load: either
  `CONFIRMLOADSAVE` (save from this version) or the old-version warning
  (other versions). Both end in `0x1176020`. No string about unsaved
  progress exists in the binary (`(?i)unsaved` matched nothing), so there is
  no "unsaved game" warning on this path. [S]
- **`0x1176020(view)`** builds a request on the stack and calls `0x5D0580`
  (call at `0x11760C9`), then `0x824810(view+0x38)`, which closes the open
  in-game windows. [G][D] The request, as `0x5D0580` reads it:

  | Offset | Content | From the view | `check_save` (`0xE95610`) |
  | --- | --- | --- | --- |
  | `+0x00` | `std::string` file name | `+0x3C0` | `"savegame_check_1.eu4"` |
  | `+0x20` | `vector<std::string>` subfolders | `+0x250…+0x258` | empty |
  | `+0x38` | session (`root+0x1E00`) | `+0x38` | `*(0x233FE78)+0x1E00` |
  | `+0x40` | cloud flag (byte) | `+0x28 == 1` (cloud tab) | 0 |
  | `+0x48` | helper object from `0x8BF160()` (lazily created global `0x242BCF8`) | `0x8BF160()` | `0x8BF160()` |

  `0x5D0580` itself reads `+0x38` into `r14` at `0x5D059F`. [D]

### In game: what `0x5D0580` does

`0x5D0580` (`remotefile.cpp` strings, so the lab's note "full load with
cloud support" stands) [G][S]:

1. If the request has a session, it tears down the running game's UI state
   first (`0x821FF0`, `0xDFFCF0`, per-window callbacks, the map's draw object
   at `+0x12D0`).
2. It copies and trims the name (`0x17043C0`); an empty name returns **1**.
3. Local file: `0x5CA070(name, subfolders)` builds
   `<save folder>/<subfolders>/<name>` and appends the global
   `std::string` at `0x1FD2F68`, which is `".eu4"` in the file (bytes
   `2E 65 75 34 00 …`, size 4 at `0x1FD2F78`). [F] Then `PHYSFS_exists`, and
   the file is opened through the VFS (`0x1710440`). Cloud file (byte
   `+0x40`): the Steam cloud context `*(*(0x242BF50)+0x348)+0x38`, or an
   exception `"Could not obtain cloud storage context."` /
   `"Could not obtain cloud file."`.
   No file returns **2**.
4. It marks the session as loading (`session+0x460 = 1`, `0x826C20`).
5. `0x5CCFE0` opens the save's parts, and `0x5D01B0` reads the metadata:
   it resets the date to `0x29C77F8` (hour 43 808 760 = 1 January of year 1,
   hour 0) before reading the save's date into the root (`0x76D260`), reads
   the player tag, and rejects an ironman save in a modded game
   (`"Loading ironman in modded game"`) or a save without a player
   (`"Player missing in savegame meta"`). Rejected returns **3**
   (call at `0x5D0BB2`, `jnz` on success).
6. `0x74B8A0(root, reader)` reads the gamestate (call at `0x5D0C78`). Its
   result is not checked by `0x5D0580`: a corrupt save shows
   `SAVEGAME_ERROR_LOADING` / `_CORRUPT` from inside `0x74B8A0` and
   `0x5D0580` still goes on.
7. Afterwards it starts the save-session thread (`0x776B90`,
   `SaveSessionUpdateThread`), runs `0x75CB40(root, 1)`, rebuilds the
   player's view (`0x277310`, `0x81AF10`, `0x81AB20`), clears the loading
   flag, and returns **0**.

The whole call is synchronous on the calling thread; nothing is queued for a
later frame. Together with the dispatcher research (`0x769EA0` draws nested
frames from inside long game updates and loads, outer frames do not run while
a save loads; Tongs `docs/design.md`, "Dispatcher") this means a load started
from a main-thread job ends before that job returns.

### Main menu: the lobby

- The lobby object keeps the selected save in strings at `+0x318` (file
  name), `+0x338` (full path, used when not empty) and the save folder's
  subfolders at `+0x260`; `+0x358` is the ironman/cloud flag of the
  selection. [G] (`0x1132CA0`, `0x10EF410`)
- **Selecting a save loads it.** `0x1130F40(lobby)` (bound as a functor in
  the lobby's construction `0x1108700`) shows `PLX_WAIT` /
  `FE_IRONMAN_CHECKSUM` for a cloud ironman selection, then calls
  `0x1132CA0(lobby, 0)`. That builds the path (`+0x338`, or the save folder
  + `"/"` + `+0x318`), checks `PHYSFS_exists`, reads the **whole file** into
  memory through the VFS open `0x1718300`, reads its metadata
  (`0x5CCCA0`, `0xB28EE0`), handles `imported_ironman/`, and calls
  `0x110D760(lobby+0x58, name, buffer, 0)` (call at `0x113373A`). [G][D]
- `0x110D760` calls `0xD84B80(*(app+0x330), gamestate, name, buffer,
  compressed)` (call at `0x110D7D5`), which logs `LOADING_SAVEGAME` and
  deserialises the buffer. On failure the lobby shows `LOAD_GAME_FAILED`; on
  success `SAVE_LOADED`, the political map mode and the lobby's nation view.
  [S][G] This explains the lab's observation that clicking an incompatible
  save in the lobby kept the main thread busy for minutes [L]
  (`docs/reverse-engineering.md`, "Menu, wczytywanie…"): the click is the
  load.
- **`Play`** is `0x1120740(lobby, 1)` (`play_button`); in single player it
  goes on through `0x111F930`, which can show
  `SAVEGAME_WARNING_ACHIEVEMENTS_DISABLED_*` before the game starts. [S][G]
  The in-game session that follows is the one Tongs sees at `root+0x1E00`.

### Main menu: `Continue` and the launcher's `continue=`

- `0x8757E0` (application init) reads two options [G]:
  - `continuelastsave`: `0x5D1670` reads `continue_game.json` from the user
    folder (keys `title`, `desc`, `date`, `filename`) and, when it parsed,
    sets the flag `0x233FE9C` (write at `0x876695`) and copies `filename`
    into the global `std::string` `0x1FD3A08`. The local file today has
    `"filename": "save games/autosave.eu4"` [F]. The game writes it through
    `continue_game.temp` (`0x5D1050`).
  - `continue=<file>` (only when the flag is still clear): sets the flag and
    `0x1FD3A08 = "save games/" + <file>` (`lea` of the string at `0x8768AD`),
    without checking the file. (Already in the lab's
    `docs/static-analysis.md`.)
- The loading orchestrator `0x2043A0` reads `0x1FD3A08` and calls
  `0x2111C0(app_loader, path)` synchronously (call at `0x204EA4`); without a
  path a pool task `0x212810` finds the newest save (`0x211960`) and calls the
  same function. `0x2111C0` fills **`app_loader+0x5D8`** with a `0x220`-byte
  metadata object of that save (the object behind the menu's
  `continue_container` / `continue_game_button`). [G] (lab notes, confirmed)
- `0x10EDB80` (called from the frontend's update functions `0x10EE500` and
  `0x10EFF90`) checks the flag (`cmp byte [0x233FE9C],0` at `0x10EDB86`),
  clears it and tail-jumps to `0x10EDA80`. [D]
- `0x10EDA80` validates the continue metadata with `0x10EFA90` →
  `0x210A80` (copy of `app_loader+0x5D8`) → `0x10DA420`, which requires a
  parsed metadata object (`+0x81`, `+0x189`, `+0x50`), a known version
  (`0xB29E10`), the DLC check `0x150DF20`, and for ironman more conditions.
  - Invalid: `0x10EDC10` shows `DefaultInfoDialog` with
    `CONTINUE_FAILED_TITLE` / `_DESC` and the menu stays. So a `continue=` name
    that does not exist ends in this dialog.
  - Valid: `0x10EE950` may first show a `DefaultConfirm` (`AgreeButton`,
    `CANCEL`): `SAVEGAME_WARNING_ACHIEVEMENTS_DISABLED_*` when `0x96900`
    fails, else `SAVEGAME_WARNING_OLD_VERSION_*` when `0xB29CA0` fails; with
    no warning it calls the functor at frontend `+0x9C8`, which the frontend
    constructor `0x10EB670` binds to **`0x10EF410`**. [G]
- **`0x10EF410`** is the `Continue` action. It re-validates (`0x10DA420`),
  switches the frontend to state 3 (the lobby, `0x10DAF60(frontend, 3)`),
  copies the save path into lobby `+0x338` and the ironman byte into
  `+0x358`, calls `0x1130F40(lobby)` (the load above, call at `0x10EF7EE`),
  sets the player (`0x763840`) and calls `0x1120740(lobby, 1)`, i.e. `Play`
  (call at `0x10EFA06`). [G][D]
- Consequence: `continue=` and the `Continue` button are the lobby path by
  name, not a separate loader. The lab has verified that
  `continue=mp_Russia1719_03_17.eu4` reaches the map without UI and leaves the
  save unchanged [L] (`docs/static-analysis.md`, run
  `continue-russia-20261014-000839`), and that a game restarted by itself
  after "Exit to Menu" → "Quit" keeps the original `continue=` [L]
  (`docs/reverse-engineering.md`).

### Between the call and the map

- Session pointer null, then the same address with another class, then a
  date of 1 January of year 1 hour 0, then the save's date [L] (Tongs
  `docs/design.md`, State reader). The 1-January value is the default
  `0x29C77F8` that `0x5D01B0` writes before reading the save's date. [G]
- While `0x5D0580` or the lobby load runs, outer frames do not run; nested
  frames do. [L] (`docs/design.md`, Dispatcher, live check: 79 s of
  `main thread busy` across start-up with the save, 3.5 s for a load from
  the menu).
- After the load the game paused itself about a second after the first
  `in_game: true` [L] (`docs/design.md`, Live check 2026-10-16), and the
  speed was reset [L] (`docs/ui-workflow.md`). The worker pools are created
  in the application init `0x8757E0` [L] (lab `docs/static-analysis.md`),
  and none of the load functions read here calls it, so the worker counts
  are expected to stay the same across a reload; deeper callees were not
  followed, so this is a hypothesis for the in-game check.

## Hypotheses (not verified)

- **H1. A direct call works outside the dialog.** Calling `0x5D0580` with the
  `check_save`-shaped request from a Tongs main-thread job, after an outer
  frame and with the session in game, loads the save as the dialog does.
  `check_save` does this from the console, so the session state it needs is
  that of a console command; whether menus left open by the user matter (the
  dialog closes them afterwards with `0x824810`) is not known.
- **H2. `check_save` passes a name with `.eu4`** (`"%s.eu4"`), and `0x5CA070`
  appends `.eu4` again, so either `check_save`'s reload looks for
  `savegame_check_1.eu4.eu4` and fails, or a step that strips the extension
  was missed. The dialog's view `+0x3C0` is expected to hold the name without
  `.eu4` (`0x1172E30` cuts each path at the last `/` and before the
  extension), but which code fills `+0x3C0` on a row click was not found.
- **H3. Continue by name from the main menu without restarting.** Writing a
  new path with `0x2110C0(app_loader, "save games/<file>")` (it frees the
  pending task at `+0x5D0` and calls `0x2111C0`) and then setting
  `0x233FE9C` would make the next frontend update run the whole continue
  flow for that file. `0x2110C0` is bound as a functor in `0x10EF120`, which
  suggests the game itself refreshes the continue entry this way.
- **H4. The lab's Russia save is "unsafe" for DLC or mod reasons, not for its
  version**: `continue=` reaches the map with no warning dialog, so
  `0xB29CA0` passes; then the OK the lab clicks in game is
  `CONFIRMLOADSAVE`, not the old-version warning.
- **H5.** The meaning of the lobby's `+0x358` (ironman or cloud) and of the
  frontend constructor's extra check (flag set and `0x1FD3A08` empty →
  validate, else `CONTINUE_FAILED`, at `0x10EC425`) were not traced further.

## The lab's path in `docs/ui-workflow.md`

| Step | What the game does | What breaks |
| --- | --- | --- |
| Menu button `(3133,1415)` | opens the in-game menu | coordinates; Tongs has `menu_load_button` by name |
| `Load Game` `(1718,550)` | opens `menu_load` | coordinates |
| `Show incompatible` `(1580,1078)` | rebuilds the list with unsafe rows (`0x1175EA0`) | needed only to see the row; a load by name skips it |
| Drag the scroll bar `(1894,380)`→`(1894,580)` | the list creates widgets only for visible rows | the row's position moves with every new save or autosave (86 saves on 16 Oct) and with the sort order; the wheel does not scroll |
| Russia row `(1708,916)` | selects the row; fills view `+0x3C0` | position-dependent; the label is a widget text that Tongs can match, but only when the row is scrolled into view |
| `Load` `(1873,1129)` | `0x11756B0`: version check, then one dialog | coordinates |
| `OK` `(1822,930)` | `CONFIRMLOADSAVE` → `0x11BE2D0` → `0x1176020` → `0x5D0580`; for another version it is the old-version warning's `AgreeButton` instead | which dialog appears depends on the save (H4) |
| wait for the map, PID, pools | `0x5D0580` runs inside one frame | the PID does not change on an in-game load; speed and pause must be read again |

The main-menu path (`Single Player`, saved-games tab, `Show incompatible`,
scroll, row, `Play`) breaks the same way, with one more trap: the row click
already loads the save into the lobby (`0x1132CA0`), so a wrong click costs a
full load.

## Candidate `[[expect]]` entries

```toml
[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x5D0580
bytes = "40 53 56 57 41 54 41 55 41 56 41 57 48 81 EC E0 04 00 00 48 8B F1 33 FF 89 BC 24 30 05 00 00 4C 8B 71 38"
name = "load a save (remotefile.cpp): request in rcx, its session at +0x38"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x117608E
bytes = "E8 CD 90 74 FF 48 89 44 24 68 48 8D 93 C0 03 00 00"
name = "load view accept: request+0x48 = 0x8BF160(), file name from view+0x3C0"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x11760C4
bytes = "48 8D 4C 24 20 E8 B2 A4 45 FF 48 8B 4B 38"
name = "load view accept: 0x5D0580(&request), then close windows with view+0x38"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0xE95BE6
bytes = "48 8D 4D C0 E8 91 A9 73 FF 85 C0"
name = "console check_save: 0x5D0580(&request) by file name, result tested"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x5D0BA5
bytes = "48 8D 94 24 60 01 00 00 48 8D 4C 24 70 E8 F9 F5 FF FF 84 C0 0F 85 9A 00 00 00"
name = "load a save: metadata check 0x5D01B0 (player, ironman in modded game)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x11BE2D9
bytes = "48 8B 89 B8 01 00 00 E8 3B 7D FB FF"
name = "CONFIRMLOADSAVE accept: 0x1176020(dialog+0x1B8 = load view)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x11757E5
bytes = "4D 8D 86 C0 03 00 00 49 8B D6 48 8B C8 E8 B9 88 04 00"
name = "load button: CONFIRMLOADSAVE dialog with the name at view+0x3C0"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x110D7CE
bytes = "48 8B 89 30 03 00 00 E8 A6 73 C7 FF 0F B6 D8"
name = "lobby: deserialise the selected save 0xD84B80(app+0x330, ...)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10EDB86
bytes = "80 3D 0F 23 25 01 00 48 8B D9 74 11 C6 05 03 23 25 01 00 48 83 C4 20 5B E9 DD FE FF FF"
name = "frontend update: consume the continue flag 0x233FE9C, run the continue flow 0x10EDA80"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x876695
bytes = "C6 05 00 98 AC 01 01"
name = "app init: continuelastsave found, set the continue flag 0x233FE9C"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x8768AD
bytes = "48 8D 15 AC 64 44 01"
name = "app init: continue=<file> prefixed with \"save games/\" (0x1CBCD60)"
```

As `symbols.toml` symbols, the values worth taking from them are the continue
flag (`0x10EDB86`, `at = 2`, `rip_relative = 7`), the session offset
`+0x38` of the request (`0x5D0580` + 34) and the view's name offset `0x3C0`
(`0x117608E` + 13). The `.eu4` extension is a `.data` string
(`0x1FD2F68`, `2E 65 75 34 00`, size 4 at `+0x10`) that a `memory = "read"`
symbol could check.

## In-game checks this leaves open

Each needs its own task ticket under the lab's game lock:

1. Call `0x5D0580` from a main-thread job with the `check_save`-shaped
   request and the Russia save's name without `.eu4` (H1, H2); compare
   `state`, pause and speed before and after, the return code, the save's hash
   and `settings.txt`.
2. With the same build, load a missing name (expect 2) and confirm that no
   dialog appears and the running game is untouched (the UI teardown in step 1
   of `0x5D0580` happens before the existence check, so this may not hold).
3. In the menu, try H3 (`0x2110C0` with a new path, then the flag) and note
   which dialogs appear.
4. Note which dialog the in-game `Load` shows for the Russia save (H4), as a
   widget tree read (`DefaultConfirm` with `CONFIRMLOADSAVETITLE` or with
   `SAVEGAME_WARNING_OLD_VERSION_TITLE`).
5. Measure how long the main thread is busy for an in-game load, to set the
   timeout of a load command.

## New questions

- A load command runs for seconds inside one main-thread job. The dispatcher
  contract lets a started job finish, but the client's answer limit
  (10 s by default) would need a load-specific timeout or an answer that
  returns at once and a `wait_until` on `in_game`.
- Should a loaded save mark the run as a game call even when the measurement
  starts after it (the lab's `continue=` start is not a game call today)?
- Cloud saves and ironman: the request has a cloud flag, but loading an
  ironman save from Tongs would let the game write to it; should the command
  refuse ironman saves?
- Subfolders of `save games` (the request's vector): does the command need
  them, or only top-level names?
