# Research: how the widget tree is laid out in memory

Ticket: happy-dwarf-team/tongs#18 (child of #15). Static analysis only; the
game was not started.

Source: `eu4.exe` 1.37.5 (Steam, win64), SHA-256
`9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A`, checked
before and after the work. Tools: Ghidra 12.1.4 headless (auto-analysis of the
lab project), the lab's `scripts/ghidra/*` helpers, raw byte reads of the PE
file. All addresses are RVAs relative to the image base. Offsets are in hex.
The game's own `.gui` files (install folder, read only) were used to match
names. "GuiObject" below is our name for the common widget base; the binary
has no RTTI for GUI classes (see "Widget classes").

## Question

Find the root of the open interface, how a widget holds its children, and
where each widget keeps its `.gui` name, position and size, visibility, and
displayed text. Which classes are widgets (windows, buttons, text boxes,
icons, event pop-ups)? How does a click reach a button, so we know whether
clicking its rectangle through input is enough?

## Verified statically

"Verified" means read in the disassembly or decompiler output of the named
function; it says nothing about behaviour at run time.

### Root: the GUI manager

- The GUI manager is a global pointer at `0x23494F0`. Its constructor
  `0x14E8640` (in `clausewitzlib/graphics/gui.cpp`) stores `this` there
  (instruction at `0x14E8904`), sets the vtable `0x1D8DC40` and initialises
  sound names `click`, `ui_menu_over`, `ui_menu_click`, `ui_menu_leave`.
- `mgr + 0x110 / +0x118 / +0x120` is a `std::vector<GuiObject*>`
  (begin/end/capacity), reserved for 64 000 entries in the constructor.
  Manager vtable slot `0x98` (`0x14EB360`) appends to it. Every widget is
  registered there, not only top-level windows:
  - the manager's own create functions (vtable slots `0x40`–`0x88`; read:
    `0x14E8FB0`, `0x14E9030`, `0x14E90F0`, `0x14E8F50`, `0x14E8EF0`;
    `0x14E9030` = create window by type name, used by the console command
    `window open <name>` in `0xE584C0`) call slot `0x98` with the new object;
  - the container window's child builder `0x1638CC0` calls the same slot for
    every child it creates (`(*(type+0x398))->vfunc[0x98](child)`; the object
    at `type+0x398` is the manager, the same value the constructors store at
    `GuiObject+0xB8`).
- The registry is compacted by the GUI update `0x14E9FA0` (caller
  `0x14C5810`, not traced further): entries whose "dead" byte
  `+0xC4` is set get `vfunc[0x38](1)` (deleting destructor) and are removed
  from the vector. A pointer read from the registry is therefore valid only
  until the next sweep (thread: see H9).
- The tutorial's widget lookup `0x1408420` matches a path of names: it scans
  `mgr+0x110` for an object whose name equals the first element, then walks
  down with `0x1408310` / `0x1408240`, calling `vfunc[0x1E8](name)` per step
  and comparing the last element with `vfunc[0xA0]()`. Errors:
  "Failed to find guiobject to show in tutorial: ".
- `mgr + 0x130` is the topmost hovered object. A debug overlay in `0x10D4390`
  prints it as "Current GUI object" (`vfunc[0xA0]`), "Current GUI scope"
  (`std::string` at `+0x38`) and "File : line nr" (type `+0x30` and `+0xA0`).
  It is written by the hover callback `0x15528D0` and cleared by the
  GuiObject destructor `0x1552680` when it points at the dying object.
- `mgr + 0x140` is the keyboard focus object (`0x14EA150`, `0x14EB4B0`: on
  focus change the old object gets `vfunc[0xC0]`, the new one `vfunc[0xB8]`).
- `mgr + 0x170 / +0x178` is a second vector: objects for which
  `vfunc[0x238]()` is true when they are added; `0x14E9FA0` calls
  `vfunc[0x240]` on each per frame.

### GuiObject base

Constructor `0x1552750` (`this, type, manager`), base vtable `0x1D91810`,
destructor `0x1552680`. Offsets relative to the GuiObject pointer, which is
what the registry stores:

