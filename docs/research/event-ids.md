# Research: where the event window keeps the event id and option keys

Ticket: happy-dwarf-team/tongs#29 (child of #15). Static analysis only; the
game was not started.

Source: `eu4.exe` 1.37.5 (Steam, win64), SHA-256
`9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A`, checked
on the lab's copy before and after the work. Tools: Ghidra 12.1.4 headless on
a private copy of the lab project (decompiler; functions that auto-analysis
had missed were created in the copy from their `.pdata` start), `dumpbin
/disasm` of the whole `.text` section for call sites, operand searches and
instruction bytes, raw reads of the PE file for strings, vtables and the
script token table, and a read-only scan of the install's `events/*.txt`.
All addresses are RVAs relative to the image base; offsets are hex. Builds on
`docs/research/widget-tree.md` (branch `research/widget-tree`), whose names
(GuiObject, `windowType`, registry) are used here.

"Event controller" below is our name for the 1000-byte object built by the
constructor in `eventwindow.cpp`; the binary has no RTTI for it.

## Question

Where does the event window keep the event it shows (its script id, e.g.
`flavor_xxx.1`) and the script keys of its options (`name = ...`), and can
Tongs read them as a main-thread read? How does an `event_option_entry` map
back to its option, and how are several open events held?

## Answer in short

- One event controller per open event window. The session
  (`*(root + 0x1E00)`) keeps every open pop-up, event controllers included, in
  a linked list at `session + 0x78`, and every filled event window once more
  as an `{event, scope, window}` triple in a second list at
  `session + 0x498`.
- Controller: window `+0x38`, event `+0x48`, scope copy `+0x50`, pending-event
  id `+0x120`, "still waiting for a choice" byte `+0x124`, list of option
  buttons `+0x208`.
- Event: script id text as written in the file, `std::string` at
  `event + 0x20`; numeric id `int32` at `event + 0x14`; options
  `std::vector<option*>` at `event + 0x790`, in file order.
- Option: `name` key, `std::string` at `option + 0x88`.
- Option button → option: the button's GuiObject string at `+0x38` holds
  `"<numeric event id>_<option index>"`; the click handler parses exactly this
  string. The index counts every option of the event, hidden ones included.
  The button also stores a pointer to the controller's scope at `+0x10`.
- Option names are not unique keys: in the base game 26 events repeat an
  option `name`, and 64 options have none. `(event id, option index)` is the
  key; the name is a label.

## Verified statically

"Verified" means read in the disassembly or decompiler output of the named
function; it says nothing about behaviour at run time.

### Who creates the controller

