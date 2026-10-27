# Research: provinces, countries, the player and the selection (EU4 1.37.5)

Ticket: happy-dwarf-team/tongs#95 (child of the map #94). Static analysis only;
no game process was started. Build: `eu4.exe` 1.37.5 Steam win64, SHA-256
`9AD3EFE1AF169F40EE577F9DAE5DEBBC87AF6FB8B5450FB345EBF110DC4D771A` (checked on
the analysed copy before and after). All addresses are RVAs relative to the
image base `0x140000000`.

Methods, named per claim below:

- **decomp**: Ghidra 12.1.4 decompiler on the lab's analysed project, read
  locally; not reproduced here.
- **disasm**: Ghidra listing of the instructions at that RVA (the bytes in the
  candidate symbols come from it).
- **xrefs**: Ghidra references to an address.
- **strings**: string literals in the EXE (section table mapping) and their
  xrefs.
- **table**: the console's static command table (`0x1FC23E0`, 378 records of
  `0xA0` bytes, see `research/console-commands`), read from the file: handler
  pointer at record `+0x38`, map-click target type at `+0x98`.
- **files**: the game's own data files, read only (`map/default.map`,
  `map/definition.csv`, `localisation/prov_names_l_english.yml`).

Most facts come from console handlers that take a province ID or a tag
(`own`, `control`, `add_core`, `debug_name`, `moveunit`, `tag`, `golden_age`,
`observe_mode`), from script triggers (`is_sea`, `is_wasteland`,
`num_of_units_in_province`), from the script value getters (`base_tax`,
`base_production`, `base_manpower`, `development`) and from the province view
and the map click. Each of these is game code that reads the same objects a
world read would.

## Question

Where does EU4 1.37.5 keep its provinces and countries, and which of their
fields can a Tongs main-thread read take reliably? How does a province ID map
to an entry and what does an unused ID look like; where are the name, owner,
controller, cores, development, units, wasteland and sea; how does a tag map to
a country and where are its name, capital and "exists"; how is the player's
country found; what does the game keep for the current selection and the
province a view shows; which of these does the daily tick rewrite?

## Answer in short

- **Provinces** are one contiguous array of `0x2E10`-byte objects indexed
  directly by province ID: `root + 0x1CA8` holds the array base, so province
  `id` is `*(root + 0x1CA8) + id * 0x2E10`. Valid IDs are `1 … N` with
  `N = (root+0x1C98 − root+0x1C90) / 8`, the length of a `Province*` vector.
  The map object (`0x233FED8`) owns the same-stride array as a vector at
  `+0x6F0`; its getter `0x9AA80` logs `map.h` "Trying to access out-of-range
  province ID" and returns **entry 0**, not null, for a bad ID.
- **Province fields**: ID `+0x20` (int32), owner tag `+0x468`, controller tag
  `+0x470`, cores `std::vector<tag>` at `+0x490`, base manpower / tax /
  production `+0x3E0` / `+0x3E4` / `+0x3E8` (int32, presumably value × 1000),
  units list head `+0x350`, siege `+0x388`; the static (map) part at
  `*(+0x28)`: name `std::string` at `+0x10`, land flag `+0x228` bit 3 (clear =
  sea), wasteland `+0x229` bit 3, map position `+0x7C`.
- **Tags** are 8-byte values: three ASCII letters and a NUL, `int16` country
  index at byte 4, a kind byte at 6 (7 = the observer pseudo-country) and a
  "valid" byte at 7 (`---` with byte 7 = 0 means no country). The country
  object is `(*(0x233D8D0 + 0x118))[index]`; a tag string is resolved through
  64 hash buckets at `0x233D8D0 + 0x148`. Country: own tag `+0x20`, capital
  province ID `+0x14B8`, owned provinces `+0x1338` (int32 array) with count
  `+0x1344` (> 0 means the country exists on the map).
- **Player**: tag at `root + 0x1E60`; when its kind byte (`root + 0x1E66`) is 7
  (observe mode) and `root + 0x1E6F` is set, the game uses the observed tag at
  `root + 0x1E68` instead. Every reader found does exactly this.
- **Selection**: the province selection holder is `*(session + 0x1220)`;
  `holder[0]` is the selected province, `holder[2]` the province view, which
  keeps the displayed province at `view + 0x958`. Selected army and selected
  country were not found (open questions).