| Offset | Meaning | How it was checked |
| --- | --- | --- |
| `+0x00` | vtable | constructors |
| `+0x38` | `std::string`, shown as "Current GUI scope" | debug overlay `0x10D4390` |
| `+0x60` | `int32`, -1 by default; hover depth limit | hover callback `0x15528D0` |
| `+0x8F` | byte: hovered this frame | set by `0x15528D0`, cleared per object per frame in `0x14E9A70` (copied to `+0xAA` first) |
| `+0xB0` | type object (the parsed `.gui` definition) | constructor; `GetName` |
| `+0xB8` | the GUI manager | constructor; destructor clears `mgr+0x130` through it |
| `+0xC4` | byte: dead, waiting for the sweep | base slot `0x78` (`0x1552980`) sets it; `0x14E9FA0` deletes |
| `+0xC5` | byte: visible | `Show`/`Hide` of button (`0x15AEB30`/`0x15AEAD0`), text box (`0x152AC90`/`0x152AD10`), window (`0x1547D90`/`0x1547B00`); per-frame update, picking and dispatch skip objects with `+0xC5 == 0` |

Virtual slots (offsets into the vtable the GuiObject pointer points at):

| Slot | Meaning | Evidence |
| --- | --- | --- |
| `0x08` | hover callback (`0x15528D0`): sets `+0x8F`, writes `mgr+0x130` | called for each picked object in `0x14DEFD0` |
| `0x10` | mouse/input dispatch (`0x1552800`), forwards to `0x48` | called by `0x14EA3D0`, `0x14EA630` |
| `0x18` | returns byte `+0x8C` | `0x15529A0`; used when `mgr+0x1A` is set (modal filter) |
| `0x38` | deleting destructor | registry sweep |
| `0x48` | input handler | button `0x15AEFC0`, icon base `0x1592BD0` |
| `0x78` | mark dead | `0x1552980` |
| `0x88` / `0x90` | show / hide | see `+0xC5` |
| `0x98` | position, out `int32[2]` | base `0x1148120` returns (0,0); button `0x1571DC0` returns `+0x178`; window part `0x154A7B0` |
| `0xA0` | name, returns `const std::string*` | base `0xFBF480`: `type->vfunc[0x40]()` |
| `0xD8` | size from the render node, out `int32[2]` | button `0x15AEDE0`, text box `0x152B060` (node `vfunc[0x178]`, floats truncated) |
| `0xE0` | size used for layout, out `int32[2]` | container child check `0x1638450` adds it to the position |
| `0x1E8` | find child by name (not a full search, see below) | tutorial walker; window part `0x1548040`, container part `0x1636A50` |
| `0x258` | button/icon state (`int32 +0x124`) | `0x1148250` |
| `0x270` | button/icon: disable, sets state 3 | `0x15937A0` |

Type objects: type `vfunc[0x40]` is `0x9A920` (`lea rax,[rcx+0x10]`), so the
type's name is the `std::string` at `type+0x10`. That is the `name = "..."`
from the `.gui` file, case preserved. `type+0x30` is the `.gui` file name and
`type+0xA0` the line number (`int32`), both from the debug overlay.

### How windows hold children

Windows have two bases: a signal/slot base at `+0x00` (vtable written first is
`0x1D92688`) and the GuiObject at `+0x30`. The registry and the child lists of
other windows hold the `+0x30` pointer; the window-type lists described below
hold full-object pointers.

Children are not kept in one generic list. Each window class keeps one
container per widget kind; the names in those containers are lower-cased
(`0x15503F0` calls `tolower`, and both lookups lower-case the query).

**`windowType` instances** (the class in `fixedwindow.cpp`: constructor
`0x1543F80`, primary vtable `0x1D926B0`, GuiObject-part vtable `0x1D929D0`;
created by the manager in `0x14E9030` → `0x155C260`). Offsets from the full
object, from the typed getters in the primary vtable, `FindChild` `0x1548040`,
the error helper `0x154E930` ("Missing <kind> '<name>' in window '<name>'")
and the destructor `0x1546730`:

| Container | Kind (from the getter's error text) |
| --- | --- |
| vectors `+0x5B8`, `+0x5E8`, `+0x618` (pointers) with names at `+0x18` from each | Button (`0x1548720` searches all three) |
| vector `+0x648` / names `+0x660` | Icon (`0x15492E0`) |
| vector `+0x678` / `+0x690` | Browser (`0x1549700`) |
| vector `+0x6A8` / `+0x6C0` | CurveGraph (`0x1549800`) |
| vector `+0x6D8` / `+0x6F0` | DropDownBox (`0x15498F0`) |
| vector `+0x708` / `+0x720` | InstantTextBox (`0x1548D30`, `0x1548EF0`) |
| vector `+0x738` / `+0x750` | Overlapping Elements Box (`0x1549550`) |
| vector `+0x768` / `+0x780` | unknown (last fallback of `FindChild`) |
| named maps at `+0x798`, `+0x7D8`, `+0x818`, `+0x858`, `+0x898`, `+0x8D8`, `+0x918` | `+0x798` TextBox (`0x15489E0`), `+0x7D8` EditBox (`0x1548F80`), `+0x818` Checkbox (`0x1548610`); the other four unknown |
| vector of 0x28-byte `{std::string name; window*}` at `+0x958` | sub-windows ("Warning: Attempt to access nonexistent subwindow", `0x15481A0`) |

A "vector pair" is two `std::vector`s: pointers (8 bytes each) at `P..P+0x10`
and lower-cased names (`std::string`, 0x20 bytes each) at `P+0x18..P+0x28`,
same index. Searched by `0x154F650` / `0x154F520`.

A "named map" (vtable `0x1D8DC00`, 0x40 bytes) keeps an insertion list at
`map+0x20` (head) / `+0x28` (tail): list nodes are `{entry*, prev, next}`
(next at `+0x10`), entries are `{value; std::string key at +0x08; flag at
+0x28}` (insert `0x14EC950`, rebuild `0x14EC750`, destructor `0x14EC650`).
Walking the list from `+0x20` visits every entry.

`FindChild` (`0x1548040`) only searches the first two button vectors, the
TextBox and EditBox maps, the sub-windows and the `+0x768` vector. It does not
find icons, instant text boxes or the other kinds, so a selector engine cannot
rely on it.

**`containerWindowType` instances** (`containerwindow.cpp`: constructor
`0x16307B0`, primary vtable `0x1DA42E8`, GuiObject-part vtable `0x1DA4090`,
type at `+0xA58`). The child builder `0x1638CC0` creates children from the
type's lists and stores them in vector pairs; the typed getters name them
("Could not find ..."):

| Vector pair (full object) | Kind | Created by |
| --- | --- | --- |
| `+0x680` | buttonType | type `vfunc[0x70]` |
| `+0x6B0` | icon | type `vfunc[0x70]` |
| `+0x6E0` | icon (second list) | `0x15F5F30` |
| `+0x710` | curvegraph | `0x15F0710` |
| `+0x740` | instantTextboxType | `0x15E7600` → `0x1529D20` |
| `+0x770` | overlapping elements box | `0x163CC10` |
| `+0x7A0` | editBoxType | `0x15E7F60` → `0x1533360` |
| `+0x7D0` | checkBoxType | type `vfunc[0x78]` |
| `+0x800` | unknown | `0x15EBBF0` → `0x152E220` |
| `+0x830` | unknown | `0x15EBD10` → `0x1583000` |
| `+0x860` | unknown | `0x15E8C30` → `0x1552B80` |
| `+0x890` | containerWindowType | `0x15EEB00` → `0x16307B0` |
| `+0x8C0` | gridBoxType | `0x1590770` → `0x1590820` |
| `+0x8F0` | extendedScrollbarType | `0x163DB10` |
| `+0x920` | dropDownBoxType | `0x15F02C0` → `0x15E5AE0` |

In the game's `.gui` files `windowType` is by far the most common window
(855 definitions against 30 `containerWindowType`).

### Position, size, text, state

- Button: position `int32 x, y` at `+0x178` (getter `0x1571DC0`), computed in
  `0x15AE930` from the orientation at `+0x230`, the `.gui` offset at `+0x234`
  and a size passed by the caller, then given to the render node
  (`+0x290`, node `vfunc[0x168]`). The caller passes a size used for the
  orientation, so the value is probably local to the parent (H1).
- Button text: `std::string` at `+0x200`, written by slot `0x290`
  (`0x15ADFA0`), which also passes it to the render nodes. Slot `0x250`
  (`0x15AF090`) sets it from `type+0x2A0` through `0x14B34A0`.
- Button state `int32 +0x124` (slot `0x258`). Slot `0x270` (`0x15937A0`)
  sets 3. Press (`0x1593BA0`) does nothing in state 1 or 3; release
  (`0x1593C80`) returns at once in state 3. State 3 is therefore "disabled".
- Instant text box: render node at `+0x118`; text `std::string` at `+0x170`
  (setter `0x152AE10`, which also hands it to the node via `0x15F42C0`);
  size from the node (slot `0xD8`).
- Window part of `windowType`: position from `+0x598` of the full object
  (`0x154A7B0` reads `+0x568` from the `+0x30` pointer); name override
  `std::string` at full `+0xA58`, used by `GetName` (`0x154AD60`) when not
  empty, otherwise the type name.

### How a click reaches a button

1. Every frame `0x14E9A70` reads the pointer through the input object's
   `vfunc[0xB8]` and scales it by two floats of the GUI render context
   (`*(mgr+0xC0)`, `+0x4D8` / `+0x4DC`) into GUI units. The input class with
   vtable `0x1DA3530` (constructor `0x162DD30`) has `0x162E2D0` in slot `0xB8`,
   which calls `0x14E5E50`, the function Tongs already hooks.
2. Picking `0x14DDA50` walks the render nodes at `ctx+0x298`. A node counts
   when its owner (`node+0x118`) says yes in slot 0, the point is inside the
   node's clip rectangle (four `int16` at `node+0x128..+0x12E`, when the flag
   at `node+0x130` is set), and the node's own hit test `vfunc[0x88]` is not
   0. `0x14DEAF0` turns the hit list into owners, last drawn first, skipping
   owners that are not visible (`+0xC5`) or dead (`+0xC4`), at most 1000.
   `0x14DEFD0` then calls the hover callback, which sets `+0x8F` on each and
   leaves the topmost in `mgr+0x130`.
