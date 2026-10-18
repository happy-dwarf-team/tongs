# Research: the console command path (EU4 1.37.5)

Ticket: happy-dwarf-team/tongs#17 (child of #15). Static analysis only; no game
process was started. Build: `eu4.exe` 1.37.5 Steam win64, SHA-256
`9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A` (checked on
the analysed copy before and after). All addresses are RVAs relative to the
image base `0x140000000`.

Methods, named per claim below:

- **decomp**: Ghidra 12.1.4 decompiler on a private copy of the lab's analysed
  project, read locally; not reproduced here.
- **disasm**: Ghidra listing of the instructions at that RVA.
- **bytes**: bytes read straight from the EXE file (section table mapping), also
  used for the `[[expect]]` entries and for the static command table.
- **xrefs**: Ghidra references to an address (code and data).

## Question

How does the console run a command line, and where does its output go? Which
function parses and dispatches a line, where is the registry of commands, where
does a command write its answer, can it be called from the main thread without
the console window, which commands are refused outside debug mode and how is
that checked, is `run <file>` available, and which commands change game state?

## Verified statically

### Command table and registry

1. **The game's commands are one static table in `.data`.** 378 records of
   `0xA0` bytes from `0x1FC23E0` (`enable_all_commands`) to `0x1FD0F80`
   (`federation_cohesion`). The fields are compile-time constants in the file
   (bytes): `+0x00` byte "available in release", `+0x08` name, `+0x10` alias
   count, `+0x18..+0x28` up to three aliases, `+0x30` help text, `+0x38`
   handler, `+0x40` argument count, `+0x48..+0x90` argument help strings,
   `+0x98` map-click target type. The `speed` record is `0x1FCCB60`; its handler
   pointer at `0x1FCCB98` is `0xE4FA00`, the handler the lab already knew.
   Nothing in the code points at a single record; the table is walked as a
   whole (next item).
2. **Registration** `0x6E90F0` (decomp, disasm `0x6E916F`): when the game root
   (`0x233FE78`) exists, it gets the console manager, sets its byte `+0x18` to
   1, sets byte `+0x19` from "multiplayer check `0x6EAA10` or root byte
   `+0x24D0`", then calls `0x1720B50` for each of the `0x17A` (378) records,
   stepping `0xA0`. It is called from the in-game idler (`0x81AF10`,
   `ingameidler.cpp`), the frontend (`0x10D5BC0`, `frontend.cpp`) and the
   language idler (`0x1179900`) (xrefs), so it runs again whenever these
   screens restore their device objects.
3. **Console manager singleton**: pointer at `0x2357B58`; getter `0x6E74C0`
   allocates `0x48` bytes on first use (disasm). Layout used by the code:
   `+0x00/+0x08/+0x10` a vector of `0xA8`-byte entries; `+0x18` byte
   "developer commands locked"; `+0x19` byte "multiplayer or ironman lock";
   `+0x20` std::string and `+0x40` int: the pending map-click command and its
   target type (used by `0x116D6C0`); `+0x44` byte set before every command
   except `stats`. The getter clears `+0x19` and `+0x44` but not `+0x18`, so
   `+0x18` is meaningful only after the first registration.
4. **Entry layout** (`0x1720B50` converts a record; decomp): `+0x00` release
   byte, `+0x08` name, `+0x10` alias count, `+0x18..+0x28` aliases, `+0x30`
   help, `+0x38` trampoline `0x172B120` (calls its third argument), `+0x40` the
   handler, `+0x48` argument count, `+0x50..+0x98` argument strings, `+0xA0`
   target type. `0x1720930` inserts it; an entry with the same name
   (`_stricmp`) is overwritten, so two duplicate records (`yesman`,
   `add_baseunrest`) leave 376 game commands.
5. **Engine commands** are registered into the same manager through
   `0x1720930` directly: `CrashReporter.DeleteCrashData`,
   `CrashReporter.SimulateCrash`, `CrashReporter.UploadCrash` (`0x164DD70`),
   `net_debuginfo` (`0x165D6F0`), `social_debuginfo`, `social_addfriend`,
   `social_joinroom`, `social_sendmessage` (`0x166BB70`) (decomp, strings).
6. **Second registry: console variables.** `0x17268B0`, called from 17 static
   initialisers, adds named variables to a vector at `0x2458BD0` (e.g.
   `Update.Particles` → byte `0x1FC23DA`, `Threading.EnableParallelFor` →
   `0x1FD10F2`, `Threading.UseTBBParallelFor` → `0x2357B34`; disasm of the
   `LEA RDX / LEA RCX / JMP 0x17268B0` stubs). `0x17265C0` looks a name up
   there.

### Dispatcher

7. **`0x1721320` executes one tokenised command line** (decomp, disasm). Win64
   arguments: `RCX` = console manager, `RDX` = result buffer, `R8` = pointer to
   a `std::vector<std::string>` of tokens; returns `RDX`. All four callers pass
   only these three (disasm at `0x166E1D2`, decomp of the others).
   - Lookup `0x1721000(manager, tokens[0], 1)`: linear search over the entries,
     `_stricmp` against the name and each alias, so names are case-insensitive.
   - Not found: if `+0x18` is 0, try the console-variable registry
     (`0x17265C0`) and call its function; otherwise answer `Unknown command` or
     `Command not found. Did you mean '<x>'?` from the suggestion helper
     `0x1721100`.
   - Found, entry byte `+0x00` is 0 and manager `+0x18` is not 0: answer
     `Command available only for developers.` (`0x1721389`..`0x1721392`).
   - Sets manager `+0x44` = 1 unless the name is `stats` (`0x172145D`).
   - Manager `+0x19` not 0 and the name is neither `enable_all_commands` nor
     `stats`: answer `Command not available in multiplayer or ironman mode.`
     (`0x1721463`).
   - Entry handler null: `Console Error - command has missing action.`
   - Otherwise copies `tokens[1..]` into a new vector, calls the hook pointer at
     `0x2357B50` if set, then calls entry `+0x38` with (result, args, entry
     `+0x40`) (`0x172174E`..`0x172176D`), i.e. the handler with the arguments
     without the command name.
   - After the handler, the multiplayer/ironman check runs once more; if it
     fails now, the code writes to address 0 (a deliberate crash). Reaching it
     needs `+0x19` to change during the handler.
8. **Result** (all handlers read, decomp of `0xE4FA00`, `0xE77800`,
   `0xE62330`, `0xE4CE80`): `0x28` bytes; `+0x00` byte success, `+0x08` a
   MSVC `std::string` (`+0x08` buffer or pointer, `+0x18` size, `+0x20`
   capacity, inline when capacity is 15). The text is the only answer. The
   success byte is not reliable: `speed` (`0xE4FA00`) clears it at entry and
   never sets it, even when it changes the speed (disasm: the only writes to
   `[RCX]`/`[RDI]` store 0, at `0xE4FA25`, `0xE4FA5A`, `0xE4FCA1`).
9. **Strings belong to the game's heap.** The game allocates with `0x950B0`
   (aligned for 4 KiB and more) and frees with `0x17394F0` (`_free_base`);
   `0x95660` is the `std::string` destructor that every caller uses on the
   result text (decomp of `0xE8D290`, `0x758430`, `0x116D6C0`).
10. **The tokens are only read.** The dispatcher copies the arguments it passes
    on and never frees or changes the caller's vector (decomp of `0x1721320`).

### Where a line comes from and where output goes