- **Stability**: all of these are written only on the main thread, the daily
  tick runs inside an outer frame and Tongs' dispatcher runs after outer
  frames only, so a main-thread read sees the state between ticks. The daily
  province tick rewrites per-province state every day, including the
  controller (through the controller setter) and the siege pointer.

## Verified statically

### Globals and the root

1. **The root `0x233FE78` and the map `0x233FED8` are separate objects.** The
   map is allocated with `0x1B70` bytes in `0x8794D0`, which writes it to
   `0x233FED8`; `0x87A330` (its destructor, vtable `0x1CBDA78`) clears it.
   `0x1FEFA0` clears both globals at teardown (`0x1FF07C`, `0x1FF13C`). These
   are the only three writes to `0x233FED8` (xrefs).
2. **The country database `0x233D8D0`** is a lazily created singleton
   (`0x800` bytes, `0x2812A0`): several readers allocate it when the pointer is
   null (`0xE42B00`, `0xE73C20`, `0x2892A0`; decomp). Its writes are mostly
   these inlined initialisations (758 write xrefs).

### Provinces: collection and IDs

3. **Count.** The console handlers `own` (`0xE75780`), `control`
   (`0xE75D20`), `add_core` (`0xE73C20`) and `debug_name` (`0xE866F0`) accept a
   province ID only when `1 ≤ id ≤ (root+0x1C98 − root+0x1C90) >> 3` and
   otherwise answer `Invalid or no province provided` (decomp; disasm of the
   check at `0xE8674D`). `root + 0x1C90` is a vector of 8-byte entries: the
   monthly tick `0x75D690` walks it and reads each entry as a province pointer
   (`+0x590`, `+0x468`, `+0x958`, `+0x8DC`, `+0x4D8`; decomp).
4. **Lookup by ID.** The same handlers, the triggers `is_sea` (`0xC467BD`),
   `is_wasteland` (`0xC400BD`) and `num_of_units_in_province` (`0xCDC7D8`) and
   `moveunit` (`0xE87A90`) all address a province as
   `*(root + 0x1CA8) + id * 0x2E10` (disasm/decomp). There is no indirection
   and no hash: the ID is the array index.