3. Queued input events (0x70-byte nodes; class at `+0x50`: 0 mouse, 1 key, 2
   text, 3 other) are handled in `0x14E9A70`. Mouse events go to `0x14EA3D0`.
   For a button event (`+0x14` is 3 or 4) whose position differs from the
   picked pointer by more than 2 units, the context picks again first. Then
   each picked owner, topmost first, gets `vfunc[0x10]` until one sets the
   "consumed" flags at `+0x54`.
4. The button handler `0x15AEFC0` returns early if an attached object at
   `+0x228` refuses (`vfunc[0x270]`) or the byte `+0x289` is set, and
   otherwise calls the icon base handler `0x1592BD0`. That handler ignores
   mouse events unless `+0x8F` (hovered this frame) is set. Left button (code
   1 at `+0x18`) needs `+0x160`, right button (2) needs `+0x161`. Press (kind
   3 at `+0x14`) calls `0x1593BA0`, release (kind 4) calls `0x1593C80`, which
   fires the click (`0x1593260`) when the button was pressed (state 1) and is
   not disabled. Kind 5 is the double click the SDL loop `0x1569C70` makes
   from `GetDoubleClickTime`.

So a click lands on the topmost visible widget whose render node passes the
hit test at that point. The game's per-frame pointer, not only the event's
coordinates, decides `+0x8F`. This explains why `SDL_PushEvent` alone did
nothing and the cursor hook made clicks work.

## Hypotheses (need a live check)

- H1: absolute screen position = sum of `vfunc[0x98]` from the top-level
  window down the container chain, in GUI units; pixel = GUI / scale
  (`ctx+0x4D8`, `+0x4DC`). Not proven: the render nodes may hold their own
  world transform, and orientation is applied when the position is computed.
  The live clicks from `tongs-stack-2` are not obviously inside rectangles
  computed this way from `speed_controls.gui` (for example `button_speedup`
  at `x = -89` from the right edge of a 3440-pixel window starts at 3351; the
  click was at 3350). Calibrate before relying on it.
