# Research: how the game's province links select a province, open its view and move the camera

Ticket: happy-dwarf-team/tongs#96 (child of map #94). Static analysis only; the
game was not started.

Source: `eu4.exe` 1.37.5 (Steam, win64), SHA-256
`9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A`, checked on
the analysed copy before and after the work. Ghidra 12.1.4 headless with
pyghidra, on a private copy of the lab's analysed project (no shared project
lock was taken). All addresses are RVAs relative to the image base
`0x140000000`; offsets are hex.

How each claim was checked:

- **[G]** Ghidra decompiler output of the named function.
- **[D]** Ghidra listing (instruction bytes and operands) at the address.
- **[F]** bytes read from the file (vtable slots, constants).
- **[X]** Ghidra references to an address (callers, data references).
- **[S]** a string and the instruction that references it.
- **[R]** an earlier research note: Tongs `research/console-commands`
  (#17), `research/main-thread-dispatch` (#16), `research/widget-tree` (#18),
  or Tongs `docs/design.md`.

"Verified" below means read in the code; it says nothing about behaviour at
run time. Names such as "selection" or "camera" are ours; the binary has no
RTTI for these classes.

## Question

Which game function do the game's own province links (alerts, outliner,
ledger, event and tooltip links) call to select a province, open its province
view and move the camera to it, and can Tongs call it from its main-thread
dispatcher? What does it need, what does it do with an invalid or undiscovered
province, does it change anything beyond the client, where do the camera's
position and zoom live, is there a "go to province" without opening the view,
and what does a map click do differently?

## Short answer

There is no single function. Every link handler read here combines up to two
calls:

1. **Camera: `0x80FE20(session, province*)`**, session vtable slot `0x280`,
   or its ID wrapper **`0x80FD30(session, id)`**, slot `0x278`. It sets the
   camera's target over the province and keeps the current height (zoom).
   It does nothing else: it does not select the province, open a view or post
   a command. This is the "go to province" without a view.
2. **Select and open the view: `0x139C830(selection, province*)`**, where
   `selection` is the object at `session+0x1220` (slot `0xF8`). It stores the
   selected province, hands it to the province view (`0x1347A50(view,
   province, 1)`), which fills and shows the view. Inside the view code there
   is **one path that posts a command into the game's command stream**: when
   the owner is another country whose capital the player has not discovered,
   it sends a command whose execute step calls `CProvince` discover code
   (`0xA0A1D0`, `province.cpp`). So opening a province view is not always
   client-only.

Links differ in which of the two they call. The religion view's province
rows, the state view's rows and holy-site "goto" buttons call (1) then (2).
The event window's `event_goto_province` and the find-province box call
`0x6EBC90(session, id)`: (1) by ID plus a map highlight, with no view. Many
alert clicks only move the camera, cycling through their provinces. A left
click on the map calls (2) without (1).

Tongs can call both from its dispatcher after an outer frame. The game itself
runs the same handlers from its event loop, which runs in outer frames and
also in nested frames inside game updates
(`research/main-thread-dispatch`). After an outer frame neither a game update
nor GUI processing is on the stack. The camera call is a client call. The
view call needs the game's own guards replicated (valid ID, terra incognita,
session mode). It must also either refuse the discover case or count it as a
game call.

## Verified statically

### Where the objects live

- **Game root** `[0x233FE78]` (`symbols.toml` `root_and_date`), **session**
  at `root+0x1E00` (`session_of_root`) [R].
- **Provinces:** the province with ID `n` is
  `[root+0x1CA8] + n * 0x2E10`. Valid IDs are `0 < n < count` with
  `count = ([root+0x1C98] - [root+0x1C90]) >> 3`. The check and the indexing
  appear identically in `0x80FD30`, `0xFF6350` and `0x12BAF00` [D]:
  `0x80FD39 7E 2C` (`id <= 0` returns), `0x80FD3B`–`0x80FD4F` (count),
  `0x80FD54 48 69 D0 10 2E 00 00` (`imul 0x2E10`),
  `0x80FD5B 49 03 90 A8 1C 00 00` (`+[root+0x1CA8]`).