5. **The map's getter** `0x9AA80(map, id)`: if `id < (map+0x6F8 − map+0x6F0) /
   0x2E10` it returns `map+0x6F0 + id * 0x2E10`; otherwise it logs
   `Trying to access out-of-range province ID <id>` with the source
   `eu4/source/map.h` line `0x135` (309) and returns **`map+0x6F0`**, the
   entry for ID 0 (decomp; disasm `0x9AAA7`). The string `0x1C3A820` has 28
   xrefs in 26 functions, so the same bounds check is inlined widely. The
   map click passes `*(*(app+0x360) + 0x30)` as `map`, the owner setter passes
   `0x233FED8` (decomp of `0x116D6C0`, `0xA020F0`).
6. **Province ID field.** `+0x20` holds the province's own ID: the map click
   formats it as the province argument of a pending console command
   (`0x116D6C0`, through `0x1706270`), `AddCore, %d` logs it (`0xA06100`),
   and the base-tax getter compares it with a country's capital (claim 13).
7. **Two bounds disagree.** `moveunit` (`0xE87A90`) accepts `0 ≤ id < N`
   while the other handlers accept `1 ≤ id ≤ N` (decomp). Both index the same
   array, so it has at least `N + 1` entries with entry 0 a placeholder.
8. **The map size in the game files** (files): `default.map` has
   `max_provinces = 4942`, `definition.csv` has 4942 lines (a header and IDs
   1–4941), and the name key of province 295 is `PROV295` ("Moskva") in
   `prov_names_l_english.yml`.

### Province fields

9. **Owner** `+0x468` and **controller** `+0x470`, both tags (claim 16).
   The controller setter `0xA015C0` stores the new tag at `+0x470`
   (`0xA01ACD`) after copying the old one to `+0x478` (`0xA019D0`) (disasm).
   The owner setter `0xA020F0` reads `+0x468` throughout and calls the
   controller setter; the script getters, triggers and the map click read the
   owner from `+0x468` (decomp, disasm `0x6DE7A3`). `+0x488` is a third tag
   the owner setter reads (meaning not settled).
10. **Cores** are a `std::vector` of tags at `+0x490/+0x498/+0x4A0`: the
    core adder `0xA05770` searches it by the tag's country index and appends
    with `push_back` (disasm `0xA0579E`, decomp).
11. **Development.** The script value table built in `0x6A87D0` maps
    `base_tax`, `base_production`, `base_manpower`, `development`,
    `local_autonomy`, `unrest`, `nationalism` and `province_trade_power` to
    `std::function` objects (strings, vtables `0x1C9F6D0` …). Their call slots
    (`vtable+0x10`) read:
    - `base_tax` `0x6DE7A0`: int32 `+0x3E4`;
    - `base_production` `0x6DE830`: int32 `+0x3E8`;
    - `base_manpower` `0x6DE8C0`: int32 `+0x3E0`;
    each plus, when the owner's `+0x9A78` object is set and the province is the
    owner's capital, that object's `+0x1C`/`+0x20`/`+0x24` rounded down to a
    multiple of 1000 (disasm). `development` `0x6DE6B0` is the sum of the
    three getters `0xA0D6B0`, `0xA0DFF0`, `0xA0DB50`; `0xA0D6B0(province,
    out, with_bonus, tooltip)` returns `+0x3E4` unchanged when `with_bonus` is
    0 (decomp). The tooltip keys are `BASE_TAX`, `BASE_PRODUCTION`,
    `BASE_MANPOWER` (`0xA2C910`).
12. **Static part and name.** `+0x28` points to a per-province static
    object. The province view's name label (`0xE31930`, widget
    `province_name`) is set from `*(+0x28) + 0x10`, passed as a
    `std::string` (disasm `0xE31A26`); the core adder and the wasteland
    tooltip format the same field into text (decomp of `0xA05770`,
    `0xC40100`).
13. **Sea and wasteland.** The trigger `is_sea` (evaluate `0xC464C0`, vtable
    `0x1D05228`, tooltip key `IS_SEA_ZONE`) is true when bit 3 of
    `*(+0x28) + 0x228` is **clear**; `is_wasteland` (evaluate `0xC3FDC0`,
    vtable `0x1CFCA48`, keys `IS_WASTELAND`/`IS_NOT_WASTELAND`) is bit 3 of
    `*(+0x28) + 0x229` (disasm `0xC467D7`, `0xC400D7`). The selection function
    returns without opening the view when that wasteland bit is set
    (`0x139C830`, decomp).
14. **Units present.** `num_of_units_in_province` (evaluate `0xCDC490`)
    walks a singly linked list from `+0x350`: node `+0x00` the unit, `+0x10`
    the next node. A unit has a virtual "counts here" test at vtable `+0x80`,
    its owner tag at `+0xD0` and a vector of sub-unit pointers at
    `+0x30/+0x38`; each sub-unit's type is at `+0x20` with its category at
    type `+0x10` (decomp, disasm `0xCDC7F4`). Units by ID live in a hash map
    at `*0x2340310` (FNV-1a over the 4 ID bytes, buckets `+0x18`, mask
    `+0x30`, end `+0x08`; `moveunit`).
15. **Other fields seen.** `+0x388` a siege object (the selection hands it to
    the siege panel `session+0x11B8`, `0x13ACAE0`, label `label_siegename`);
    `+0x1B0` int, non-zero shows `strait_indicator` in the province view;
    `*(+0x28) + 0x7C` two `int16` map coordinates the camera moves to
    (`0x139C6A0`); `*(+0x28) + 0x168/+0x170` a vector of 36-byte neighbour
    records (decomp of `0x1347A50`, `0xA05770`).

### Tags and countries

16. **Tag value.** `0x286120(db, &out, text, case_sensitive)` resolves a tag
    string: it sums the first three characters (upper-cased), takes the sum
    modulo 64 as a bucket, walks the bucket's array of 8-byte values at
    `db + 0x148 + bucket*0x18` (count int32 at `+0x154`) and compares three
    characters with `_strnicmp`; it returns the stored 8-byte value, or
    `0x002D2D2D` ("`---`", byte 7 zero) when nothing matches (decomp, disasm
    `0x2861BB`). Readers take the country index from bytes 4–5
    (`(int16)(tag >> 32)`), test byte 7 for "valid" and byte 6 for the kind
    (`AddAsCore` refuses kind 7, `0xA06100`), and the map click turns a tag
    into text by `strlen` on its first bytes (`0x116D6C0`).
17. **Country object** = `(*(0x233D8D0 + 0x118))[index]`, 8-byte pointers
    (decomp of `0xE75780`, `0xE42FD0`, `0x8740B0`, disasm `0x6DE7B9`). Fields:
    - `+0x20` the country's own tag (the AI loop compares `+0x26` with 7,
      `0x75B4B0`);
    - `+0x14B8` int32 capital province ID: `add_core` without arguments uses
      the player's `+0x14B8` as the province (`0xE73C9E`), and the base-tax
      bonus applies only when it equals the province's `+0x20`;
    - `+0x1338` pointer to int32 province IDs with count `+0x1344`: `tag`
      refuses to switch to a country whose `+0x1344` is not positive, answering
      `<tag> has no provinces, switch would resign.` (`0x779330`), and the AI
      task loop skips countries with `+0x1344 == 0` unless their kind is 7;
    - `+0x2680` non-null marks a custom name: `0x2892A0` (tag → display text)
      then returns `+0x2690`; otherwise it goes through the database
      (`0x286550`) (decomp);
    - `+0x28A3` byte set by the pre-command hook (`research/console-commands`
      H3).
18. **The tag argument parser** `0xE42B00(args, i)` returns the parsed tag of
    `args[i]` when it is valid and otherwise the player tag (claim 19), so
    console commands default to the player (decomp).

### The player

19. **Player tag** `root + 0x1E60`, **observed tag** `root + 0x1E68`. Every
    reader found (`0x8740B0`, `0xE42B00`, `0xE73C20`, `0x779330`, `0x75BBE0`,
    `0x9F52E0`, `0xA020F0`, `0x116D6C0`) computes "the player's country" as:
    `root+0x1E68` if `root+0x1E66 == 7` and `root+0x1E6F != 0`, else
    `root+0x1E60` (decomp; disasm `0x874133`). `0x1E66` is the kind byte of the
    player tag and `0x1E6F` the valid byte of the observed tag.
20. **The observed tag is written by `0x763A20(root, tag)`**, which the map
    click (`0x116D6C0`) and the province click (`0x139C070`) call with the
    clicked province's owner when the player kind is 7, and which re-applies
    the observe mode through the session (`0x80C440`). `observe_mode <n>`
    (`0xE86140`) passes `root+0x1E60` to the same session call and answers
    `Observe mode changed` (decomp).

### Selection and the province view

21. **Selection holder.** The province view's sub-window calls
    `0x139C830(*(session + 0x1220), province)` (`0xE31D13`), and Etch's notes
    name `+0x1220` of the in-game idler as the holder. In `0x139C830`:
    `holder[1]` is the session-like owner (it reads `+0x1440`, `+0x2C`,
    `+0x5BA0`, `+0x11B8`, `+0x1228`, `+0x368`, the same offsets `0x763A20`
    uses on `*(root+0x1E00)`); it stores the province in **`holder[0]`**
    unless `holder[1]+0x2C == 0x25` (then it stores null) (`0x139C870`);
    it returns early when the province's `+0x20` is 0, and then shows it in
    `holder[2]` through `0x1347A50(view, province, 1)` or, when `holder[3]` is
    set, a second path (decomp, disasm).
22. **The province view keeps the province it shows at `view + 0x958`.**
    `0x1347A50` returns when the province is null or equal to `view+0x958`,
    otherwise refreshes the view, stores the province there and passes it to
    its sub-windows (the sub-window `0xE31930` keeps its own copy at `+0x560`)
    (decomp, disasm `0x1347A89`).
23. **Map pick.** `0x10A3DD0(app+0x360 object, &xy)` turns map coordinates
    into a province ID or −1; the map click then fetches the province with
    `0x9AA80` (`0x116D6C0`; 9 callers by xrefs).

### What the tick writes

24. **The tick runs on the main thread inside an outer frame.** The lab's
    profile (`docs/simulation-profile.md`) places the daily tick `0x758430`
    under `0x14C25A0 → 0x8137C0 → 0x20F810 → … → 0xB79520`; nested frames
    `0x769EA0` are drawn from inside the tick stages. Tongs' dispatcher runs
    jobs after outer frames only (`docs/design.md`, "Dispatcher").
25. **Province day** (`0x75B2D0` walks the pointer array `root+0x1CD8`, count
    `root+0x1CE4`, calling `0x9F52E0` per province): writes province fields
    `+0xC0`, `+0x330`, `+0x334`, `+0x360`, `+0x388`, `+0x404`, `+0x46F`,
    `+0x4D8`, `+0x4E0`–`+0x4EC`, `+0x588`, `+0x7F8`–`+0x820`, `+0x864`,
    `+0x87C`, `+0x8B8`, `+0x8E0`–`+0x8F4`, `+0x958`, `+0x2DD8`, and calls the
    controller setter `0xA015C0` (decomp). Monthly: `0x75D690` calls
    `0x9F35C0` for every entry of `root+0x1C90`.

## Hypotheses (need a live check)

- **H1 – one array.** `*(root + 0x1CA8) == *(0x233FED8 + 0x6F0)`: the root
  and the map name the same province array (same stride, both used for
  "province by ID"). Not shown: no write to `root+0x1CA8` was traced.
- **H2 – size.** `N` (claim 3) is 4941 and the array holds 4942 entries
  (`max_provinces`), entry 0 a placeholder whose `+0x20` is 0 and whose owner
  is `---`. Under this, valid IDs are 1–4941 and IDs past the definitions
  never exist.
- **H3 – development scale.** `+0x3E0/+0x3E4/+0x3E8` are fixed-point values
  × 1000 (the rounding to multiples of 1000 in claim 11); `base_tax 3` reads
  3000.
- **H4 – name.** `*(+0x28)+0x10` holds the localised name ("Moskva"), loaded
  from the key `PROV<id>`; the key itself is not stored there.
- **H5 – observer.** Kind 7 is the observer pseudo-country; in a normal game
  `root+0x1E66` is not 7, so the player is `root+0x1E60`.
- **H6 – session.** `holder[1]` is the session `*(root+0x1E00)` (the in-game
  idler), so the holder is `*(*(root+0x1E00) + 0x1220)` in game. In the main
  menu the session has another class and `+0x1220` means something else.
- **H7 – exists.** `country+0x1344 > 0` is the game's own "exists" (owns at
  least one province); the observer and the dummy `---` have 0.
- **H8 – no writer off the main thread.** Province and country fields are
  not written by worker threads after the tick has joined them. The AI tasks
  run on the pools during the tick (`0x77D8A0`) and the main thread waits for
  them; nothing static excludes a background writer elsewhere.
- **H9 – the static part is constant.** `*(+0x28)` and its name, flags and
  position do not change after the map is loaded.
- **H10 – the array is stable while a save is loaded.** The array is
  allocated when the map loads and not reallocated by a save load; pointers
  into it stay valid between frames. Reads should still re-derive everything
  from the root on each job, as `state` does.

## Proposed read recipe

On the main thread (a main-thread read), with `in_game: true`:

1. `root = *0x233FE78`; `N = (root[0x1C98] − root[0x1C90]) / 8`; refuse an ID
   outside `1 … N` with Tongs' own error. Never call `0x9AA80` to validate: it
   writes `error.log` and returns entry 0.
2. `p = root[0x1CA8] + id * 0x2E10`; check `p[0x20] == id` (a cheap guard
   that also catches a wrong layout).
3. Province: owner `p[0x468]`, controller `p[0x470]`, cores
   `p[0x490]..p[0x498]` (8-byte tags), base tax/production/manpower
   `p[0x3E4]`/`p[0x3E8]`/`p[0x3E0]` (int32, raw), units from `p[0x350]`
   (bounded walk), static `s = p[0x28]`: name `s+0x10` (MSVC string),
   sea `!(s[0x228] & 8)`, wasteland `s[0x229] & 8`.
4. Tags: answer the three letters and test byte 7; to resolve a client's tag,
   walk the buckets at `0x233D8D0 + 0x148` in Tongs (the same hash as
   `0x286120`) instead of calling it, then
   `c = (*(0x233D8D0 + 0x118))[(int16)(tag >> 32)]`; check `c[0x20]` equals
   the tag. Country: capital `c[0x14B8]`, province count `c[0x1344]`.
5. Player: `root[0x1E66] == 7 && root[0x1E6F] ? root[0x1E68] : root[0x1E60]`,
   and answer both raw tags so a client can tell observe mode.
6. Selected province: `*(*(root+0x1E00) + 0x1220)` → `holder[0]`; the
   province view's displayed province `holder[2] + 0x958`. Answer IDs (`+0x20`)
   rather than pointers.

Localised country names need the game's functions (`0x2892A0` may allocate
and caches results); keep them out of the first version or make them a
separate main-thread call after a live check.

## Candidate symbols for `plugin/symbols.toml`

In the `[symbol.*]` form that replaced `[[expect]]` (bytes from the listing of
this build; the `rip_relative` targets were checked by hand: `0x233FE78`,
`0x233D8D0`).

```toml
# --- World reads (docs/research/world-objects.md) ---