11. **Console window path** `0x166DE20` (`clausewitz/.../tweakergui/console.cpp`;
    decomp): reads the edit box, trims it (`0x17043C0`), stores it in the
    in-memory history (up to `0x200` lines) and appends it to
    `console_history.txt` unless byte `0x1FD10C7` is 0, splits the line on `|`
    and the first part on spaces (`0x17047C0`; empty tokens are kept, so double
    spaces give empty arguments), and calls `0x1721320` with the manager from
    `0x2357B58`. Then it logs `Console command <name> succeeded:` or
    `failed:` and the text through the engine logger (`0x1711D40`,
    `0x16FF900`, source `console.cpp` lines 271 and 273), and adds each output
    line to the window list through `0x166D260`, coloured `§W` (success) or
    `§R` (failure) and closed with `§!`. With exactly one `|`, the text is also
    written to the file named after it (`Output written to `).
12. **Programmatic callers exist and do not open the window.**
    - `run_commands <file>` (`0xE8D290`): reads the file through PhysFS, clears
      `0x1FD10C7` while it runs, splits each line on spaces and calls
      `0x1721320` with the manager directly; the per-line results are
      destroyed, not shown.
    - `runyear <year> <file>` (`0xE4FDB0`) stores the year at `0x233FE44` and
      the file name in the string at `0x1FD33D8`. The daily tick `0x758430`
      (`gamestate.cpp`) compares `root + 0x1DDC` with that year, clears it and
      calls `0x1721320` with the tokens `run_commands`, `<file>` (decomp).
    - Startup option `auto_run` (string `0x1FD39E8`): `0x81DE70`
      (`ingameidler.cpp`) logs `Run console commands from startup option
      auto_run:`, writes `run_commands <file>` into the console's edit box
      (console object pointer `0x23496D0`) and calls `0x166DE20`. This uses the
      console object, not an open window.
    - The map-click command (`0x116D6C0`) appends a province ID or country tag
      to the pending command and calls `0x1721320`.
13. **Some handlers need the console object.** `clear` (`0xE62330`) calls
    methods of the object at `0x23496D0 + 8` without a null check. Most
    handlers use only the game root `0x233FE78` (e.g. `speed`, `debug_mode`).

### Gates

14. **Developer gate.** Manager `+0x18` is written only by `0x6E90F0` (1) and by
    `enable_all_commands` (`0xE43160`, 0). That handler hashes its argument
    (`0x1724B90`, `0x1725020`) and compares the hex text with a constant built
    at first use; only on a match does it clear `+0x19` and `+0x18`. In
    multiplayer under conditions it answers `Cheaters are not welcome here.`
    Checked by listing every function that reads `0x2357B58` or calls the
    getter `0x6E74C0` (xrefs) and searching their listings for byte writes to
    `+0x18`/`+0x19`: the others are the inlined getters (they clear `+0x19`),
    the registration and string terminators of unrelated objects. The `-debug`
    startup option is not read by any of them.
15. **54 records are developer-only** (byte `+0x00` = 0): see the table. They
    include all `debug_*` render toggles, `disable_dlc`, `debug_crash`,
    `dist`, `build_all`, `build_forts`, `deiron`, `set_achievements_ok`,
    `mapclick`. The console variables are reachable only when `+0x18` is 0
    too, so in a normal game both are refused.
16. **`debug_mode` is not the gate.** Its handler `0xE4CE80` toggles root byte
    `+0x23A0` and answers `Debug Mode ENABLED`/`DISABLED`; it does not touch
    the manager.
17. **Multiplayer/ironman gate.** `+0x19` is computed at registration only: 1
    when `0x6EAA10(session)` is true (session vtable `+0x318`, then flags
    `+0x54`/`+0x55` of the object from vtable `+0x80`, and a walk of the list
    at `root + 0xA50`) or root byte `+0x24D0` is set.
18. **Pre-command hook** `0x8740B0`, installed at `0x875898` in the application
    init `0x8757E0`: under the same multiplayer test or with root `+0x24D0`
    set, it clears byte `0x1FC23D8` and sets byte `+0x28A3` of the player's
    country object; otherwise it returns without effect.

### `run <file>` and the speed command

19. **`run` exists and is not developer-only** (record 2, `0x1FC2520`, help
    `Runs effects in specified file`, handler `0xE4F1D0`). The handler resolves
    the name (`0x17061A0`, `PHYSFS_exists`), parses the file as a script and
    runs it as an effect, with a scope taken from `root + 0x1E60` / `0x1E68`
    (the fields the hook in claim 18 uses to find the player's country); it
    answers `<file> executed`, `No such file` or `Wrong format`
    (decomp).
20. **`speed <0-5>`** (`0xE4FA00`): one argument, `atol`; 0 pauses through
    `0x7E02D0`, 1–5 unpause through `0x7E0770` and set speed `n-1` with
    `0x7579F0(root, n-1)`; errors `Need for speed argument` and
    `Speed must be in range 0 to 5`. There is also `gamespeed` (`0xE913A0`,
    `Set the current game speed, pausing the game at speed 0.`).
21. **`echo`** (`0xE77800`) succeeds only with exactly one argument and copies
    it into the result; with the space splitter, `echo a b` fails. This makes
    `echo <token>` a cheap, harmless round-trip test.

## Hypotheses (need a live check)

- **H1 – main thread.** Command handlers expect the main thread: the window
  path is GUI input and the `runyear` path is the daily tick, both of which the
  lab's profiles place on the main thread; nothing in the dispatcher locks.
  Calling `0x1721320` from the pipe thread is unsafe until shown otherwise.
- **H2 – the log file.** `Console command … succeeded:` goes to `game.log`
  (the logger and the source file are known; the channel-to-file mapping is
  not).
- **H3 – flag meanings.** `0x6EAA10` means "multiplayer with other humans" and
  root `+0x24D0` means ironman; the hook `0x8740B0` marks the game as cheated
  (byte `0x1FC23D8` and country `+0x28A3`). In the lab's single-player,
  non-ironman Russia save, manager `+0x19` should read 0 and `+0x18` 1.
- **H4 – map target.** Record `+0x98` (entry `+0xA0`) is the target type the
  map click fills in: 1 province ID, 2 country tag. Consistent with the help
  texts and with `0x116D6C0`, which compares manager `+0x40` with 2.
- **H5 – `+0x44`.** The byte set before every command other than `stats` and
  cleared in `0x74A260` records that the console was used; its reader is not
  found.
- **H6 – direct call has no side output.** Calling `0x1721320` directly adds
  nothing to the console window, `console_history.txt` or the log; only the
  result text comes back.
- **H7 – classification.** The `Class` column of the table is from names and
  help texts only, except the handlers marked `*`.

## Proposed entry point and `[[expect]]` bytes

Call the dispatcher directly on the main thread, with tokens Tongs builds
itself; do not go through the console window.

1. Read the manager pointer from `0x2357B58`. Refuse when it is null or its
   entry vector is empty (before the first registration `+0x18` is not set).
   Do not call the getter `0x6E74C0`: it would allocate a manager with an
   unset `+0x18`.
2. Split the client's line on runs of spaces (the game keeps empty tokens; a
   client should not get empty arguments by accident) and build a
   `std::vector<std::string>` in Tongs' own memory: three pointers
   (begin, end, capacity end) to 32-byte MSVC strings (16-byte inline buffer
   or heap pointer, size at `+0x10`, capacity at `+0x18`, capacity 15 means
   inline). The game only reads them (claim 10), so they need not come from
   the game's heap. Building them by hand avoids depending on the plugin's own
   STL settings.
3. Call `0x1721320(manager, &result, &tokens)` with a zeroed 0x28-byte result.
4. Copy the text out of `result + 8`, then free it with the game's string
   destructor `0x95660(&result + 8)`; never with Tongs' allocator.
5. Answer with the text and the success byte, but treat the byte as a hint
   (claim 8) and, where possible, read the state afterwards (e.g. `speed` is
   checked by reading `root + 0x1E08`).