- Session vtable `0x1CB8460` (the session's class, see `docs/design.md`)
  slot `0x108` is `0x828C00` (vtable entry at `0x1CB8568`, found by a raw
  search for the pointer value). It does nothing unless the byte
  `root + 0x74E0` is set; otherwise it allocates `0x3E8` bytes, calls the
  constructor `0x10B9A50(mem, session, event, scope, int)` and hands the
  result to `0x6ECAE0(session, controller)` (call sites `0x828C4F`,
  `0x828C5B`).
- The only other caller of the constructor is the console command `event`
  (handler `0xE5BB30`, record 29 of the command table, help text "Executes an
  event"). At `0xE5C15B`–`0xE5C1C0` it runs the event's immediate block,
  registers a pending event with `0x756F00(root, event, scope)` and passes the
  returned id as the last constructor argument, then calls `0x6ECAE0` the same
  way. Found by a raw search for `call` targets over `.text`; there are no
  other direct calls.

### Session lists

- `0x6ECAE0(session, popup)` appends the pop-up to a doubly linked list:
  head `session + 0x78`, tail `+0x80`, count `int32 +0x88`; 0x20-byte nodes
  `{popup*, prev, next at +0x10, byte at +0x18}`. First it asks the new pop-up
  `vfunc[0x08]`; if that is true and an existing pop-up also says true, the
  new one is deleted instead. The event controller's slot `0x08` is
  `0x8F450` (returns 0), so event controllers are always appended. Many other
  pop-up kinds use the same function (217 direct calls in `.text`), so the list is
  not only events.
- `0x826C20` walks `session + 0x78` calling each pop-up's `vfunc[0x28]`
  (for the controller `0x10B9A20`: clear `+0x124`), then frees the nodes of
  `session + 0x498`: a "close all pop-ups" path.
- At the end of the fill function (`0x10BBAE8`–`0x10BBB1F`, tail jump through
  `[vtable + 0x2F0]`) the controller calls session slot `0x2F0` =
  `0x82B020(session, event, controller + 0x50, window)`. It allocates a
  0x18-byte entry `{event*, scope*, window*}` and appends it to a list with
  head `session + 0x498`, tail `+0x4A0`, count `int32 +0x4A8` (same node
  layout). The destructor `0x10BA030` calls slot `0x2F8` = `0x82B0C0(session,
  event, window)`, which finds the entry by event and window and unlinks it,
  or only sets the node byte `+0x18` when the byte `session + 0x4AC` is set
  (deferred removal while the list is being walked).
- The constructor also calls `session->vfunc[0x100]()` (the game-speed object
  at `session + 0x418`, `docs/design.md`) and adds the controller to a
  pointer vector there with `0x7E0A20` when the byte at `*(0x242BDE8) + 0x256`
  is set; the destructor removes it with `0x1AA090`. This looks like "pause
  while an event is open" (the lab's `pauseHolderCount`); not part of the
  event list.

### Event controller (constructor `0x10B9A50`, vtable `0x1D590F8`)

| Offset | Meaning | How it was checked |
| --- | --- | --- |
| `+0x00` | vtable `0x1D590F8` | constructor `0x10B9B3B` |
| `+0x08` | second vtable `0x1D590E0` (sub-object) | constructor |
| `+0x30` | `int32`, index of the first visible option, -1 before filling | fill sets it in the option loop; `0x10BC580` uses it as the auto-selected option |
| `+0x38` | event window, full `windowType` object (primary vtable `0x1D926B0`; GuiObject at `+0x30` of it) | fill `0x10BA23C` stores the result of `vfunc[0x48]("EventWindow")` on the object at `session + 0x58`, which is used like the GUI manager (the constructor looks the type up in its `+0xC8` registry; manager slot `0x48` is `0x14E9030`, create window by type name; that `session + 0x58` is the manager itself was not checked); then calls the window's slots `0x98` ("Title", instant text box getter `0x1548D30`), `0xC8` ("event_picture", icon getter `0x15492E0`), `0x68` ("event_goto_province", button getter `0x1548700`), `0x1A8` ("event_option_listbox") |
| `+0x40` | session | constructor (`rdi`), destructor and click handler call session slots through it |
| `+0x48` | event object | constructor (`r15`); fill reads title, descriptions and options from it |
| `+0x50` | copy of the scope (copied by `0xCF6A0`) | constructor; passed as scope to triggers, localisation and the option command |
| `+0x120` | `int32`, pending-event id (last constructor argument) | constructor; copied into the option command |
| `+0x124` | byte: still waiting for a choice | set to 1 before the first fill; cleared by the click handler, by auto-selection and by slot `0x28` (`0x10B9A20`); slot `0x18` returns it |
| `+0x128` | slot object for option clicks, function `0x10BC220` at `+0x198` | constructor; fill connects it to each option button |
| `+0x208` | list of option buttons: head `+0x208`, tail `+0x210`, count `int32 +0x218`; nodes `{button*, prev, next +0x10}` | fill appends each button with `0x0DF120` (`0x10BB81F`); the click handler searches it |
| `+0x220` | `double`, timer value when the controller was built | constructor (timer object `0x23494A8`, `vfunc[0x28]`); update compares against it |
| `+0x228` | slot object, function `0x10BA180` (calls window `vfunc[0x60]`) | constructor; connected to the window |
| `+0x308` | slot object for the goto buttons, function `0x10C0210` at `+0x378` | constructor; fill connects it |

Vtable `0x1D590F8` (11 slots): `0x00` destructor `0x10BA030`, `0x08`
`0x8F450` (false), `0x18` per-frame update `0x10BBEB0`, `0x28` `0x10B9A20`
(clear `+0x124`), `0x40` `0x6E7480`, `0x48` `0x10BC820`, `0x50` fill
`0x10BA1A0`; the rest are `0x8F450`.

### Event object (`eventtype`; token handler `0x6EE910`, vtable `0x1CAB3A8`)

The script token ids come from the token table in `.data` (records
`{int32 id; char name[]}`; for example `0x0B` precedes `"id"` at `0x1FD8664`,
`0x1B` precedes `"name"`, `0x2969` `"option"`, `0x2996` `"title"`, `0x3484`
`"highlight"`, `0x2966` `"trigger"`).

| Offset | Meaning | How it was checked |
| --- | --- | --- |
| `+0x10` / `+0x14` | lookup key `{int32 type = 0x35, int32 numeric id}` | the registration slot `0x40` (`0x14CF190`) writes the 8-byte key to `+0x10` and inserts the object into the global lookup at `0x2340160[type]`; `0x14CF730({0x35, id})` finds it again |
| `+0x20` | `std::string`, the `id = ...` text as written in the file | `0x6EE910`, token `0x0B`: copies the token text to `+0x20` (`0x6EEC19`); the same string is the `"id"` field of the option telemetry in `0x6EE530` and appears in the log lines "Selected event option N for event X" (`0x10BC220`) and "Auto-selected event option N for event X" (`0x10BC580`) |
| numeric id | for a numeric `id` the number itself (an error "Event id too big: " above 99 999); for a namespaced id (`atoi` gives 0) a number assigned by `0x6F5500` from the text, and the text is also copied to `+0x830` | `0x6EE910` after `0x6EEC2B` |
| `+0x790` / `+0x798` / `+0x7A0` | `std::vector<option*>` in file order | token `0x2969`: allocate `0x330` bytes, construct with `0x6ED900`, parse, push back; the fill loop (`0x10BB593`) and the option execution `0x6EE530` (`0x6EE56D`) index it |
| `+0x7E0` | `std::string`, `title` key | token `0x2996`; fill localises it for "Title" |
| `+0x180` | effect block run for option index < 0; the `event` command runs it before opening the window (probably `immediate`: token `0x2A58` parses into the object at `+0x130`, not traced further) | `0x6EE530`, `0xE5C162` |

### Option object (constructor `0x6ED900`, vtable `0x1CAB438`, 0x330 bytes)

| Offset | Meaning | How it was checked |
| --- | --- | --- |
| `+0x50` | effect block | `0x6EE530` calls `vfunc[0x78]` on `options[i] + 0x50` |
| `+0x88` | `std::string`, `name` key | token handler `0x6F6B40`, token `0x1B`: copies the token text to `+0x88` (`0x6F6C5F`); fill localises it as the button text |
| `+0xA8` | `trigger` block | `0x6F6B30` evaluates it (`add rcx,0xA8; jmp 0xB98200`) |
| `+0x120` | byte, `highlight` | token `0x3484`; fill then sets the sprite `GFX_event_button_547_green` |

### How the fill function builds the options (`0x10BA1A0`)

For each option `i` of `event + 0x790`, in order:

1. If the option's trigger fails (`0x6F6B30`), no entry is made, but `i`
   still advances.
2. Otherwise it creates an `event_option_entry` (`0x152DF80`, a 0x50-byte list
   entry whose window is at `+0x40`), gets `event_option_button` and
   `event_option_goto_button` from it, sets the button text to the localised
   `option + 0x88` (button slot `0x290`, `0x15ADFA0`; "Missing Localization!"
   when the key has none).
3. It builds the string `to_string(event + 0x14) + "_" + to_string(i)`
   (`0x10BB74C`–`0x10BB778`) and gives it to button slot `0x118`, which is
   `0x1552930`: assign to the `std::string` at GuiObject `+0x38` (the "Current
   GUI scope" of the widget-tree note).
4. It stores `controller + 0x50` at button `+0x10` (`0x10BB7BA`) and connects
   the button's signal (`+0xC8`, `vfunc[0x08]`) to the slot at
   `controller + 0x128`.
5. The goto button gets the same string and slot `+0x308`, or is hidden
   (slot `0x2F8`) when the option has no province.
6. The button is appended to `controller + 0x208`; the entry is added to the
   list box; `+0x30` is set to `i` if it is still -1.

The `event_goto_province` button gets `"<id>_-1"`.

### What a click does

- `0x10BC220(controller, button)`: returns unless `+0x124` is set and the
  button is in the `+0x208` list. Splits button `+0x38` at `_`, requires two
  parts, `atoi`s the second as the option index, logs "Selected event option
  N for event <event + 0x20>", builds the option command `0x6F2660(event,
  controller + 0x50, index, controller + 0x120)` (0x140 bytes, vtable
  `0x1CAB740`) and posts it to the command queue at
  `session->vfunc[0x80]() + 0x58`, then clears `+0x124`. It does not check the
  button state.
- `0x10C0210` (goto) splits the same string, looks the event up with
  `0x14CF730({0x35, first part})` and the option as `event + 0x790`
  `[second part]`. So both parts are meant to be read back.
- Update `0x10BBEB0`, every frame: while less than the float define at
  `0x233E854` has passed since `+0x220`, every option button gets slot
  `0x270` (disable, state 3); afterwards slot `0x268`. Then it looks for its
  pending event in `root + 0x1EA0` (0x28-byte entries, count `int32
  root + 0x1EAC`: event `+0x10`, scope `+0x18`, deadline date `+0x20`, id
  `+0x24`; added by `0x756F00`) by event and scope fields; past the deadline
  it calls `0x10BC580`, which posts the option at `+0x30` ("Auto-selected
  event option"). Returns `+0x124`.

### Base-game event files (read-only scan of `events/*.txt`)

6 366 `country_event`/`province_event` blocks, 11 535 options, at most 33
options in one event, 377 numeric ids, no duplicate ids. 657 events have at
least one option with a `trigger`. 64 options have no `name`. 26 events use the
same option `name` twice (for example `flavor_gre.1` with `flavor_gre.1.a`,
`flavor_tur.2` with `EXCELLENT`), usually on options with exclusive triggers.

## Hypotheses (need a live check)

- E1: the session deletes a pop-up when its slot `0x18` returns 0 (the loop
  that does it was not found), so a chosen event's controller, list entries
  and window disappear within a frame or two.
- E2: `session + 0x498` holds exactly the event windows on screen; entries
  whose node byte `+0x18` is set are being removed and must be skipped.
- E3: button `+0x38` is unique per open event (`<numeric id>_<index>`) and
  the numeric id is stable during a session; across game versions or mod sets
  it is not (it is assigned at load for namespaced ids). Report `event + 0x20`
  to clients, never the number.
- E4: `+0x30` is the option the game picks itself at the deadline, and it is
  the first option whose trigger passed when the window was filled.
- E5: the option buttons are disabled (state 3) for the define at
  `0x233E854` after the window opens; clicking then does nothing.
- E6: the strings are 8-bit game text; ids and keys are ASCII in the base
  game, but mods may use other bytes.
- E7: everything here changes only on the main thread (GUI and fill run from
  the frame); read it as a main-thread read.

## Proposed read path

As a main-thread read (glossary), after an outer frame:

1. `root = *(base + 0x233FE78)`; `session = *(root + 0x1E00)`; require
   `*(session) == base + 0x1CB8460` (as `state` does).
2. Walk `session + 0x78` (node `+0x00` pop-up, `+0x10` next). Keep pop-ups
   with `*(p) == base + 0x1D590F8` and byte `p + 0x124 != 0`. Cross-check:
   `session + 0x498` has an entry with `event == *(p + 0x48)`,
   `scope == p + 0x50`, `window == *(p + 0x38)`. The order of `+0x78` is the
   opening order.
3. For each controller: `event = *(p + 0x48)`; id = MSVC `std::string` at
   `event + 0x20` (pointer if capacity `+0x38 > 15`, else inline; length
   `+0x30`); title key `event + 0x7E0`; window `*(p + 0x38)` (GuiObject
   `+0x30`, for the widget path).
4. Options: `begin = *(event + 0x790)`, `end = *(event + 0x798)`; for index
   `i`, `opt = begin[i]`, key = `std::string` at `opt + 0x88` (may be empty),
   highlight byte `opt + 0x120`.
5. Visible options: walk `p + 0x208`; for each button `b`: parse the
   `std::string` at `b + 0x38` as `<number>_<i>`; require
   `<number> == *(int32*)(event + 0x14)` and `*(b + 0x10) == p + 0x50`; text
   at `b + 0x200`, state `int32 b + 0x124` (3 = disabled), visible `b + 0xC5`.
6. Answer per event: id, title key, and per option index, name, visible,
   enabled, button text and the button's widget path.
7. `event choose <id> <index|name>`: resolve to one button (a name that
   matches more than one visible option is an error), refuse while state is 3
   or `p + 0x124 == 0`, then use the existing move + click path with the
   hover check from the widget-tree note. Confirm by `p + 0x124` turning 0 or
   the controller leaving `session + 0x78`.

A game call to `0x10BC220(p, b)` on the main thread would choose without
input, but it skips the disabled-button delay; if ever used it must check the
state first. Not proposed for now.

### Candidate `[[expect]]` entries

Bytes read from the file; RVA = first byte.

```toml
[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x828C20
bytes = "48 8B 05 51 72 B1 01 80 B8 E0 74 00 00 00 74 30 B9 E8 03 00 00 E8 9A A6 20 01 48 89 44 24 30 89 7C 24 20 4C 8B CE 4C 8B C5 48 8B D3 48 8B C8 E8 FC 0D 89 00 90 48 8B D0 48 8B CB E8 80 3E EC FF"
name = "session slot 0x108: new event controller (0x10B9A50), append to session pop-up list (0x6ECAE0)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x6ECB39
bytes = "48 8B 9E 80 00 00 00 B9 20 00 00 00 E8 8A 67 34 01 48 89 38 48 89 58 08 48 C7 40 10 00 00 00 00 C6 40 18 00"
name = "pop-up list append: tail session+0x80, node {popup, prev, next +0x10, flag +0x18}"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10B9B3B
bytes = "48 8D 05 B6 F5 C9 00 48 89 03 48 8D 05 94 F5 C9 00 48 89 43 08 C7 43 30 FF FF FF FF 33 F6 48 89 73 38 48 89 7B 40 4C 89 7B 48"
name = "event controller ctor: vtable 0x1D590F8, +0x30 = -1, window +0x38, session +0x40, event +0x48"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10BA234
bytes = "48 8D 54 24 50 41 FF D0 49 89 47 38"
name = "event fill: created EventWindow stored at controller+0x38"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10BB593
bytes = "49 8B 47 48 48 8B 88 90 07 00 00 48 8B 80 98 07 00 00"
name = "event fill: options vector at event+0x790/+0x798"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10BB74C
bytes = "49 8B 4F 48 8B 51 14 48 8D 4D 68 E8 A4 82 64 00"
name = "event fill: numeric event id at event+0x14 for the button string"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10BB7A8
bytes = "49 8B 04 24 48 8D 54 24 70 49 8B CC FF 90 18 01 00 00 49 8D 47 50 49 89 44 24 10"
name = "event fill: button slot 0x118 gets '<id>_<index>', button+0x10 = controller+0x50"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10BB81F
bytes = "49 8D 8F 08 02 00 00 48 8D 95 18 01 00 00 E8 EE 38 02 FF"
name = "event fill: append option button to controller+0x208"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10BBAE8
bytes = "48 8B 05 89 43 28 01 48 8B 88 00 1E 00 00 48 8B 01 4D 8B 4F 38 4D 8B C4 49 8B 57 48"
name = "event fill: session slot 0x2F0 gets (event, scope, window)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x82B055
bytes = "48 89 18 48 89 78 08 48 89 70 10 48 8B 9D A0 04 00 00"
name = "session slot 0x2F0: entry {event, scope, window}, list tail session+0x4A0"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x1552930
bytes = "48 83 7A 18 10 72 03 48 8B 12 49 C7 C0 FF FF FF FF 49 FF C0 42 80 3C 02 00 75 F6 48 83 C1 38 E9 BC 27 B4 FE"
name = "GuiObject slot 0x118: assign string at +0x38"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x10BC220
bytes = "40 57 48 83 EC 40 80 B9 24 01 00 00 00 48 8B F9 0F 84 92 00 00 00 4C 8B 81 08 02 00 00 4D 85 C0 0F 84 82 00 00 00 49 39 10 4D 8B 40 10 74 0B 4D 85 C0 75 F2 48 83 C4 40 5F C3 48 85 D2 74 69 48 8D 4A 38 41 B0 5F"
name = "option click: +0x124 active, button in +0x208 list, split button+0x38 at '_'"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x6EEC19
bytes = "4C 8D 61 20 49 8B D6 49 8B CC E8 E8 64 9A FF 49 8B CE E8 A8 A3 35 01 44 8B E8 41 BF 04 00 00 00 3D A0 86 01 00"
name = "event parser, token id: text to event+0x20, atoi, limit 100000"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x6F6C5F
bytes = "48 81 C2 44 04 00 00 49 C7 C0 FF FF FF FF 0F 1F 00 49 FF C0 42 80 3C 02 00 75 F6 48 81 C1 88 00 00 00 48 83 C4 20 5F E9 85 E4 99 FF"
name = "option parser, token name: text to option+0x88"
```

The vtables Tongs compares at run time (`0x1D590F8` for controllers,
`0x1CB8460` for the session) are data; the constructor entry above covers the
controller's. The `std::string` layout (data/inline buffer, length `+0x10`,
capacity `+0x18`, pointer when capacity > 15) is the MSVC one used throughout,
for example in `0x1552930`.

## Open questions

- Who deletes finished pop-ups and when (E1); whether a controller can stay in
  `session + 0x78` with `+0x124 == 0` for long.
- The meaning of `root + 0x74E0` (gate in `0x828C00`) and `root + 0x7484`
  (the constructor fills the window only when it is set): probably "this
  player sees event windows"; not checked for observer or multiplayer.
- Which country the event belongs to: the scope copy at `controller + 0x50`
  (0xD0 bytes, copied by `0xCF6A0`) was not decoded. Useful for reports in
  multiplayer or after a tag switch.
- Description: `event + 0x820` (array) / `+0x82C` (count) of conditional
  descriptions, the first one whose trigger holds; layout of the entries
  (`+0x10` key, triggers at `+0x38`/`+0x44`) only skimmed.
- The define behind `0x233E854` (option delay) and its default.
- Whether mods can produce options whose index differs from file order (for
  example by event overrides that replace a whole event: the later file
  wins, and duplicate ids log an error in `0x6EE910`).

## Live checks a later `task` ticket should run

On the Russia save of 17 March 1719 through `continue=`, with the existing
hook; reads only, except the clicks and the console command.

1. Open an event with the console: `event <id>` (command 29, not behind the
   developer lock in the console research) for an event with several options,
   one of them hidden by a trigger in the current state. Expect one new
   controller in `session + 0x78` with vtable `0x1D590F8`, one new entry in
   `session + 0x498`, `event + 0x20` equal to the id typed, and option names
   equal to the event file.
2. Read every option button's `+0x38` and check the indexes skip the hidden
   option; check `*(b + 0x10) == p + 0x50` and `b + 0x200` equals the
   localised text on screen.
3. Read the button state right after opening and again a second later (E5).
4. Open two events, check order and that both lists have two entries; choose
   the second, check the first stays and the second leaves both lists (E1,
   E2), and that `game.log` (or the channel the game uses) has "Selected event
   option N for event X".
5. Let an event expire (or find one near its deadline) and check the
   auto-selected option is `+0x30` (E4).
6. Trigger an event whose id is numeric (one of the 377) and check
   `event + 0x20` holds the digits.

## Sources in the binary

| RVA | What |
| --- | --- |
| `0x10B9A50` | event controller constructor (`eventwindow.cpp`) |
| `0x10BA1A0` | fill (title, description, picture, option entries) |
| `0x10BA030` | controller destructor |
| `0x10BBEB0` | controller per-frame update, option delay, deadline |
| `0x10BC220` | option click handler |
| `0x10BC580` | auto-select at the deadline |
| `0x10C0210` | option goto handler |
| `0x828C00` | session slot `0x108`: show event |
| `0xE5BB30` | console command `event` |
| `0x6ECAE0` | append to session pop-up list `+0x78` |
| `0x826C20` | close all pop-ups, clear `+0x498` |
| `0x82B020` / `0x82B0C0` | session slots `0x2F0` / `0x2F8`: add / remove open event entry |
| `0x6EE910` | event token handler (id, title, option, ...) |
| `0x6F6B40` | option token handler (name, highlight, ...) |
| `0x6ED900` | option constructor |
| `0x6EE530` | execute option `i` of an event |
| `0x6F2660` | option command constructor |
| `0x756F00` | add pending event to `root + 0x1E98` |
| `0x14CF190` / `0x14CF730` | register / look up object by `{type, id}` |
| `0x1552930` | GuiObject slot `0x118`: set string `+0x38` |