[symbol.province_count]
rva = 0xE8674D
bytes = "48 8B 05 24 97 4B 01 48 8B 88 98 1C 00 00 48 2B 88 90 1C 00 00 48 C1 F9 03"
memory = "code"
values.root = { at = 3, rip_relative = 7 }
values.provinces_end = { at = 10, size = 4 }
values.provinces = { at = 17, size = 4 }
note = "debug_name: a province ID is valid for 1..(root+0x1C98 - root+0x1C90)/8"

[symbol.province_is_sea]
rva = 0xC467BD
bytes = "48 69 D0 10 2E 00 00 48 8B 05 AD 96 6F 01 48 8B 88 A8 1C 00 00 48 8B 44 0A 28 0F B6 88 28 02 00 00 C0 E9 03 F6 D1 80 E1 01"
memory = "code"
values.province_size = { at = 3, size = 4 }
values.root = { at = 10, rip_relative = 14 }
values.province_array = { at = 17, size = 4 }
values.static = { at = 25, size = 1 }
values.land_flags = { at = 29, size = 4 }
values.land_bit = { at = 35, size = 1 }
note = "is_sea trigger: province = root+0x1CA8 + id*0x2E10; sea when bit 3 of static+0x228 is clear"

[symbol.province_is_wasteland]
rva = 0xC400BD
bytes = "48 69 D0 10 2E 00 00 48 8B 05 AD FD 6F 01 48 8B 88 A8 1C 00 00 48 8B 44 0A 28 0F B6 88 29 02 00 00 C0 E9 03 80 E1 01"
memory = "code"
values.wasteland_flags = { at = 29, size = 4 }
values.wasteland_bit = { at = 35, size = 1 }
note = "is_wasteland trigger: bit 3 of static+0x229"