Before calling, Tongs can read the entry itself (manager vector, `0xA8` stride,
`_stricmp` on name and aliases) to refuse developer-only, unknown or
blocklisted commands with its own message.

Expected bytes (from the EXE file):

```toml
[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x1721320
bytes = "48 89 5C 24 10 48 89 4C 24 08 55 56 57 41 54 41 55 41 56 41 57 48 8D 6C 24 D0 48 81 EC 30 01 00 00 49 8B F0 48 8B FA 4C 8B E1"
name = "console dispatcher: (manager RCX, result RDX, tokens R8)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x172136E
bytes = "41 B0 01 48 8B 16 E8 87 FC FF FF 48 89 85 80 00 00 00 48 85 C0 0F 84 39 06 00 00 38 18 75 61 41 38 5C 24 18 74 5A"
name = "console dispatcher: lookup 0x1721000, entry+0x00 release byte, manager+0x18 developer gate"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x1721463
bytes = "41 80 7C 24 19 00"
name = "console dispatcher: manager+0x19 multiplayer/ironman gate"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x172174E
bytes = "48 8B 05 FB 63 C3 00 48 85 C0 74 02 FF D0 49 8B 47 38 4D 8B 47 40 48 8D 54 24 28 48 8D 4D 08 FF D0"
name = "console dispatcher: pre-command hook 0x2357B50, then entry+0x38(result, args, entry+0x40)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x6E74C0
bytes = "48 83 EC 28 48 8B 05 8D 06 C7 01 48 85 C0 75 3D"
name = "console manager getter: singleton pointer at 0x2357B58"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x95660
bytes = "40 53 48 83 EC 20 48 8B 51 18 48 8B D9 48 83 FA 10 72 2C 48 8B 09"
name = "std::string destructor used to free the result text"
```

Optional, if Tongs lists commands from the static table instead of the
manager:

```toml
[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x6E916F
bytes = "48 8D 1D 6A 92 8D 01 BF 7A 01 00 00"
name = "console registration: 0x17A records of 0xA0 bytes at 0x1FC23E0"
```

## Open questions

- Which thread runs the daily tick and the console window's Enter handler
  (H1), and is there a point in the main loop where Tongs can run a game call
  between frames? This belongs to the map ticket #15, not to the console.
- Which handlers dereference the console object (`0x23496D0`) or the session
  without a check, and so crash in the frontend or during loading? Only
  `clear` and `check_save` (`Cannot use this console command in the
  frontend.`, `0xE95610`) were read.
- Can `+0x19` change between registrations (for example when a multiplayer
  session starts on an already loaded game)? The second check after the
  handler (claim 7) would then crash on purpose.
- The meaning and reader of manager `+0x44` (H5) and of byte `0x1FC23D8`
  (H3).
- Whether Tongs should ever offer the console variables or developer
  commands. That would mean writing manager `+0x18`, which the game only
  clears after a password check; this is a design decision, not a technical
  one, and conflicts with "never write game state directly".
- Where `run` and `run_commands` resolve relative file names (`0x17061A0`,
  PhysFS search path: game directory, mods, user directory?).

## Live checks a later `task` ticket should run

All on the lab copy with the Russia save of 17 March 1719, single player,
not ironman, with Tongs reading memory only until the last step.

1. Read `*(0x2357B58)`: non-null after the save is loaded; entry count
   `(end-begin)/0xA8` is 376 game commands plus up to 8 engine commands
   (registered by their subsystems), `+0x18` = 1,
   `+0x19` = 0 (H3). Repeat in the main menu.
2. Find each name of the table in the manager vector and compare the release
   byte and handler with the table here.
3. From a main-thread game call: `echo tongs` → success byte 1, text `tongs`;
   `version` → the game version; `speed 3` → text empty, byte 0, and
   `root + 0x1E08` = 2 (claim 8); `help speed` → the help text.
4. `debug_zoom` → `Command available only for developers.`; `nosuchcommand` →
   `Unknown command` or the suggestion text.
5. Check that a direct call adds nothing to the console window,
   `console_history.txt` or `game.log`, then type the same command in the
   console window and find the `Console command … succeeded:` line (H2, H6).
6. Record the thread ID of the daily tick and of the console's Enter handler
   (breakpoint-free: log `GetCurrentThreadId` from a game call and compare with
   the thread that owns the window) (H1).
7. Read manager `+0x44` before and after one command, and after loading a save
   (H5).
8. `run <file>` with a one-line effect file in a test mod, confirm the change
   and the answer `<file> executed`; record which directory the name is
   resolved against.

## Command list and classification