- **A province's own ID is the int at `province+0x20`.** `0x12BAF00` reads it
  from its province, range-checks it, indexes the array with it and passes
  the result to the camera call (`0x12BAF2F`–`0x12BAF5E`). `0x80FE20` refuses
  `province+0x20 <= 0` (`0x80FE24 83 7A 20 00`, `0x80FE28 0F 8E`) [D].
- **Province map data:** `province+0x28` points to a record with the map
  position as two int16 at `+0x7C` (x) and `+0x7E` (y). `0x80FE20` loads the
  dword at `+0x7C` and splits it with `movsx`/`shr 16` (`0x80FE2E`–`0x80FE4D`)
  [D]. Byte `+0x229` bit `0x08` sends `0x139C830` to its "close the view"
  branch. `0x9F3050` deselects instead of selecting when `+0x228` bit `0x08`
  is clear or `+0x229` bit `0x08` is set [G].
- **Session vtable `0x1CB8460`** (the in-game session class, [R]
  `session_speed_slot`), slots read from the file [F]:

  | Slot | Entry | What it is (from the entry's code) |
  | --- | --- | --- |
  | `0x080` | `0x811AA0` | `[[session+0x330]+0x370]`: the command manager |
  | `0x0F8` | `0x8025A0` | `session+0x1220`: the selection object |
  | `0x1A0` | `0x5C44E0` | `session+0x1200`: the camera |
  | `0x258` | `0x818080` | deselect units, close the unit and side panels |
  | `0x278` | `0x80FD30` | camera to province by ID |
  | `0x280` | `0x80FE20` | camera to province object |
  | `0x288` | `0x80FD70` | camera to map coordinates `(x, y)` (ints) |
  | `0x290` | `0x80FF90` | select a unit and move the camera to its location (`unit+0xE8`) |

  `0x1A0` and `0x198` share the entry `0x5C44E0`. `debug_lockcamera`
  (`0xE85A90`) uses slot `0x1A0` and toggles byte `+0x758` of the result [G].
- **Selection object** (`session+0x1220`): 32 bytes allocated in the session
  constructor `0x803270` (`0x8038B7 B9 20 00 00 00`, `0x8038E0 48 89 86 20 12
  00 00`) [D]:
  - `+0x00`: the selected province, initially 0;
  - `+0x08`: the session;
  - `+0x10`: the province view;
  - `+0x18`: a second, small province view.

  `0x139BDF0(selection, flag)`, called from the in-game idler `0x81AF10`,
  creates the province view (`0x26F0` bytes, constructor `0x1343200`, string
  `provinceview`) into `+0x10`. It also handles `+0x18`, the view built from
  `interface/small_provinceview.gui` (`0x143C550`) [G][S].
- **The province view keeps its province at `view+0x958`** and a pointer back
  to the selection at `view+0x26D8`. `0x1347A50` writes `param_1[299]`
  (`+0x958`) [G]. `0x1346036 48 8B 93 58 09 00 00` / `0x1346040 48 8B 8B D8
  26 00 00` pass both to `0x139C830` [D].
- **Camera** (`session+0x1200`): `0x7D0` bytes, constructor `0x109B830` (it
  registers the input actions `pan_camera` and `zoom_camera` [S]), stored at
  `0x80A17F 48 89 86 00 12 00 00` in the session constructor [D]. Fields used
  below [G][D]:
  - `+0x67C`, `+0x680`, `+0x684`: current position (x, height, z);
  - `+0x688`, `+0x68C`, `+0x690`: current look-at;
  - `+0x694`, `+0x698`, `+0x69C`: target position;
  - `+0x6A0`, `+0x6A4`, `+0x6A8`: target look-at;
  - `+0x758`: lock byte (`debug_lockcamera`);
  - `+0x778`: horizontal wrap flag.

### Camera: `0x80FE20` and `0x80FD30`

- `0x80FE20(session, province)` [G][D]:
  1. returns if `province+0x20 <= 0`;
  2. takes x, y from `[province+0x28]+0x7C`;
  3. when `camera+0x778` is set, shifts x by the map width
     (`[0x233FED8]+0x28`) to the copy nearest the current x
     (`camera+0x67C`);
  4. calls `0x109D140(camera, {x, camera+0x698, y})`, so the height (zoom) is
     the current target height.
- `0x109D140(camera, pos)` returns at once when the lock byte `+0x758` is set
  (`0x109D140 80 B9 58 07 00 00 00`, `75 6B`). Otherwise it writes the target
  position `+0x694..+0x69C` and the target look-at `+0x6A0..+0x6A8` (look-at
  height 0, z moved back by the setting `[0x242BDE8]+0x27C` times a constant)
  and calls `0x109D0E0`, which clamps both through `0x109CF60` [D]. It does
  not touch the current position. `0x109CD80` is the routine that copies
  target to current.
- `0x80FD30(session, id)` is the same check and indexing as above, then a
  tail jump to `0x80FE20` (`0x80FD62 E9 B9 00 00 00`) [D].
- Nothing in `0x80FE20`, `0x80FD30`, `0x109D140` or `0x109D0E0` writes
  outside the camera object or calls the command manager [G].

### Select and open: `0x139C830(selection, province)`

In order [G][D]:

1. Returns at once when `session+0x1440` is set and `0x140D100` is true
   (`0x139C840`–`0x139C853`). `0x140D100` looks for type `0xE` in the list
   of the object behind `session+0x1440`; the object is the tutorial (our
   reading of `0x140D100`, not proven).
2. Stores the province in `selection+0x00`, **or 0 when the session mode
   `session+0x2C` is `0x25`** (`0x139C860 41 83 79 2C 25`, `cmovnz`).
3. Calls `0x1474AC0(session+0x5BA0)` (closes or refreshes a panel through
   its slot `0x290`).
4. Returns when the stored pointer is 0 or `province+0x20 == 0`.
5. Siege view (`[session+0x11B8]`): when the province has a siege
   (`province+0x388`) and the siege view is not open, it may open it
   (`0x13ACAE0`); when the siege view is open, it closes it.
6. When `[province+0x28]+0x229` bit `0x08` is set: hides the province view
   (its slot `0x28`) and the panel at `session+0x1228`, and returns.
7. Otherwise, with no small view (`selection+0x18 == 0`), calls
   **`0x1347A50(view, province, 1)`**. With a small view, it either refreshes
   the small view (`0x143D550`) or, when the big view is shown, calls
   `0x1347A50` and then **`0x139C6A0(camera, [province+0x28]+0x7C, view
   window, …)`**. That routine (`0x109D540`) places the camera target so the
   province lands in the screen area beside the view window. It does **not**
   check the lock byte.

`0x1347A50(view, province, show)` [G], already exported in the lab's
`work/ghidra/decomp-provinceview`, re-read here:

- When the province changes, it rebuilds the strait and river indicators,
  stores the province at `+0x958`, refreshes the sub-panels
  (`0x1376B40`, `0x136F000`, `0x1390280`, `0x0E31930`) and terrain text.
- With `show` set, a foreign province (owner's index at
  `(int16)(province+0x468 >> 32)` differs from the player's) closes one
  sub-panel. Then:
  - no owner (byte `province+0x46F` is 0) and bit `0x10` of `province+0x958`
    set: returns;
  - an owner and (`province+0x3F8 < 1` or `province+0x958` bit `0x10`): the
    "other" layout (slot `0x288` on `view+0xA0`, the view's slots `0x48` and
    `0x50`, and slot `0x290` with 1 on `[view+0x2F0]+0x558`). Then **the
    discover command** (next section);
  - otherwise: slot `0x298` with 1 on `view+0xA0` (shows the window when the
    sub-window `[view+0x2F0]+0x558` is not shown) and `0x0E37BD0`.

### The command that opening a view can post

At `0x1348290`–`0x13482E1` [D], still inside `0x1347A50`, after the
returns above. Conditions [G]:

- the owner is not the player;
- the owner's capital province is `country+0x14B0`, where the countries are
  `[[0x233D8D0]+0x118][index]`;
- its byte `+0x959` has bit `0x10` set;
- the player tag is valid and not type 7;
- the capital's discovered-by bit for the player's country index is clear.
  The bits are a bitset at `[capital+0x5E0]`, `index >> 5` dwords, bit
  `index & 31`, the same test as in `0xFF6350` below.

With all of them true, the code:

1. gets the command manager through session slot `0x80` (`0x811AA0`:
   `[[session+0x330]+0x370]`);
2. allocates `0x60` bytes and fills a command: vtable `0x1C55958`
   (`0x13482C9 48 8D 0D 88 D6 90 00`), `+0x08` = `0x165`, type id
   `+0x44` = `0x2C8E` (`0x13482B9 41 C7 41 44 8E 2C 00 00`),
   `+0x50` = the player tag (`root+0x1E60`, or `root+0x1E68` in observer
   mode), `+0x58` = the owner's capital province ID (`country+0x14B8`);
3. calls **`0x14CA2D0(manager, command)`**.

- `0x14CA2D0` is the command post [G]. Refusals: the manager's sink
  `+0x58` is null; or none of `manager+0x55`, `manager+0x80D`, the command's
  slot `0x80` and its slot `0x58` is set or true. Otherwise it stamps
  `command+0x38` with `manager+0x8C` and `command+0x48` with `min(manager+0x68,
  0x7FFF)` and hands the command to the sink's slot `0x30`.
- `manager+0x54`/`+0x55` are the bytes the multiplayer test
  `0x6EAA10` reads [R console-commands].
- The same post is used by army-group keys in the map input handler
  `0x116D6C0` (command vtable `0x1D1C390`, type id `0x2830`) [G].
- Vtable `0x1C55958` [F]: slot `0x20` `0x2A3CF0`, slot `0x48` `0x2A5000`,
  slots `0x58`/`0x78`/`0x80` `0x8F450` (`xor al,al; ret`) [D].
  Its default constructor `0x2A4F90` sets the same `0x165` / `0x2C8E` [G].
- **Slot `0x48`, `0x2A5000`**, calls
  `0xA0A1D0(provinces + id*0x2E10, tag, root+0x1DD0 (the date), 1, 1, 1, 0)`
  on the capital ID at `+0x58` and the tag at `+0x50` [G]. `0xA0A1D0` belongs
  to `eu4/source/province.cpp`. Its strings are `Discovery prestige gain: %s,
  %f, %d`, `PRESTIGE`, `DISCOVER` and `Invalid tag trying to discover
  province` [S].

So this command makes the player's country discover the owner's capital
province, and may give prestige. It goes through the same command manager as
every other player action, which in multiplayer is the synchronised command
stream.

### Who calls what (the link handlers)

All [G], call sites [D] or [X]; "camera" = `0x80FE20`/`0x80FD30`,
"select" = `0x139C830`.

| Handler | UI (from its strings) | Calls |
| --- | --- | --- |
| `0xFF6350` (vtable `0x1EEFC10`, created by `0xFE4330`) | religion view province rows (`countryreligionview`) | terra incognita check, close views (`0x8208A0`/`0x8205E0`), camera if `0 < id < count`, select |
| `0x133C700` (vtable `0x1F31CD0`, `0x133BE40`) | state view province rows (`state_province_entry`) | refresh a province-view sub-panel, `0x810010` (select the province's units, camera), select |
| `0xE16C10` (`0xE15BE0`) | holy site row `goto_button` (`site_icon`, `GFX_holy_site_`) | close the panel, camera, select |
| `0x12BAF00` (`0x12BA790`) | a province button; skipped while the panel at `session+0x12F0` is shown | camera if `0 < id < count`, `0x82AEE0` (close views and panels), slot `0x258`, select |
| `0x1488350` (vtable `0x1D88900`) | unit panel (`unitpanel`) | select the unit's location (`unit+0xE8`), then siege view `0x139C130` |
| `0x10C0210` (vtable `0x1EFF8B4`, `0x10B9A50`) | event window `event_goto_province` (event `+0x854`, resolved by `0x6EFFD0`) | `0x6EBC90(session, id)` |
| `0x1143420` | find-province box (`goto_edit`, `provinces`; the type `CGotoBoxSearchList` is in RTTI) | `0x6EBC90(session, id)` |
| `0xE01070` (from `0xDF20F0`, `alertmanager.cpp`, `alert_*` names) | alert icons | per alert kind: camera only, cycling an index through the alert's list (e.g. `+0x59F8`, `+0x5248`, `+0x5260`); or select units and camera to their location; or a country capital (`country+0x14B0`); or open a view index with `0x820990` |
| `0x12BC5E0` | capital / country button | camera to the capital through slot `0x280`, then `0x821420` (country view) |
| `0x10B4720` (vtable `0x1EFE7E8`) | country shield click | camera to the capital (slot `0x280`) when `root+0x7484` is 0, otherwise `0x821420` |

- `0x6EBC90(session, id)` [D]: slot `0x278` (camera by ID), then
  `[[session+0x330]+0x360]+0x28 = id`. With `session+0x30D` set it also calls
  `0xE1B030` twice and ORs 2 into a per-province byte array at
  `[session+0xC0]`. It opens no view.
- `0x821420(session, tag)` opens the **country** view, not the province
  view. The argument is an 8-byte country tag: the handlers pass
  `province+0x468`, the owner tag, and `0x10B4720` matches tags with
  `strncmp(…, "REB", 3)`. It runs `0xF15C30(view 4, 0)` (strings
  `select_rivals_window`, `countryview_tabs`), `0x820990(session, 4)` and
  `0xF01870(view 4, tag)` [G]. `0x820990(session, n)` shows view `n` of the
  vector `session+0x1110`, and the call sites pass `0`–`13` [D]. The
  province view is not in that vector.
- The outliner and the ledger were not traced. `interface/outliner.gui` is
  loaded by the session constructor `0x803270` [S]. Their row handlers are
  likely among the 57 callers of `0x80FE20` and the 11 callers of `0x139C830`
  that were not identified [X].

### Terra incognita and invalid IDs

- `0x80FD30` and the handlers check `0 < id < count` themselves; `0x80FE20`
  checks only `province+0x20 > 0`. `0x139C830` and `0x1347A50` do no range
  check, so a caller must pass a valid province pointer [D][G].
- **The terra incognita check is the caller's.** `0xFF6350` acts only when
  (`0xFF63B8`–`0xFF63E3`) [D]:
  - bit `0x10` of `province+0x959` is clear; or
  - the player tag is invalid (byte 7 is 0) or of type 7 (observer); or
  - the player country's bit is set in `[province+0x5E0]`.

  Otherwise it returns without camera or view. `0x80FE20`, `0x139C830` and
  `0x1347A50` contain no such check for the province itself; the only one in
  `0x1347A50` is about the owner's capital (above).
- The player tag is `root+0x1E60`, or `root+0x1E68` when byte `root+0x1E66`
  is 7 and `root+0x1E6F` is set (`0xFF6384`–`0xFF639F`) [D]. Its int16 at
  bits 32–47 is the country index used for the bitsets.

### Map click versus link

- **Left click on a province on the map** reaches `0x9F3050`. It is the
  slot of a province's selectable sub-object at `province+0x10`, and
  `param_1 - 0x10` is the province. Only data references point to it; its
  dispatcher was not traced [X]. It runs [G]:
  1. in session modes `0x30`/`0x33`, a unit-list path instead of selection;
  2. no land bit, or the `+0x229` bit set: slot `0x258` (deselect);
  3. the siege view;
  4. `0x139C070(selection, province)` unless the mode is `0x25`.

  `0x139C070` closes the panel at `session+0x1228`. In observer mode
  (`root+0x1E66 == 7`) with a key held (input slot `0x30`, `0x400000E0`) or
  `session+0x5BA8` set, it calls `0x763A20(root, province+0x468)`, which
  switches the observed country (`root+0x1E68`). Otherwise it calls
  `0x139C830`. **There is no camera call**; only the side-panel offset inside
  `0x139C830` (small-view layout) moves it.
- **`0x116D6C0` is the map input handler** (the console research's
  "map-click command" lead). For a mouse event (type 4) it maps the cursor to
  a province through the province bitmap (`0x10A3DD0`, an int16 grid at
  `[map+0x30]+0x10`, width `+0x28`), then by button:
  - button 1 with a modifier: either the observer country switch
    (`0x763A20`) or the developer `mapclick` command. That one appends the
    province or the owner tag to the pending command (manager `+0x20`/`+0x40`)
    and calls the console dispatcher `0x1721320`;
  - button 2 with no unit selected and no widget under the cursor
    (`[0x23494F0]+0x130 == 0`): `0x821420(session, province+0x468)`, the
    owner's country view.

  Digit keys post army-group commands through `0x14CA2D0` [G]. None of this
  is the province-view path.

## Hypotheses (need a check in the game)

- **H1 – smooth pan.** `0x109D140` sets only the target, so the camera glides
  to the province over the next frames; `0x109CD80` (target → current) is the
  snap. The per-frame update that moves current toward target was not
  identified.
- **H2 – camera call is client-only.** No writes outside the camera were seen
  in `0x80FE20`/`0x109D140`/`0x109D0E0`, but `0x109CF60` (clamp) was not read.
- **H3 – terra incognita bits.** `province+0x959` bit `0x10` means "can be
  undiscovered" and `[province+0x5E0]` is the discovered-by bitset, indexed by
  country index. Inferred from the shape of the tests in `0xFF6350` and
  `0x1347A50`.
- **H4 – the discover command fires in a normal game.** It is posted when a
  foreign province is shown whose owner's capital the player has not
  discovered. In single player, `manager+0x55` or `+0x80D` must be set for
  the post to pass `0x14CA2D0`, because the command's own slots `0x58`/`0x80`
  return 0. Army groups work in single player, and they post a command with
  the same slots, so one of the bytes is presumably set while a game runs.
- **H5 – `province+0x3F8 < 1` / `province+0x958` bit `0x10`** select the
  "other country" layout of the view; their meaning is unknown.
- **H6 – `[[session+0x330]+0x360]+0x28`** is the map's highlighted province
  (−1 = none): `0x6EBC90` writes the ID, `0x116D6C0` writes −1.
- **H7 – the small view** (`selection+0x18`) belongs to a layout that is not
  the default, so on the default interface `0x139C830` calls `0x1347A50`
  directly and does not offset the camera.
- **H8 – session modes.** `session+0x2C == 0x25` is a province-picking mode
  in which selection is suppressed; `0x27`, `0x30`, `0x33` are other modes
  (`0x1229A10` skips selection in `0x27`). Names unknown.
- **H9 – `session+0x1440`** is the tutorial, and selection is blocked while
  a tutorial step of type `0xE` is active.

## Can Tongs call it from its dispatcher?

Statically, yes, with conditions:

- **Where the game calls it.** All handlers above run as GUI callbacks from
  the event loop `0x1569C70`. The frame function calls the event loop in
  outer frames and also in nested frames, i.e. while a game update or a save
  load is on the stack ([R] main-thread-dispatch). Tongs' batch runs after an
  outer frame returned: no game update is on the stack, GUI processing and
  rendering for the frame are done, and a pending state switch was applied.
  That is a calmer point than the one the game itself uses for clicks during
  an update.
- **Preconditions Tongs must check, each call:**
  - `in_game`: root and `root+0x1E00` non-null, and the session vtable is
    `0x1CB8460` (in the menu and while loading the session has another
    class, [R] `session_speed_slot`);
  - `0 < id < count`;
  - for the view: `selection` (`session+0x1220`) and its view `+0x10`
    non-null;
  - pointers are read fresh each call and never kept across frames.
- **Camera (slot `0x278`)** is a candidate client call: one call with an
  int, a self-contained range check, no command. It honours
  `debug_lockcamera`.
- **View (`0x139C830`)** needs more care:
  - replicate the link handlers' terra incognita check (`0xFF6350`), or
    decide deliberately to bypass it (observer-like);
  - expect nothing to happen in mode `0x25` or with the tutorial guard;
  - either refuse the discover case (pre-check the same capital conditions
    and answer an error), or treat `province open` as a game call when it
    would post (count it in `game_calls`, require `game_calls = true`, ADR
    0001), or accept it and mark the run.
- **Mirror the game's sequence**, as `0xFF6350` and `0x12BAF00` do: close
  views (`0x8208A0`/`0x8205E0`, or `0x82AEE0`), camera, select. Calling only
  `0x139C830` gives what a map click gives (no camera jump).

## Candidate symbols

`plugin/symbols.toml` now declares `[symbol.<name>]` entries (`rva`, `bytes`,
`memory`, `values`, `points_to`, `signature`, `note`) rather than the older
`[[expect]]` list; the candidates follow that style. Bytes are from [D]/[F].

```toml
[symbol.session_camera_to_province_id_slot]
rva = 0x1CB86D8
memory = "read"
points_to = 0x80FD30
note = "session vtable slot 0x278: camera to province by ID"

[symbol.camera_to_province_id]
rva = 0x80FD30
bytes = "4C 8B 05 41 01 B3 01 85 D2 7E 2C 49 8B 80 98 1C 00 00 49 2B 80 90 1C 00 00 48 C1 F8 03 3B D0 7D 16 48 63 C2 48 69 D0 10 2E 00 00 49 03 90 A8 1C 00 00 E9 B9 00 00 00"
memory = "code"
signature = "void (void* session, int province)"
values.root = { at = 3, rip_relative = 7 }
values.province_ptrs_end = { at = 14, size = 4 }
values.province_ptrs_begin = { at = 21, size = 4 }
values.province_size = { at = 39, size = 4 }
values.provinces = { at = 46, size = 4 }
note = "0 < id < (root[+0x1C98]-root[+0x1C90])/8, province = root[+0x1CA8] + id*0x2E10, tail jump to camera_to_province"

[symbol.camera_to_province]
rva = 0x80FE20
bytes = "48 83 EC 38 83 7A 20 00 0F 8E A9 00 00 00 48 8B 42 28 4C 8B 89 00 12 00 00 8B 50 7C"
memory = "code"
signature = "void (void* session, void* province)"
values.province_id = { at = 6, size = 1 }
values.map_data = { at = 17, size = 1 }
values.camera = { at = 21, size = 4 }
values.map_position = { at = 27, size = 1 }
note = "session slot 0x280: camera target over [province+0x28]+0x7C (int16 x, y), height kept"

[symbol.camera_set_target]
rva = 0x109D140
bytes = "80 B9 58 07 00 00 00 75 6B 8B 02 89 81 94 06 00 00 8B 42 04 89 81 98 06 00 00"
memory = "code"
values.lock = { at = 2, size = 4 }
values.target_x = { at = 13, size = 4 }
values.target_height = { at = 22, size = 4 }
note = "camera: returns when the debug_lockcamera byte is set, else writes the target position and look-at"

[symbol.session_selection_slot]
rva = 0x1CB8558
memory = "read"
points_to = 0x8025A0
note = "session vtable slot 0xF8: the province selection object"

[symbol.selection_of_session]
rva = 0x8025A0
bytes = "48 8B 81 20 12 00 00 C3"
memory = "code"
values.selection = { at = 3, size = 4 }
note = "selection at session+0x1220: {province*, session*, province view*, small view*}"

[symbol.camera_of_session]
rva = 0x5C44E0
bytes = "48 8B 81 00 12 00 00 C3"
memory = "code"
values.camera = { at = 3, size = 4 }
note = "session slots 0x198/0x1A0: the camera at session+0x1200"

[symbol.select_province]
rva = 0x139C830
bytes = "40 53 48 83 EC 20 4C 8B 49 08 48 8B D9 4C 8B D2 49 8B 89 40 14 00 00 48 85 C9 74 0D E8 AF 08 07 00 84 C0 0F 85 C8 02 00 00 48 89 6C 24 30 33 ED 41 83 79 2C 25"
memory = "code"
signature = "void (void* selection, void* province)"
values.tutorial = { at = 19, size = 4 }
values.session_mode = { at = 51, size = 1 }
note = "select a province and show it in the province view; nothing in session mode 0x25 or under the tutorial guard"

[symbol.province_view_province]
rva = 0x1346036
bytes = "48 8B 93 58 09 00 00 0F B6 E8 48 8B 8B D8 26 00 00 E8 E4 67 05 00"
memory = "code"
values.province = { at = 3, size = 4 }
values.selection = { at = 13, size = 4 }
note = "province view: its province at +0x958, the selection at +0x26D8"

[symbol.province_view_discover_command]
rva = 0x13482B9
bytes = "41 C7 41 44 8E 2C 00 00 49 C7 41 48 FF FF 00 00 48 8D 0D 88 D6 90 00 49 89 09 49 89 41 50 41 89 51 58 49 8B D1 48 8B CB E8 EA 1F 18 00"
memory = "code"
values.command_type = { at = 4, size = 4 }
note = "province view: posts the discover-capital command (type 0x2C8E) through the command manager; a build that moves it changes what province open may do"

[symbol.terra_incognita_check]
rva = 0xFF63B8
bytes = "F6 85 59 09 00 00 10 74 24 84 D2 74 20 80 F9 07 74 1B 0F BF C8 48 8B 85 E0 05 00 00"
memory = "code"
values.ti_flags = { at = 2, size = 4 }
values.ti_bit = { at = 6, size = 1 }
values.discovered_by = { at = 24, size = 4 }
note = "religion view row: a province link acts only if province+0x959 bit 0x10 is clear, the player is an observer, or its bit in [province+0x5E0] is set"

[symbol.goto_province_highlight]
rva = 0x6EBC90
bytes = "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 48 8B D9 48 63 FA 8B D7 FF 90 78 02 00 00 48 8B 83 30 03 00 00 48 81 C3 B8 00 00 00 4C 8B 80 60 03 00 00 41 89 78 28"
memory = "code"
signature = "void (void* session, int province)"
note = "event window goto and find-province box: camera by ID (slot 0x278), then [[session+0x330]+0x360]+0x28 = id"
```

The player-tag selection (`root+0x1E60` / `+0x1E68` with `+0x1E66 == 7`,
`+0x1E6F`) is needed by the terra incognita check and by `selected` reads.
It can be declared from `0xFF6384`: `80 BA 66 1E 00 00 07 75 12 80 BA 6F 1E
00 00 00 74 09 48 8B 82 68 1E 00 00 EB 07 48 8B 82 60 1E 00 00`.

## Checks in the game (a separate task ticket)

Under the lab's game lock, with a Tongs build that exposes the calls as
main-thread jobs:

1. Camera by ID (slot `0x278`) on an owned, a foreign and an undiscovered
   province:
   - read camera `+0x67C..+0x6A8` over the next frames to confirm the glide
     (H1) and that the height `+0x680`/`+0x698` is kept;
   - repeat with `debug_lockcamera` on;
   - repeat near the map's date line, for the wrap at `+0x778`.
2. `0x139C830` on an own province with the view closed and open:
   - `selection+0x00` and `view+0x958` hold the province;
   - the view is shown;
   - whether the camera moves (H7).
3. `0x139C830` on a foreign province whose owner's capital is undiscovered:
   - the capital becomes discovered (H4);
   - the command manager's post count or any prestige change;
   - repeat with the capital already discovered: nothing is posted.
4. The game's own links on the same provinces: the religion view row, the
   event `event_goto_province` and the find-province box, to compare their
   effects with the direct calls.
5. A left click on the map, against `0x139C830` alone (expect the same, no
   camera move).
6. `selected` read: `selection+0x00` after a map click, after closing the
   view, and after selecting an army (does it clear?).
7. Session mode `+0x2C` while choosing a province in a diplomatic action or
   peace deal (H8), and with the tutorial running (H9).

## New questions

- Is `province open` a client call or a game call? Opening the view can post
  the discover-capital command. The options: refuse that case, mark it as a
  game call, or accept it as "what the player's own click would do". This
  belongs in ADR 0003 (client calls).
- Should Tongs honour terra incognita like the game's links do, or allow
  opening undiscovered provinces? Allowing them reveals information the
  player does not have, but changes no state.
- Should `province goto` also set the map highlight, as the event and
  find-province links do (`0x6EBC90`), or only move the camera
  (slot `0x278`)?
- `0x821420(session, tag)` opens the country view by tag, and `0x820990`
  switches among 14 session views. This covers part of "Other views by ID"
  under "Not yet specified" in #94.
- The outliner and ledger row handlers were not identified; if the spec
  promises "like the outliner", trace them.