[symbol.map_province_getter]
rva = 0x9AAA7
bytes = "48 8B 89 F0 06 00 00 48 8B 86 F8 06 00 00 48 2B C1 48 C1 F8 04 48 BA 21 C1 54 8C EC 37 3F 8A 48 0F AF C2 48 3B F8 73 0F 48 69 C7 10 2E 00 00"
memory = "code"
values.map_provinces = { at = 3, size = 4 }
values.province_size = { at = 43, size = 4 }
note = "map.h getter 0x9AA80: out-of-range IDs log an error and return entry 0; do not call it to validate"

[symbol.province_base_tax]
rva = 0x6DE7A0
bytes = """
4C 8B CA 49 8B 88 68 04 00 00 48 8B 05 1F F1 C5 01 48 C1 E9 20 48 0F BF C9
48 8B 80 18 01 00 00 48 8B 14 C8 48 8B 8A 78 9A 00 00 48 85 C9 74 26
41 8B 40 20 39 82 B8 14 00 00 75 1A B8 D3 4D 62 10 F7 69 1C C1 FA 06 8B C2
C1 E8 1F 03 D0 69 C2 E8 03 00 00 EB 02 33 C0 41 8B 90 E4 03 00 00
"""
memory = "code"
values.owner = { at = 6, size = 4 }
values.countries_db = { at = 13, rip_relative = 17 }
values.countries = { at = 28, size = 4 }
values.province_id = { at = 51, size = 1 }
values.capital = { at = 54, size = 4 }
values.base_tax = { at = 91, size = 4 }
note = "script value base_tax: owner tag +0x468, country = db+0x118[index], capital +0x14B8, base tax +0x3E4"