- H2: the clickable rectangle is `vfunc[0xD8]` (render size) at the position
  above. `vfunc[0xE0]` is the layout size, which may differ for text boxes.
- H3: `*(0x23494B8)` is the same render context as `mgr+0xC0` (both are used
  with `+0x4D8`/`+0x4DC`; the global is written in `0x14D9A20`).
- H4: top-level windows = registry entries that are not a child of another
  registry entry. No root list was found. `mgr+0x170` holds only objects
  with per-frame updates.
- H5: a widget is on screen when it and all its ancestors have `+0xC5` set.
  `Hide` on a window clears only the window's own byte and hides its render
  node; the children keep `+0xC5 = 1`.
- H6: the text strings are the game's 8-bit text with its colour and icon
  escape codes, already localised (`0x14B34A0` builds them from keys);
  encoding not checked.
- H7: `+0x162` (set to 1 in the icon constructor, setter slot `0x160`) gates
  the double-click action; `+0x289` is set once in the button constructor
  from a type property and is not the "disabled" flag.
- H8: the event pop-up is a `windowType` named `EventWindow`
  (`interface/eventwindow.gui`), driven by the controller in `eventwindow.cpp`
  (constructor `0x10B9A50`, vtable `0x1D590F8`, window pointer at `+0x38`).
  `0x10BA1A0` fills "Title", "Description", "event_picture" and builds
  options as `event_option_entry` windows (each with `event_option_button`)
  inside the list box `event_option_listbox`. The list box's own item
  container was not traced.
- H9: the GUI update (`0x14E9A70`, `0x14E9FA0`) runs on the main thread, so
  the registry must be read there. Expected for a Clausewitz frame, not traced
  to the main loop.

## Widget classes

The game's GUI classes have no RTTI: the lab's RTTI export lists 82 classes
with a complete object locator, all from the C++ standard library, the CRT
and the name demangler, and no GUI class name occurs among the type
descriptors at all (the others are exceptions, lambdas and boost types). Classes are told
apart by their vtables. The `.gui` keyword → type → instance chain comes from
the window type parser `0x15EC770` (`containerwindowtype.cpp`), the type
constructors and the create functions.

| `.gui` keyword | Type ctor / type vtable | Instance ctor | Vtable at the GuiObject pointer | GuiObject offset |
| --- | --- | --- | --- | --- |
| `windowType` | registry at `mgr+0xC8` | `0x1543F80` | `0x1D929D0` (primary `0x1D926B0`) | `+0x30` |
| `containerWindowType` | `0x15EC1F0` / `0x1D9EB70` | `0x16307B0` | `0x1DA4090` (primary `0x1DA42E8`) | `+0x30` |
| `guiButtonType`, `buttonType` | `0x15B17E0` / `0x1D9A7B0` | `0x15AC670` (create `0x15AF5F0`) | `0x1D9A340` | `+0` |
| `checkboxType` | `0x15B17E0` / `0x1D9A7B0` | `0x152B930` (create `0x15AF410`) | `0x1D8FFC8` | `+0` |
| `iconType` | `0x1592000` / `0x1D98568` | `0x15857D0` (create `0x1592580`) | `0x1D96D00` | `+0` |
| `instantTextBoxType` | `0x15E74A0` / `0x1D9E478` | `0x1529D20` (create `0x15E7600`) | `0x1D8FD10` | `+0` |
| `editBoxType` | `0x15E7E10` / `0x1D9E558` | `0x1533360` (create `0x15E7F60`) | `0x1D91358` | `+0` |
| `OverlappingElementsBoxType` | `0x15EBE30` / `0x1D9E870` | `0x163CC10` | `0x1DA4910` | not checked |
| `gridBoxType` | `0x158FE90` / `0x1D98218` | `0x1590820` | `0x1D98288` | `+0` |
| `extendedScrollbarType` | `0x15EEC40` / `0x1D9ED10` | `0x163DB10` | `0x1DA4CF8` | `+0` |
| `dropDownBoxType` | `0x15EFAD0` / `0x1D9EE40` | `0x15E5AE0` | `0x1D9E0A8` | `+0` |
| `listboxType`, `smoothListboxType` | `0x15EAB80` / `0x1D9E800` | not traced | — | — |
| `scrollbarType` | `0x15E8460` / `0x1D9E5E0` | not traced (maybe `0x1552B80`, vtable `0x1D93748`) | — | — |
| `textBoxType` | not traced | not traced (map `+0x798` of `windowType`) | — | — |