378 static records (`0x1FC23E0`, stride `0xA0`), read from the EXE file.
Columns: **Dev only** = record byte `+0x00` is 0 (refused with
`Command available only for developers.` unless `enable_all_commands` passed);
**Map target** = record `+0x98` (H4). **Class**: `state` changes game state
(the simulation, the player's country or the AI), `client` changes only this
client's view, render, UI, timers or statistics, `read` only answers with text,
`file` writes a file, log or clipboard, `flow` runs other commands, scripts or
events, `danger` crashes, leaks, desyncs, deletes achievements or changes the
installation or save rules, `gate` the developer unlock. A `*` marks commands
whose handler was read; the others are classified by name and help text only
(H7). Counts: state 211, client 80, read 36, file 21, danger 18, flow 11,
gate 1. Duplicate names (`yesman`, `add_baseunrest`): the later record wins.

Engine commands outside this table (claim 5): `CrashReporter.DeleteCrashData`
(file), `CrashReporter.SimulateCrash` (danger), `CrashReporter.UploadCrash`
(danger: network upload), `net_debuginfo` (read), `social_debuginfo` (read),
`social_addfriend`, `social_joinroom`, `social_sendmessage` (danger: online
actions).

| # | Command | Aliases | Arguments (help text) | Dev only | Map target | Class | Help text |
|---:|---|---|---|:-:|:-:|---|---|
| 0 | `enable_all_commands` |  |  |  |  | gate |  |
| 1 | `center_of_trade` | `cot` | CoT Level Province ID |  |  | state | Create or upgrade/downgrade a center of trade |
| 2 | `run` |  |  |  |  | flow* | Runs effects in specified file |
| 3 | `assert` |  |  |  |  | flow | Asserts trigger (otherwise causes test failure) |
| 4 | `country_modifier` |  |  |  |  | read | Show country modifier values. |
| 5 | `province_modifier` |  |  |  |  | read | Show province modifier values. |
| 6 | `nopausetext` |  |  |  |  | client | Toggles the pausebanner for nicer screenshots. |
| 7 | `nextsong` |  |  |  |  | client | Changes the currently playing soundtrack. |
| 8 | `date` |  | date in format yyyy.mm.dd |  |  | state | Changes current date |
| 9 | `time` |  |  |  |  | read | What time is it? |
| 10 | `oos` |  |  |  |  | danger | Make the client go oos |
| 11 | `print_modifiers` | `file` |  |  |  | file | Print gamestate modifiers to a file |
| 12 | `combatsound` |  |  |  |  | client | How often does the combat view give a random sound? 0-50 |
| 13 | `morehumans` | `humans` | num |  |  | state | Adds more humans |
| 14 | `colonize` |  | province |  | province | state |  |
| 15 | `fast_colonize` |  |  |  |  | state |  |
| 16 | `window` | `wnd` | Arguments: open/close window gui name |  |  | client | Opens or closes the specified window |
| 17 | `smoothframelog` |  |  |  |  | client | Toggles smooth frame logging |
| 18 | `reload` |  | file name |  |  | client | Reloads the gui or lua file |
| 19 | `reloadtradewinds` |  |  |  |  | client | Reloads trade winds |
| 20 | `reloadinterface` |  |  |  |  | client | Reloads the entire interface |
| 21 | `reloadfx` |  | Arguments: map/mapname/postfx or *.fx filename |  |  | client | Reloads the shader |
| 22 | `reloadtexture` |  | texture file name |  |  | client | Reloads the specified texture |
| 23 | `reloadloc` |  |  |  |  | client | Reloads the localisation |
| 24 | `update_loc` |  | localization tag |  |  | file | Updates the localization tag file |
| 25 | `verify_loc` |  |  |  |  | read | Verifies the localization keys |
| 26 | `poll` |  |  |  |  | read | Polls valid Events |
| 27 | `startup_screen` |  |  |  |  | danger | Creates a new startup screen |
| 28 | `epicfail` |  | &lt;Target Country Tag&gt; OPTIONAL |  | country | state | Makes all spies of target country fail |
| 29 | `event` |  | event id &lt;Target Country Tag&gt; OPTIONAL &lt;Option id&gt; OPTIONAL |  |  | flow | Executes an event |
| 30 | `disaster` |  | distaste_key |  |  | state | Adds 100 progress to a disaster |
| 31 | `ai` | `ai` | &lt;Target Country Tag&gt; |  | country | state | Enables/Disables AI for all nations or a specific tag |
| 32 | `spectate` | `spectate`, `observe`, `spectator` |  |  |  | client | Goes into Spectator mode |
| 33 | `rgb` | `rgb` | &lt;Amount&gt; |  |  | file | Prints desired amount of random color values to game.log |
| 34 | `reset_god` |  |  |  |  | state | Resets Personal Deity |
| 35 | `die` | `kill` | &lt;Target Country Tag&gt; OPTIONAL |  | country | state | Kills the monarch of a tag |
| 36 | `add_monarch` |  | &lt;Target Country Tag&gt; OPTIONAL |  | country | state | Adds a monarch to a tag |
| 37 | `add_heir` |  | &lt;Target Country Tag&gt; OPTIONAL |  | country | state | Adds a heir of a tag |
| 38 | `kill_heir` |  | &lt;Target Country Tag&gt; OPTIONAL |  | country | state | Kills the heir of a tag |
| 39 | `add_consort` |  | &lt;Target Country Tag&gt; OPTIONAL &lt;Origin Country Tag&gt; OPTIONAL |  |  | state | Adds a consort from a tag |
| 40 | `kill_consort` |  | &lt;Target Country Tag&gt; OPTIONAL |  | country | state | Kills your consort |
| 41 | `siege` |  | &lt;Province ID&gt; |  | province | state | Wins the siege in the specified province |
| 42 | `annex_all` |  |  |  | country | state | Annexes all other countries |
| 43 | `annex` |  | &lt;Target Country Tag&gt; &lt;Actor Country tag&gt; OPTIONAL |  | country | state | Begin annex/annexes the specified tag |
| 44 | `reform` |  | &lt;Target Province ID&gt; |  | province | state | Begin reformation in the specified province ID |
| 45 | `institution` |  | &lt;Province ID&gt; &lt;institution tag&gt; &lt;Amount&gt; OPTIONAL |  | country | state | Add institution embracement to a province. |
| 46 | `remove_country` |  | &lt;Target Country Tag&gt; |  | country | state | Obliterate a country from the map |
| 47 | `integrate` |  | &lt;Target Country Tag&gt; &lt;Actor Country tag&gt; OPTIONAL |  | country | state | Begin integrate/integrates the specified tag |
| 48 | `vassalize` |  | &lt;Target Country Tag&gt; &lt;Actor Country tag&gt; OPTIONAL |  | country | state | Vassalize the specified tag |
| 49 | `create_march` |  | &lt;Target Country Tag&gt; &lt;Actor Country tag&gt; OPTIONAL |  | country | state | make a march the specified tag |
| 50 | `remove_march` |  | &lt;Target Country Tag&gt; &lt;Actor Country tag&gt; OPTIONAL |  | country | state | make a march the specified tag |
| 51 | `discover` |  | &lt;Target Country Tag&gt; &lt;Actor Country tag&gt; OPTIONAL |  | province | state | Discover capital of target tag |
| 52 | `modifiers_debug` |  |  |  | country | read | Show all modifiers and icons in country modifier view. |
| 53 | `golden_age` |  | &lt;Target Country Tag&gt; |  | country | state | Give Golden Age to a tag. |
| 54 | `winwars` |  |  |  |  | state | Gives max war score in all wars for the country |
| 55 | `spritelevel` |  | &lt;Sprite level&gt; |  |  | client | Sets a forced sprite level. Specify no argument to reset. |
| 56 | `sprite_pack_debug` |  | &lt;CountryTag&gt; Optional |  |  | read | Prints debug data for sprite packs. |
| 57 | `add_cardinal` |  |  |  |  | state | Add a new Cardinal |
| 58 | `kill_cardinal` |  | &lt;Countrytag&gt; OPTIONAL |  | country | state | Kill a Cardinal |
| 59 | `clear` |  |  |  |  | flow* | Clears the console |
| 60 | `clearae` |  | &lt;Against Country Tag&gt;, &lt;By Country Tag (empty for all)&gt; |  | country | state | Clears Aggressive Expansion |
| 61 | `validateevents` |  |  |  |  | read | Validates all events without triggering it |
| 62 | `testevent` |  | &lt;Event ID&gt; &lt;Character ID&gt; |  |  | read | Tests an event without triggering it |
| 63 | `cash` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds gold to the player |
| 64 | `trust` |  | &lt;Target Country tag&gt; &lt;Amount&gt; |  |  | state | Adds trust with target country |
| 65 | `spynetwork` |  | &lt;Target Country tag&gt; &lt;Amount&gt; |  |  | state | Adds spy network with target country |
| 66 | `favors` |  | &lt;Target Country tag&gt; &lt;Amount&gt; |  |  | state | Adds favors with target country |
| 67 | `add_loyalty` |  | &lt;Target Estate&gt; &lt;Amount&gt; |  |  | state | Adds loyalty to target estate |
| 68 | `church_power` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds church power to the player |
| 69 | `karma` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds karma to the player |
| 70 | `fervor` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds fervor to the player |
| 71 | `army_tradition` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds army tradition to the player |
| 72 | `navy_tradition` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds army tradition to the player |
| 73 | `splendor` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds splendor to the player |
| 74 | `absolutism` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds Absolutism to the player |
| 75 | `army_professionalism` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Sets army professionalism for target country |
| 76 | `religious_school` |  | School &lt;Target Country tag&gt; OPTIONAL |  |  | state | Sets religious school for target country |
| 77 | `age` |  | &lt;which age&gt; |  |  | state | age |
| 78 | `population` |  | &lt;ProvinceID&gt; &lt;Amount&gt; |  | province | state | Adds population to a province. |
| 79 | `setmissionaryprogress` |  | &lt;ProvinceID&gt; &lt;Amount&gt; |  | province | state | Sets the missionary progress for a province. |
| 80 | `manpower` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds manpower to player |
| 81 | `sailors` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Adds sailors to player |
| 82 | `add_natives` |  | &lt;ProvinceID&gt; &lt;Amount&gt; |  |  | state | Add natives to given province |
| 83 | `native_uprising` |  | &lt;ProvinceID&gt; |  | province | state | Create native uprising in given province |
| 84 | `pass_next_reform` |  | &lt;Amount&gt; |  |  | state | Add reforms to empire |
| 85 | `add_liberty_desire` |  | &lt;Amount&gt; &lt;Target Country tag&gt; OPTIONAL |  |  | state | Add liberty desire to the player |
| 86 | `add_opinion` |  | &lt;Actor Country tag&gt; &lt;Target Country tag&gt; OPTIONAL |  | country | state | Add opinion to/from tag |
| 87 | `combat_dice` |  | &lt;Size&gt; |  |  | state | Set Fixed Dice (-1 to disable) |
| 88 | `add_pa` |  | &lt;Country tag&gt; |  | country | state | Add patriach authority to tag |
| 89 | `add_cb` |  | &lt;casus belli tag&gt; &lt;target country tag&gt; &lt;Actor Country tag&gt; OPTIONAL |  |  | state | Add casus belli against target country |
| 90 | `remove_cb` |  | &lt;casus belli tag&gt; &lt;target country tag&gt; &lt;Actor Country tag&gt; OPTIONAL |  |  | state | Remove casus belli from target country |
| 91 | `declare_war` |  | &lt;attacker tag&gt; &lt;defender tag&gt; &lt;call willing allies&gt; |  |  | state | Declares war between two nations (no CB) |
| 92 | `tag` |  | &lt;Country tag&gt; |  | country | state | Switch tag to another country |
| 93 | `remove_defender_faith` |  |  |  |  | state | Removes the defender of the Faith for the players religion |
| 94 | `form_union` |  | &lt;Country tag&gt; &lt;Country tag&gt; OPTIONAL |  | country | state | Forms Union with target |
| 95 | `add_missionary` |  | &lt;Country tag&gt; |  | country | state | Adds a missionary to a country |
| 96 | `add_pi` |  | &lt;Country tag&gt; |  | country | state | Add papal influence to tag |
| 97 | `add_colonist` |  | &lt;Country tag&gt; |  | country | state | Adds a colonist to a country |
| 98 | `add_interest` |  | &lt;Country tag&gt; |  | country | state | Add specified country tag to your interest |
| 99 | `remove_interest` |  | &lt;Country tag&gt; |  | country | state | Removes specified country tag from your interest |
| 100 | `add_diplo` |  | &lt;Country tag&gt; OPTIONAL |  | country | state | Adds diplomatic entroute |
| 101 | `add_idea_group` |  | &lt;Idea group key&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Adds the specified idea group |
| 102 | `tech` |  | &lt;AMOUNT&gt; |  |  | state | Get levels in all techs |
| 103 | `power` |  | &lt;stability/tech_table_key/idea_key&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Adds the specified idea group |
| 104 | `powerpoints` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get power in all powers |
| 105 | `adm` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get administrative power |
| 106 | `paper_mana` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get administrative power |
| 107 | `dip` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get diplomatic power |
| 108 | `bird_mana` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get diplomatic power |
| 109 | `mil` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get military power |
| 110 | `sword_mana` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get military power |
| 111 | `add_republican_tradition` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get republican tradition |
| 112 | `piety` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Get/Add piety |
| 113 | `stability` |  | &lt;AMOUNT&gt; OPTIONAL &lt;Country tag&gt; OPTIONAL |  |  | state | Increase the stability of your country |
| 114 | `imperial_authority` |  | &lt;Amount&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Increase your Imperial Authority |
| 115 | `lucky` |  | &lt;Country tag&gt; OPTIONAL |  | country | state | Toggle lucky nation |
| 116 | `exhaust` |  | &lt;Amount&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Alter your war exhaustion |
| 117 | `prestige` |  | &lt;Amount&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Increase your prestige |
| 118 | `mandate` |  | &lt;Amount&gt; OPTIONAL |  |  | state | Increase your mandate |
| 119 | `government_power` | `russian` | &lt;Amount&gt; OPTIONAL |  |  | state | Increase all your government powers |
| 120 | `incident` |  | &lt;IncidentTag&gt;, &lt;CountryTag&gt; OPTIONAL |  |  | state | Trigger an incident for a country. |
| 121 | `isolationism` |  |  |  |  | state | Set your isolation level |
| 122 | `inflation` |  | &lt;Amount&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Increase your inflation |
| 123 | `PrintSynchStuff` |  |  |  |  | read | Prints random count and seed |
| 124 | `SetRandomCount` |  |  |  |  | state | Sets the randomcount to 0 or arg |
| 125 | `stop` |  | &lt;YYYY.MM.DD&gt; |  |  | flow | Pauses the game at a specified date |
| 126 | `fow` | `debug_fow` | &lt;Province ID&gt; OPTIONAL |  | province | client | Turns off fog of war in a province or in general |
| 127 | `collision` | `debug_collision` |  |  |  | client | Toggles debug display of normals/bounding boxes/collision |
| 128 | `ti` | `debug_ti` |  |  |  | client | Toggles Terra Incognita on/off |
| 129 | `savegame` |  |  |  |  | file | Creates an savefile. |
| 130 | `autosave` |  |  |  |  | file | Triggers an autosave. |
| 131 | `test` |  | &lt;test ID&gt; |  |  | flow | Runs a code test |
| 132 | `reinitchat` |  |  |  |  | danger | Posts a CChatSyncAllCommand |
| 133 | `nudge` |  |  |  |  | client | Go to the nudge tool |
| 134 | `embrace` |  | &lt;Province ID&gt;] [&lt;institution name&gt; optional |  | province | state | Embraces all institutions in a province |
| 135 | `echo` |  | Echo |  |  | read* | Echoes the arguments in the console (also useful for commenting script files) |
| 136 | `invasion_target` |  | &lt;Province ID&gt; |  | province | state | Force sets AI invasion target to province |
| 137 | `revolt` |  | &lt;Province ID&gt; |  | province | state | Starts a Revolt in a province |
| 138 | `revolution_target` |  | &lt;CountryTag&gt; |  | country | state | Sets the revolution target. |
| 139 | `add_local_autonomy` |  | &lt;Province ID&gt; &lt;Amount&gt; |  |  | state | Adds local autonomy to a province. |
| 140 | `add_unrest` |  | &lt;CID Type&gt; &lt;CID Index&gt; &lt;Unrest&gt; |  |  | state | Adds unrest to a faction |
| 141 | `add_baseunrest` |  | &lt;Province ID&gt; &lt;Base Unrest&gt; |  |  | state | Adds base unrest to a province |
| 142 | `add_baseunrest` |  | &lt;Province ID&gt; &lt;Base Unrest&gt; |  |  | state | Adds base unrest to a province |
| 143 | `add_devastation` |  | &lt;Province ID&gt; &lt;Devastation&gt; |  |  | state | Adds devastation to a province |
| 144 | `add_prosperity` |  | &lt;Province ID&gt; &lt;Prosperity&gt; |  |  | state | Adds Prosperity to a state (provinceid is used) |
| 145 | `pirate` |  | &lt;Province ID&gt; |  | province | state | Starts a Pirate in a province |
| 146 | `legitimacy` |  | &lt;AMOUNT&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set the legitimacy of the ruler |
| 147 | `corrupt` |  | &lt;AMOUNT&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set the corruption of the reich |
| 148 | `horde_unity` |  | &lt;AMOUNT&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set the unity of a horde |
| 149 | `meritocracy` |  | &lt;AMOUNT&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set the meritocracy of the Chinese Emperor |
| 150 | `devotion` |  | &lt;AMOUNT&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set the devotion of a theocracy |
| 151 | `doom` |  | &lt;AMOUNT&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Add to a country's doom |
| 152 | `authority` |  | &lt;AMOUNT&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Add to a country's authority |
| 153 | `mapmode` |  | Mapmode type (int) |  |  | client | Change mapmode. |
| 154 | `fullscreen` |  |  |  |  | client | Toggles fullscreen |
| 155 | `score` |  |  |  |  | read | Score output. |
| 156 | `stats` |  |  |  |  | file | Stats to stats file. |
| 157 | `balance` |  |  |  |  | read | Region Balance output. |
| 158 | `prices` |  |  |  |  | read | Price Info |
| 159 | `add_core` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Add core |
| 160 | `remove_core` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Remove core |
| 161 | `add_faction` |  | &lt;factionname&gt; |  |  | state | Adds a Faction to your country |
| 162 | `add_claim` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Add claim |
| 163 | `add_permanent_claim` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Add permanent claim |
| 164 | `remove_claim` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Remove claim |
| 165 | `delete_wars` |  | &lt;Country tag&gt; OPTIONAL |  | country | state | Delete wars without removing truces |
| 166 | `test_achievement` |  | &lt;achievement&gt; |  |  | danger | tests a specific achievement |
| 167 | `culture` |  | &lt;Province ID&gt; |  | province | state | Changes culture on a province to your primary culture |
| 168 | `state` |  | &lt;Province ID&gt; |  | province | state | Toggles State for owner of region of province. |
| 169 | `own` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Change ownership |
| 170 | `own_core` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Change ownership + add core |
| 171 | `controll` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Change controller |
| 172 | `control` |  | &lt;Province ID&gt; &lt;Country tag&gt; OPTIONAL |  | province | state | Change controller |
| 173 | `leader` |  | &lt;fire&gt; &lt;shock&gt; &lt;maneuver&gt; &lt;siege&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Create a leader |
| 174 | `admiral` |  | &lt;fire&gt; &lt;shock&gt; &lt;maneuver&gt; &lt;siege&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Create an admiral |
| 175 | `debug_zoom` |  |  | yes |  | client | Zooms in the game |
| 176 | `debug_commands` |  |  | yes |  | file | Printing commandcount to message.log |
| 177 | `debug_countrynames` |  |  | yes |  | file | printing countrynames & tags to game.log |
| 178 | `debug_eventtriggers` |  |  | yes |  | client | Debug event triggers |
| 179 | `disable_dlc` |  |  | yes |  | danger | Disable All DLC |
| 180 | `debug_events` |  |  | yes |  | client | Start Counting events |
| 181 | `debug_dumpevents` |  |  | yes |  | file | Dump Event data to game log |
| 182 | `debug_diploactions` |  |  | yes |  | client | Start Counting diplomatic actions |
| 183 | `debug_dumpdiploactions` |  |  | yes |  | file | Dump diplomatic action data to game log |
| 184 | `debug_reload_regions` |  |  |  |  | client | Reload map regions |
| 185 | `debug_reload_areas` |  |  |  |  | client | Reload map areas |
| 186 | `debug_assert` |  |  | yes |  | client | Toggles asserts on/off |
| 187 | `debug_smooth` |  |  | yes |  | client | Toggle framesmoothing |
| 188 | `debug_terrain` |  |  | yes |  | client | Toggles Terrain on/off |
| 189 | `debug_hires` |  |  | yes |  | client | Toggles Terrain Hires mode on/off |
| 190 | `debug_water` |  |  | yes |  | client | Toggles Water on/off |
| 191 | `debug_borders` |  |  | yes |  | client | Toggles Borders on/off |
| 192 | `debug_trees` |  |  | yes |  | client | Toggles Trees on/off |
| 193 | `debug_rivers` |  |  | yes |  | client | Toggles Rivers on/off |
| 194 | `debug_lakes` |  |  | yes |  | client | Toggles Lakes on/off |
| 195 | `debug_postfx` |  |  | yes |  | client | Toggles PostFX on/off |
| 196 | `debug_sky` |  |  | yes |  | client | Toggles Sky on/off |
| 197 | `debug_bloom` |  |  | yes |  | client | Toggles Bloom on/off |
| 198 | `filewatcher` |  |  | yes |  | client | Toggles filewatcher |
| 199 | `debug_tooltip` |  |  | yes |  | client | Toggles Tooltips on/off |
| 200 | `debug_citysprawl` |  |  | yes |  | client | Toggles Citysprawl on/off |
| 201 | `debug_nogui` |  |  |  |  | client | Toggles GUI on/off |
| 202 | `debug_volume` |  | &lt;Volume Delta&gt; | yes |  | client | Modifies music volume |
| 203 | `debug_lockcamera` |  |  | yes |  | client | Toggles Camera locked on/off |
| 204 | `debug_lines` |  |  | yes |  | client | Toggles Debuglines |
| 205 | `debug_info` |  |  |  |  | client | Toggles Debug info |
| 206 | `debug_particle` |  |  | yes |  | client | Toggles Particles Debug info |
| 207 | `debug_ai_budget` |  | CountryTag | yes | country | client | Show ai budget data |
| 208 | `observe_mode` |  |  |  |  | client | Changes the observe mode |
| 209 | `debug_textures` |  |  | yes |  | file | Writes Texture info to application debug log |
| 210 | `debug_wireframe` |  |  | yes |  | client | Toggles forced wireframe on/off |
| 211 | `debug_name` |  | &lt;Province ID&gt; OPTIONAL | yes | province | client | Generates names for provinces |
| 212 | `debug_achievements_clear` |  |  |  |  | danger | Clear all achievements and user stats |
| 213 | `debug_achievements` |  |  |  |  | danger | Enables popups for achievements to debug them |
| 214 | `moveunit` |  | &lt;Unit ID&gt; &lt;Province ID&gt; | yes |  | state | Moves a unit to a province |
| 215 | `init_tax` |  |  | yes |  | state | Init original base tax |
| 216 | `spawnactor` |  | &lt;Actorname&gt; &lt;Province ID&gt; &lt;Animation&gt; OPTIONAL |  |  | client | Spawns an actor with an optional animation |
| 217 | `spawn` |  | &lt;Province ID&gt; &lt;SubUnit Type&gt; |  |  | state | Spawns a unit in a province |
| 218 | `spawn_all_units` |  | &lt;Animation state&gt; |  |  | danger | Spawns all units in capital for all existing countries. Requires unit_tester mod under Art/Test Resources enabled. |
| 219 | `testprovs` |  | &lt;First Province ID&gt; &lt;Second Province ID&gt; | yes |  | client | Test graphical positions of provinces |
| 220 | `testrng` |  | &lt;Number of samples&gt; | yes |  | client | Test RNGs |
| 221 | `trade_recalc` |  |  | yes |  | state | Recalculates trade node provinces |
| 222 | `trade_edit` |  |  | yes |  | client | Toggles Tradedit |
| 223 | `trade_save` |  |  | yes |  | file | Saves current trade node database to file |
| 224 | `change_religion` |  | &lt;province ID&gt; &lt;religion&gt; |  |  | state | Change religion of a province or the state religion of the current country |
| 225 | `selflearningai` |  |  |  |  | state | Enables/Disables Self-Learning AI |
| 226 | `bearhaslanded` |  |  |  |  | danger | An unwise command to use |
| 227 | `aiview` |  |  |  |  | client | Toggles additional AI info |
| 228 | `ai_minister` |  | &lt;0 = army, 1 = diplomacy, 2 = trade&gt; |  |  | state | Enables/Disables specific AI ministers |
| 229 | `ai_recalc_strategy` |  | &lt;Country&gt; OPTIONAL |  |  | state | Recalculates AI strategy immediately |
| 230 | `yesman` |  |  |  |  | state | Toggles AI positive responses |
| 231 | `memory` |  |  |  |  | read | Prints out the used memory |
| 232 | `leak` |  |  |  |  | danger | Leaks memory |
| 233 | `leak_gpu` |  | Chunk size (bytes) Number of Chunks |  |  | danger | Leaks GPU memory |
| 234 | `refreshknowledgecount` |  |  |  |  | client | Refresh knowledge perf couner |
| 235 | `economy` |  |  |  |  | read | Prints out global economy statistics |
| 236 | `ai_budget` |  | &lt;tag&gt; OPTIONAL |  | country | read |  |
| 237 | `powerspend_count` |  |  |  |  | read | Prints out number of times powerspend was executed |
| 238 | `powerspend` |  |  |  |  | read | Prints out global powerspend statistics |
| 239 | `heir_history` |  |  |  |  | read | Prints out heir history |
| 240 | `reset_powerspend` |  |  |  |  | client | Resets global powerspend statistics |
| 241 | `reset_economy` |  |  |  |  | client | Resets global economy statistics |
| 242 | `diplocount` |  |  |  |  | read | Prints out global diploaction statistics |
| 243 | `timer` |  |  |  |  | read | Prints out debug timing info |
| 244 | `timer_start` |  |  |  |  | client | Starts debug timing |
| 245 | `timer_show` |  |  |  |  | client | Shows live timing data |
| 246 | `timer_restart` |  |  |  |  | client | Restarts (resets and starts) debug timing |
| 247 | `timer_reset` |  |  |  |  | client | Resets debug timing |
| 248 | `timer_stop` |  |  |  |  | client | Stops debug timing |
| 249 | `timer_dump` |  |  |  |  | file | Dumps debug timing info |
| 250 | `ct` |  |  |  |  | file | Puts timer info in clipboard |
| 251 | `hrereforms` |  |  | yes |  | state | Toggles ability to enact HRE reforms |
| 252 | `popup_pause_all` |  |  |  |  | client | Sets all messages to popup and pause |
| 253 | `guibounds` |  |  |  |  | client | Toggles GUI bounds debug |
| 254 | `msg` |  |  |  |  | client | Toggles all messages popup |
| 255 | `great_project` |  | &lt;great project tag&gt; |  | country | state | Create or destroy a great project |
| 256 | `canals` |  |  |  |  | client | Debug canals on map |
| 257 | `reload_canals` |  |  |  |  | client | Reload the canal bitmaps |
| 258 | `reload_straits` |  |  |  |  | client | Update strait vertex buffers |
| 259 | `map_vertextextures` |  |  |  |  | client | Toggle map rendering with vertex textures |
| 260 | `reload_map` |  |  |  |  | client | Reloads the map |
| 261 | `reload_heightmap` |  |  |  |  | client | Reloads the heightmap |
| 262 | `reload_provincemap` |  |  |  |  | client | Reloads the provincemap |
| 263 | `reload_treemap` |  |  |  |  | client | Reloads the treemap |
| 264 | `reload_lakes` |  |  |  |  | client | Reloads the lakes |
| 265 | `debug_mode` |  |  |  |  | client* | Makes some debug features available |
| 266 | `texture_usage` |  |  |  |  | read | Prints texture usage |
| 267 | `low_memory` |  |  |  |  | danger | Simulates a low memory alert and tries to reduce memory usage |
| 268 | `speed` |  | &lt;0-5&gt; |  |  | state* | Sets game speed |
| 269 | `runyear` |  | &lt;year&gt; &lt;command filename&gt; |  |  | flow* | Runs command once we reach year |
| 270 | `nowar` |  |  |  |  | state | Toggles AI war declarations |
| 271 | `norevolts` |  |  |  |  | state | Toggles revolts |
| 272 | `yesman` |  |  |  |  | state | Toggles AI positive responses |
| 273 | `aiinvalid` |  |  |  |  | read | Print invalid AI command counts |
| 274 | `zombie` |  |  |  |  | state | zombie army debug on/off |
| 275 | `god` |  |  |  |  | state | godmode on/off |
| 276 | `infinite_combat` |  |  |  |  | state | Infinite combat |
| 277 | `map_random` |  | &lt;seed=-1&gt; &lt;restore&gt; &lt;nosmooth&gt; &lt;nosmoothcoasts&gt; &lt;topology&gt; &lt;terrain&gt; &lt;colormap&gt; &lt;minimap&gt; &lt;rivers&gt; &lt;trees&gt; |  |  | danger | Randomize new world part of map (heightmap, terrain, ...) |
| 278 | `run_commands` |  |  |  |  | flow* | Runs the specified file with list of console commands |
| 279 | `debug_crash` | `crash` |  | yes |  | danger | Crash! |
| 280 | `helplog` |  |  |  |  | file | Print out all console commands to game.log file. |
| 281 | `help` |  | command name |  |  | read* | Print out all console commands or a specific command description. |
| 282 | `helphelp` |  |  |  |  | read | Double Rainbow help. |
| 283 | `findcommands` |  | &lt;Search string&gt; |  |  | read | Finds console commands |
| 284 | `dist` |  | &lt;Province ID&gt;, &lt;Province ID&gt; | yes |  | read | Prints the navigation distance between two provinces |
| 285 | `age_ruler` |  | &lt;age&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set rulers age |
| 286 | `age_heir` |  | &lt;age&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set heirs age |
| 287 | `age_consort` |  | &lt;age&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set consorts age |
| 288 | `remove_rival` |  | &lt;Country tag&gt; |  | country | state | Remove rival |
| 289 | `show_ideas` |  | &lt;Country tag&gt; |  | country | read | Show Ideas |
| 290 | `ideadump` |  |  |  |  | file | Dump all countries that have generic national ideas |
| 291 | `frenzy` |  |  |  |  | state | Force enable Great Power frenzy!!! |
| 292 | `frenzy_off` |  |  |  |  | state | Force disable Great Power frenzy!!! |
| 293 | `add_trait` |  | &lt;personality tag&gt;, &lt;heir&gt; OPTIONAL, &lt;consort&gt; OPTIONAL |  |  | state | Add ruler personality |
| 294 | `remove_trait` |  | &lt;personality tag&gt; |  |  | state | Remove ruler personality |
| 295 | `set_leader_trait` |  | &lt;personality&gt; |  |  | state | Set selected leader's personality trait |
| 296 | `kill_advisors` |  | &lt;Country tag&gt; |  | country | state | Kills all advisors in the court and from the pool |
| 297 | `repair` |  | &lt;Country tag&gt; |  | country | state | Repairs all ships of country |
| 298 | `add_issue` |  | &lt;issue tag&gt; |  |  | state | Add issue |
| 299 | `add_backer` |  | &lt;prov id&gt; |  | province | state | Add backer |
| 300 | `build_all` |  | &lt;Province ID&gt; | yes | province | state | Build all basic buildings in a province |
| 301 | `version` |  |  |  |  | read | Prints the game version |
| 302 | `cv` |  |  |  |  | file | Prints the game version, and copies to clipboard. |
| 303 | `victorycard` |  |  |  |  | state | Force enables victory cards in singleplayer |
| 304 | `surrender` |  |  |  |  | state | Surrender |
| 305 | `touch_test` |  |  |  |  | client | Testing touch |
| 306 | `fast_diplo` |  |  |  |  | state | Ignore the date check for enroute diplomats |
| 307 | `build_forts` |  | &lt;FortLevel(1-4)&gt; | yes |  | state | Build fort in a province or in all provinces |
| 308 | `evil_dead` |  | &lt;0xDEAD&gt; | yes |  | state | Record your current wars as having X many men lost of each unit type |
| 309 | `diplomacy_info` |  |  |  |  | read | Print summary information about currently active diplomatic relations. |
| 310 | `set_achievements_ok` |  |  | yes |  | danger | Sets 'achievements_ok' flag true. |
| 311 | `purge_mem` |  |  | yes |  | danger | Purges unused texture and audio mem. |
| 312 | `print_unit_names` |  |  | yes |  | read | Print names of army units |
| 313 | `condottieri` |  |  | yes |  | state | Rent out an army to another country |
| 314 | `deiron` |  |  | yes |  | danger | Allows local Ironman games to be saved in text format |
| 315 | `kill_leader` |  |  |  |  | state | Kill a random leader with given death cause |
| 316 | `gamespeed` |  | speed (0-5) |  |  | state* | Set the current game speed, pausing the game at speed 0. |
| 317 | `set_prov_flag` |  | &lt;Flag&gt; &lt;Province&gt; |  |  | state | Sets a flag for a province |
| 318 | `clr_prov_flag` |  | &lt;Flag&gt; &lt;Province&gt; |  |  | state | Clears a flag from a province |
| 319 | `set_flag` |  | &lt;Flag&gt; |  |  | state | Sets a global flag |
| 320 | `clr_flag` |  | &lt;Flag&gt; |  |  | state | Clears a global flag |
| 321 | `set_country_flag` |  | &lt;Flag&gt; |  |  | state | Sets a country flag |
| 322 | `clr_country_flag` |  | &lt;Flag&gt; |  |  | state | Clears a country flag |
| 323 | `set_consort_flag` |  | &lt;Tag&gt; &lt;Flag&gt; |  |  | state | Sets a global flag |
| 324 | `clr_consort_flag` |  | &lt;Tag&gt; &lt;Flag&gt; |  |  | state | Clears a global flag |
| 325 | `print_flags` |  |  |  |  | read | Prints global flags |
| 326 | `print_prov_flags` |  | &lt;Prov ID&gt; |  | province | read | Prints province flags |
| 327 | `make_subject` |  | &lt;Tag&gt; |  |  | state | Make country a subject |
| 328 | `tributary` |  | &lt;Tag&gt; |  | country | state | Make country a Tributary State |
| 329 | `harmony` |  | &lt;AMOUNT&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Add to a country's Harmony |
| 330 | `harmonization` |  | &lt;AMOUNT [0, 100]&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Set to a country's Harmonization progress |
| 331 | `army_drill` |  | &lt;AMOUNT [0.0, 100.0]&gt; &lt;Country tag&gt; OPTIONAL |  |  | state | Sets drill for all your subunits |
| 332 | `test_neighbour` |  | &lt;Tag&gt; |  | country | read | Will tell you if target Tag counts as neighbour to your country |
| 333 | `refresh_knowledge` |  |  |  |  | state | Will recalc bunch of stuff like neighbours, idea rules, etc. |
| 334 | `ai_plan_regions` |  | &lt;Country tag&gt; OPTIONAL |  | country | state | Makes ai reevaluate all region assignments |
| 335 | `ai_army_tick` |  | &lt;Country tag&gt; OPTIONAL &lt;File name for logging&gt; OPTIONAL |  | country | state | Makes ai reevaluate all army movements |
| 336 | `writetestentities` |  |  |  |  | file |  |
| 337 | `check_save` |  |  |  |  | flow | Check save and load persistence |
| 338 | `innovativeness` |  |  |  |  | state | Add Innovativeness |
| 339 | `reformprogress` |  | Points to add Target tag &lt;optional&gt; |  |  | state | Add Government reform progress |
| 340 | `clearreforms` |  | Target tag &lt;optional&gt; |  | country | state | Removes all government reforms |
| 341 | `resetandreloadmissions` | `crash` |  | yes |  | state | Resets completed missions for all countries and reloads missions from disk |
| 342 | `lock_merchants` |  |  | yes |  | state | Locks merchants so they can't be placed anymore. Is a toggle, call again to unlock. |
| 343 | `test_trade_steering` |  |  | yes | country | state | Places merchants of target country as if you would have used Trade Steering Diplo Action/Peace Treaty. |
| 344 | `change_religion_court` |  | &lt;ruler/heir/consort/admadv/dipadv/miladv&gt; &lt;Religion&gt; |  |  | state | Changes the religion of a court member. |
| 345 | `change_culture_court` |  | &lt;ruler/heir/consort/admadv/dipadv/miladv&gt; &lt;Culture&gt; |  |  | state | Changes the culture of a court member. |
| 346 | `mapclick` |  | &lt;Command&gt; | yes |  | flow | Set map click command (Ctrl + Alt + Left Click) |
| 347 | `insult` |  | Sender Receiver Insult Index/Key |  |  | state | Sends a  |
| 348 | `mission_tree` |  | &lt;true/false&gt; &lt;Tag&gt; |  |  | state | Toggles mission tree completion |
| 349 | `mission` |  | Mission identifier |  |  | state | Toggles mission completion |
| 350 | `invest` |  | Investment Key Province ID |  |  | state | Toggles a Trade Company Investment defined by investment key in Trade Company defined by Province ID |
| 351 | `reshuffle_seat_bribes` |  |  |  |  | state | Will reshuffle all the seat bribes |
| 352 | `assimilate` |  | &lt;Province&gt; &lt;Tag&gt; OPTIONAL |  | province | state | Change ownership, core, set culture and religion |
| 353 | `on_action` |  | &lt;Identifier&gt; |  |  | flow | Will run the giver on_action |
| 354 | `syntheticdawn` |  | &lt;Target Tag&gt; OPTIONAL |  |  | danger | The dawn of a scary new world |
| 355 | `tribal_development` |  | &lt;province id&gt;, &lt;value&gt; |  |  | state | adds tribal development to the federation |
| 356 | `set_base_tax` |  | &lt;province id&gt;, &lt;value&gt; |  |  | state | Sets base tax in province to provided value |
| 357 | `set_base_production` |  | &lt;province id&gt;, &lt;value&gt; |  |  | state | Sets base production in province to provided value |
| 358 | `set_base_manpower` |  | &lt;province id&gt;, &lt;value&gt; |  |  | state | Sets base manpower in province to provided value |
| 359 | `flagship_heavy` |  |  |  |  | state | Build a heavy flagship |
| 360 | `flagship_light` |  |  |  |  | state | Build a light flagship |
| 361 | `flagship_galley` |  |  |  |  | state | Build a galley flagship |
| 362 | `enable_game_controller` |  |  |  |  | client | Enables/disables support for game controller |
| 363 | `highlight_islands` |  | &lt;threshold&gt; |  |  | client | Highlight islands on the map |
| 364 | `landing_page` |  | &lt;dlc_key&gt; |  |  | client | Show Red Queen dlc landing page |
| 365 | `add_to_curia_treasury` |  | &lt;money&gt; |  |  | state | Add money to the Curia Treasury |
| 366 | `pick_golden_bull` |  | &lt;name&gt; |  |  | state | Pick a golden bull |
| 367 | `estate_agenda` |  | &lt;name&gt; |  |  | state | Start a estate agenda |
| 368 | `imperial_incident` |  | &lt;name&gt; |  |  | state | Start an imperial incident |
| 369 | `spread_revolution` |  | &lt;province ID or tag&gt; |  |  | state | Spread the Revolution |
| 370 | `zeal` |  | &lt;Amount&gt; [&lt;Target Country tag&gt;] |  |  | state | Adds Revolutionary Zeal to a country |
| 371 | `crownland` |  | &lt;Amount&gt; [&lt;Target Country tag&gt;] |  |  | state | Adds Crownland to a country |
| 372 | `print_estate_land_share` |  | &lt;estate_type&gt; [ &lt;\|&gt; percent] |  |  | read | Prints estates (or crown) land shares |
| 373 | `debug_crownland` |  | &lt;tag&gt; |  |  | file | Will log the change in crownland for a tag |
| 374 | `debug_budgettag` |  | &lt;tag&gt; |  |  | file | Will log income and expenditure for a tag |
| 375 | `debug_budgetall` |  | &lt;tag&gt; |  |  | file | Will log income and expenditure for all countries. This will result in large files and a slower game! |
| 376 | `reform_desire` |  | [&lt;value&gt;] |  |  | state | Will add reform desire (don't you dare) |
| 377 | `federation_cohesion` |  | [&lt;value&gt;] |  |  | state | Will change federation cohesion by given value |