[symbol.province_base_production]
rva = 0x6DE86C
bytes = "B8 D3 4D 62 10 F7 69 20 C1 FA 06 8B C2 C1 E8 1F 03 D0 69 C2 E8 03 00 00 EB 02 33 C0 41 8B 90 E8 03 00 00"
memory = "code"
values.base_production = { at = 31, size = 4 }
note = "script value base_production: +0x3E8"

[symbol.province_base_manpower]
rva = 0x6DE8FC
bytes = "B8 D3 4D 62 10 F7 69 24 C1 FA 06 8B C2 C1 E8 1F 03 D0 69 C2 E8 03 00 00 EB 02 33 C0 41 8B 90 E0 03 00 00"
memory = "code"
values.base_manpower = { at = 31, size = 4 }
note = "script value base_manpower: +0x3E0"

[symbol.province_controller]
rva = 0xA01AC6
bytes = "48 8B 85 B8 01 00 00 48 89 86 70 04 00 00"
memory = "code"
values.controller = { at = 10, size = 4 }
note = "controller setter 0xA015C0 stores the new controller tag"

[symbol.province_cores]
rva = 0xA0579E
bytes = "48 8D B1 90 04 00 00 48 8B 46 08 4C 8B 16 4C 3B D0 74 28 48 C1 EA 20"
memory = "code"
values.cores = { at = 3, size = 4 }
note = "core adder 0xA05770: std::vector of tags at +0x490"