Shared bases: GuiObject `0x1552750` / `0x1D91810`; icon base `0x1592840` /
`0x1D986D0` (abstract, base of icons and buttons). The second vtables at
`+0xC8` and `+0xF8` in buttons, icons and text boxes were not studied.

## Proposed read path and `[[expect]]` entries

Read the tree on the main thread as a game call (glossary: "Game call"). The
registry is compacted and objects are deleted on the main thread, so reading
from the pipe thread can follow freed pointers.

1. `mgr = *(base + 0x23494F0)`; read `mgr+0x110..+0x118`.
2. For each entry `g` (GuiObject*): skip `+0xC4 != 0`. Name =
   `*(std::string*)(*(type = g+0xB0) + 0x10)`, or call `vfunc[0xA0]` on the
   main thread (windows can override the name). Kind = the vtable at `g`
   (table above). File and line from `type+0x30` / `type+0xA0`.
3. Children: by kind. `windowType` windows (`vtbl == 0x1D929D0`, full object
   = `g - 0x30`): the ten vector pairs, seven named maps and the sub-window
   vector listed above. Container windows (`vtbl == 0x1DA4090`): the fifteen
   vector pairs. Other kinds: no children found yet. Mark every child and take
   the unmarked entries as roots (H4).
4. Position `vfunc[0x98]`, size `vfunc[0xD8]`, visible `+0xC5`, text: button
   `+0x200`, instant text box `+0x170`; button state `+0x124` (refuse to click
   in state 3).
5. Click: compute the centre (H1/H2), convert to client pixels, and use the
   existing `move` + `click` path. Before pressing, check on the main thread
   that `mgr+0x130` is the target after the move (the topmost hovered
   object). That one check covers occlusion, visibility and the hit test.

Candidate entries (bytes read from the file; RVA = first byte):

```toml
[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x14E8904
bytes = "48 89 35 E5 0B E6 00"
name = "gui manager ctor: stores this to the global 0x23494F0"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x14E9E16
bytes = "48 8B BE 10 01 00 00 48 8B 86 18 01 00 00"
name = "gui input: object registry vector at mgr+0x110/+0x118"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0xFBF480
bytes = "48 8B 89 B0 00 00 00 48 8B 01 48 FF 60 40"
name = "GuiObject vtable slot 0xA0: name from type at +0xB0, type vfunc 0x40"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x9A920
bytes = "48 8D 41 10 C3"
name = "gui type vfunc 0x40: name string at type+0x10"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x14DEAF0
bytes = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 41 56 41 57 48 83 EC 40 49 8B D8 8B EA 48 8B F1 48 8B 79 08 48 2B 39 48 C1 FF 03 83 EF 01 0F 88 85 00 00 00 4C 8D 35 9D F2 8B 00 4C 8D 3D C6 E4 8A 00 48 8B 06 48 8B 14 F8 48 8B 82 18 01 00 00 48 85 C0 74 16 80 B8 C5 00 00 00 00 74 0D 80 B8 C4 00 00 00 00 75 04"
name = "picking: owner at node+0x118, visible +0xC5, dead +0xC4"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x15528D0
bytes = "48 89 5C 24 08 57 48 83 EC 20 48 8B 01 8B FA 48 8B D9 FF 10 84 C0 74 2F 8B 4B 60 83 F9 FF 74 12 3B F9 0F 9E C0 88 83 8F 00 00 00 83 F9 FE 7E 17 EB 07 C6 83 8F 00 00 00 01 48 8B 83 B8 00 00 00"
name = "GuiObject hover callback: +0x8F hovered, manager at +0xB8 (current object mgr+0x130)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x1571DC0
bytes = "48 8B 81 78 01 00 00 48 89 02 48 8B C2 C3"
name = "button vtable slot 0x98: position at +0x178"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x1148250
bytes = "8B 81 24 01 00 00 C3"
name = "button/icon slot 0x258: state at +0x124 (3 = disabled)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x15AEAD0
bytes = "40 53 48 83 EC 20 C6 81 C5 00 00 00 00"
name = "button hide: visible byte +0xC5"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x152AE10
bytes = "48 89 5C 24 08 48 89 6C 24 10 48 89 74 24 18 57 48 83 EC 20 48 8D B9 70 01 00 00"
name = "instant text box set text: string at +0x170"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x1548040
bytes = "48 89 5C 24 10 57 48 83 EC 20 48 83 7A 18 10 48 8B F9 48 8B CA 48 8B DA 72 03 48 8B 0A 8B 52 10 4C 8D 8F 88 05 00 00 4C 8D 87 A0 05 00 00"
name = "windowType find child: first button vector pair (+0x588/+0x5A0 from the GuiObject part)"

[[expect]]
build = "eu4-1.37.5-steam-win64"
rva = 0x1636A50
bytes = "48 89 5C 24 08 57 48 83 EC 20 48 83 7A 18 10 48 8B F9 48 8B CA 48 8B DA 72 03 48 8B 0A 8B 52 10 4C 8D 8F 50 06 00 00 4C 8D 87 68 06 00 00"
name = "containerWindowType find child: first vector pair (+0x650/+0x668 from the GuiObject part)"
```

The plugin should also check the vtable values it uses to tell kinds apart
(they are data, so a vtable compare at run time plus an `[[expect]]` on the
constructor bytes that write them, for example `0x1543F80`), and the getter
of each container offset it reads. The button text offset `+0x200` is
covered by `0x15ADFA0`, which needs a longer byte range than shown here.

## Open questions

- Absolute coordinates and the GUI-unit to pixel scale (H1–H3).
- Roots: is there an explicit list of top-level windows, or only the
  registry (H4)?
- List boxes: where `listboxType` keeps its items (event options live in
  one); the classes behind the four unknown named maps of `windowType`
  windows and behind the container lists `+0x800`, `+0x830`, `+0x860`.
- Whether clicks on a button inside a hidden window can still be picked
  (H5), and what `+0x289` and `+0x228` mean for buttons (H7).
- Text encoding and whether `+0x200` / `+0x170` always hold what is drawn
  (text boxes may truncate or format; `0x15F42C0` copies the string into the
  render node at `node+0x1B0`).
- Thread safety of reading the registry outside the main thread; a game call
  avoids the question.

## Live checks a later `task` ticket should run

All on the Russia 1719 save through `continue=`, game unfocused, using the
existing hook. No writes to game memory.

1. On the main thread: dump the registry (count, then name, vtable, `+0xC5`,
   `+0xC4`, `type+0x30` and line for each entry). Expect names from
   `speed_controls.gui` (`speed_controls`, `button_speedup`,
   `button_speeddown`, `DateText`) and vtables from the table above.
2. Read `DateText` (`+0x170`) and compare it with the date from `state`.
3. For `button_speedup` / `button_speeddown`: read `vfunc[0x98]`,
   `vfunc[0xD8]`, `vfunc[0xE0]`, and the parent window's position. Compare
   with the click points that worked in `tongs-stack-2` ((3350,32) and
   (3350,62) on a 3440x1440 client) to settle H1–H3. Read `ctx+0x4D8/+0x4DC`.
4. `move` to the computed centre, read `mgr+0x130` and `+0x8F` of the target,
   then `click` and confirm the speed change from `state`.
5. Let an event pop-up open (or find one in a save): find the `EventWindow`
   entry, its `Title`/`Description` text, and the option buttons; click one
   and check the pop-up closes (`+0xC4` set, then gone from the registry).
6. Open and close a window with the console `window open|close <name>`
   (`0xE584C0`) and check the registry grows and the entry disappears after
   the sweep.
7. Hide a parent (for example close the ledger) and check its children keep
   `+0xC5` (H5).