[symbol.province_units]
rva = 0xCDC7D8
bytes = "48 69 D8 10 2E 00 00 48 8B 05 92 36 66 01 48 03 98 A8 1C 00 00 48 89 9D D8 00 00 00 48 8B BB 50 03 00 00 48 85 FF 0F 84 61 01 00 00 0F 1F 40 00 0F 1F 84 00 00 00 00 00 4C 8B 3F 48 8B 7F 10"
memory = "code"
values.units = { at = 31, size = 4 }
values.next = { at = 62, size = 1 }
note = "num_of_units_in_province: list of {unit, ?, next} nodes from +0x350"

[symbol.province_name]
rva = 0xE31A26
bytes = "48 8B 8F 60 05 00 00 45 33 C0 48 8B 51 28 48 8B C8 48 83 C2 10"
memory = "code"
values.static = { at = 13, size = 1 }
values.name = { at = 20, size = 1 }
note = "province view label province_name: std::string at static(+0x28)+0x10"

[symbol.player_country]
rva = 0x874133
bytes = """
80 B8 66 1E 00 00 07 C6 05 97 E2 74 01 00 75 1B 80 B8 6F 1E 00 00 00 74 12
48 8B 88 68 1E 00 00 EB 10 48 8B 05 1C BD AC 01 EB D5 48 8B 88 60 1E 00 00
48 8B 05 64 97 AC 01 48 C1 E9 20 48 0F BF D1 48 8B 88 18 01 00 00 48 8B 04 D1
"""
memory = "code"
values.player_kind = { at = 2, size = 4 }
values.observer_kind = { at = 6, size = 1 }
values.observed_valid = { at = 18, size = 4 }
values.observed = { at = 28, size = 4 }
values.player = { at = 46, size = 4 }
values.countries_db = { at = 53, rip_relative = 57 }
values.countries = { at = 68, size = 4 }
note = "pre-command hook: player = root+0x1E68 if kind(root+0x1E60) == 7 and root+0x1E6F, else root+0x1E60"

[symbol.tag_buckets]
rva = 0x2861BB
bytes = "48 8D 0C 40 4D 8D 3C CA 4D 63 B7 54 01 00 00 4D 85 F6 7E 45 8B DD 49 8B 87 48 01 00 00"
memory = "code"
values.bucket_count = { at = 11, size = 4 }
values.buckets = { at = 25, size = 4 }
note = "tag lookup 0x286120: 64 buckets of 0x18 bytes, values at +0x148, count at +0x154"

[symbol.country_provinces]
rva = 0x779429
bytes = "48 8B B1 38 13 00 00 48 63 81 44 13 00 00 4C 8D 24 86"
memory = "code"
values.owned = { at = 3, size = 4 }
values.owned_count = { at = 10, size = 4 }
note = "tag switch: the country's province IDs at +0x1338, count +0x1344 (0 = no provinces)"

[symbol.selected_province]
rva = 0x139C860
bytes = "41 83 79 2C 25 48 89 74 24 38 8B F5 49 0F 45 F2 48 89 33 49 8B 89 A0 5B 00 00 E8 41 82 0D 00 48 85 F6 0F 84 8F 02 00 00 39 6E 20"
memory = "code"
values.province_id = { at = 42, size = 1 }
note = "province selection 0x139C830: holder[0] = province unless owner+0x2C == 0x25"

[symbol.selection_holder]
rva = 0xE31D05
bytes = "48 8B 86 50 05 00 00 48 8B 96 60 05 00 00 48 8B 88 20 12 00 00 E8 11 AB 56 00"
memory = "code"
values.holder = { at = 17, size = 4 }
note = "province view: select(*(session+0x1220), province)"

[symbol.province_view_province]
rva = 0x1347A89
bytes = "48 39 91 58 09 00 00 0F 84 3E 01 00 00"
memory = "code"
values.shown = { at = 3, size = 4 }
note = "province view 0x1347A50: the displayed province at +0x958"
```

## Live checks a later `task` ticket should run

All read-only, on the lab copy with the Russia save of 17 March 1719, under
the lab's game lock.

1. **Array (H1, H2).** Read `N`, `root+0x1CA8`, `0x233FED8+0x6F0/+0x6F8`:
   expect equal bases, `N = 4941`, array length 4942; entry 0 has `+0x20 == 0`
   and owner `---`; for a sample of IDs (1, 295, 4941) `+0x20 == id`.
2. **Fields against the save.** Province 295 (Moskva): owner and controller
   `RUS`, cores contain `RUS`, base tax/production/manpower × 1000 equal the
   save's values (H3), name `Moskva` (H4), sea bit set (land), wasteland bit
   clear. A sea province (e.g. 1252), a wasteland province and a lake: the
   flags.
3. **Countries.** Resolve `RUS` through the buckets; its `+0x20` is the tag,
   `+0x14B8 == 295`, `+0x1344` equals the province count in the save. A
   dead tag has `+0x1344 == 0` (H7); `---` resolves to byte 7 = 0.
4. **Player (H5).** `root+0x1E60` is `RUS`, kind byte not 7; after
   `observe_mode` (a marked run with `game_calls`) the kind is 7 and the map
   click changes `root+0x1E68`.
5. **Selection (H6).** Open a province view by clicking a province: `holder[0]`
   and `view+0x958` point at it; close the view and read both again (which one
   is cleared is not known). Check `holder[1] == *(root+0x1E00)`.
6. **Units.** At a province with an army, walk `+0x350` and compare the count
   with the province's map display.
7. **Stability (H8–H10).** At speed 5, read the same province fields in every
   outer frame for a month and log changes per day: owner/controller/siege
   should change only between days; repeat after a save reload and compare the
   array pointer.

## Open questions

- **Selected army and selected country.** Not found statically. Leads: the
  province click with a siege hands the siege to the session's siege panel
  (`session+0x11B8`); the unit panel and the country (diplomacy) view are
  other windows of the same session. Needs its own search (the army ticket in
  "Not yet specified" of the map).
- **Localised names.** Is `*(+0x28)+0x10` the localised text (H4)? Where is a
  country's localised name kept when there is no custom name (`0x2892A0`
  takes several paths, and `0x286550` and `+0x2650` are not read)?
- **What owns `root+0x1CA8`?** Where is it written, and is it updated when the
  map is rebuilt (`reload_provincemap`, `0xE53680`)?
- **Which code calls the owner setter `0xA020F0`** (peace deals, events,
  annexation)? All seen paths are on the main thread, but the list was not
  enumerated.
- **Meaning of the other tag fields:** province `+0x478` (previous
  controller?), `+0x488`, and the special country indices `0x233D8A4`,
  `0x233D8D8`, `0x233D8DC` that the core adder refuses (rebels, natives,
  pirates?).
- **Country count.** The vector `0x233D8D0+0x118` has no end pointer at
  `+0x120` in any reader seen; `0x2892A0` compares an index with
  `db+0x100`. The safe bound for a direct index read is not settled; going
  through the buckets avoids it.
