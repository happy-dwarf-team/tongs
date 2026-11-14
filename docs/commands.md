# Tongs commands

The full reference of the pipe protocol: every command and its answer, widget selectors, events, notifications and console commands. Installing, the settings and the safety rules are in the [README](../README.md).

## Commands

The pipe is `\\.\pipe\tongs`. A request is one line of text; the answer is
one JSON object on one line. Up to 8 clients can be connected at once, so one
can wait in `run_until` while another reads `state` or sends `cancel`, and a
connection can instead carry notifications (see [Notifications](#notifications)).

| Command | Answer |
| --- | --- |
| `ping` | plugin version, game process id (`pid`) and its creation time (`process_start_ms`, Unix ms) |
| `state` | `in_game`; with a loaded game also `date` (`"1719.3.17"`), `date_raw`, `hour`, `speed` (1–5 as in the UI), `speed_internal` (0–4), `paused` (the game's own answer), `paused_flag` (the player pause byte), `out_of_sync` (this client's checksums differed from the host's; the host never has it), `paused_oos` (paused because a machine went out of sync), `map_mode` (the map mode shown, a value of the game's map mode enum) and `map_mode_id` (the id of the plugin mode that borrowed that number, from Anvil's `anvil-map`, or `null`; see [Map mode](design.md#map-mode)), `session` (see [Sessions](#sessions-and-the-ready-wait)), and `game_calls_total` and `client_calls_total` (game calls and client calls since the save was loaded); always `pid` and `process_start_ms`, which every answer that carries the state repeats, so a client can tell a restarted game apart |
| `window` | the game window: client `width` / `height`, `screen_x` / `screen_y`, `focused`, `minimized`, `thread_id` (the game's main thread), `dpi` |
| `threads` | worker threads in each of the game's two pools, the `threads` option, process and main-thread CPU masks, ideal CPU and priority of the main thread, number of threads |
| `screenshot [NAME]` | writes a PNG of the game window to `anvil/data/tongs/screenshots/` (keeps the newest 100) and answers with its `path`, size and capture `method`; works while the window is covered |
| `frames [timeout=S]` | read on the game's main thread after a frame of its main loop: frames per second (`outer_per_s`, and `nested_per_s` for frames drawn inside a game update or a save load), counts and the longest gap between main-loop frames since the previous `frames`, and the main thread's `thread_id`; with `queued_ms`, `run_ms`, the game's `frame` counter and the fields of `state` read on that frame, so its `session` says which session the frame belongs to. Waits up to 10 s (`timeout=` up to 600) for a frame; while a save loads none comes, and the answer is `main thread busy` |
| `ui find SEL [all] [stale] [limit=N]` | widgets that match the selector `SEL` (see [Widgets](#widgets)): `count`, `truncated` and up to 50 `matches` (`limit=` up to 5000), each with `path`, `type`, `visible`, `rect` and `click_point` in client pixels, and `text` / `enabled` / `stale` where they apply; `ui_scale` is the game's GUI scale (`game_ui_scale`) the rects were converted by. Read on the main thread, like `frames` |
| `ui tree [SEL] [depth=N] [limit=N] [all]` | the widget tree from the top-level windows, or from the matches of `SEL`, as nested `widgets` (up to 200, `limit=` up to 5000); `depth=N` gives deeper levels only as `children_count` |
| `event list [timeout=S]` | the open events, topmost first (see [Events](#events)), read on the main thread |
| `province ID [timeout=S]` | one province by ID: name, owner, controller, cores, development, land and wasteland, its flags and variables (see [World reads](#world-reads)) |
| `country TAG [timeout=S]` | one country by tag: whether it exists, its provinces, its capital, its flags and variables, the events waiting for its choice and its diplomacy |
| `player [timeout=S]` | the player's country |
| `selection [timeout=S]` | the last selected province and whether a province view shows one |
| `units [TAG] [timeout=S]` | a country's armies and navies (the player's without `TAG`), through Anvil's `anvil-game.world` (see [Units](#units)) |
| `cursor [timeout=S]` | what the game has under its pointer: the widget, the province and the army or navy, from the game's own state (see [Cursor](#cursor)) |
| `wait_until DATE [timeout=S] [max_pause=S]` | waits (without touching the game) until the date reaches `DATE` (`1720.1.1` or `1720.1.1.12`); answers with the timing below |
| `cancel` | ends every `wait_until` / `run_until` in progress and withdraws main-thread jobs still waiting for a frame (`withdrawn`); a client that disconnects ends its own wait the same way (`client_left`) |
| `subscribe KINDS [date=day\|month\|year]` | turns the connection into a stream of notifications (see [Notifications](#notifications)); `unsubscribe` ends it |
| `unsubscribe` | on a subscribed connection: ends the notifications and answers how many were `sent`, `dropped` and `discarded`; elsewhere `not subscribed` |
| `journal` | the command journal of this game start: `enabled`, its `path`, `lines` and `truncated` (see [Journal](#journal)) |
| `help` | list of commands (`commands`, the first words), whether input commands are enabled and whether Anvil suspended Tongs (`suspended`, see [Safety](../README.md#safety)), and `types`: the [command type](../GLOSSARY.md) of every command by its full name (`"province open":"client_call"`): `read`, `input`, `client_call` or `game_call`, as in [commands.json](commands.json) |

With `input = true` in the settings, also:

| Command | What it does |
| --- | --- |
| `pause` | presses the top bar's pause button unless the player pause is already on, then waits until it is |
| `resume` | presses the pause button if the player pause is on, then waits until the game runs; refuses when something else holds the game paused |
| `speed N` | presses the speed up / down button one step at a time until the speed is `N` (1–5, as in the UI); works while paused |
| `run_until DATE [speed=N] [timeout=S] [max_pause=S] [events=first\|stop]` | sets the speed, resumes, waits for the date and pauses again; for measuring how fast the game runs. `events=first` chooses the first enabled option of every event that opens on the way |
| `key [MOD+]NAME` | presses one key: `a`–`z`, `0`–`9`, `f1`–`f12`, `space`, `return`, `escape`, `tab`, `backspace`, `delete`, `minus`, `equals`, `grave` (the console key on a US layout), `kp_plus`, `kp_minus`, `kp_enter`, arrows (`up`, `down`, `left`, `right`), `home`, `end`, `page_up`, `page_down`, and the left `shift`, `ctrl`, `alt` alone; with modifiers held while it is pressed, `MOD+` once each of `shift`, `ctrl` (or `control`), `alt`, in any order: `shift+left`, `ctrl+a`, `ctrl+shift+end` (see [Keys](#keys)) |
| `text STRING` | types the rest of the line into the focused text field (characters U+0020–U+007E and U+00A0–U+00FF); with no text field focused it sends nothing and answers `no_text_field` |
| `move X Y` | points the mouse at client coordinates `X Y` (hovering shows tooltips) |
| `click X Y [left\|right\|middle] [double]` | moves there and clicks |
| `scroll STEPS [X Y]` | turns the wheel over the list or map under the pointer; positive scrolls up (zooms in on the map), negative down. With `X Y` it moves there first and lets the game see the pointer, as `click` does |
| `release` | hands the pointer back to the real mouse |
| `ui click SEL [timeout=S] [wait=S]` | clicks the centre of the one widget the selector matches; `wait=` waits for it to show (see [Widgets](#widgets)) |
| `event choose N\|text^="..." [event=N\|event=title^="..."] [timeout=S]` | clicks an option of an open event (see [Events](#events)) |
| `province open ID [timeout=S]` | opens the province's view and moves the camera to it, as the game's links do (see [Navigation](#navigation)) |
| `province goto ID [timeout=S]` | moves the camera to the province |
| `unit drill start\|stop UNITS [timeout=S]` | starts or stops drilling the armies, as the unit panel's button does (see [Units](#units)) |
| `unit split UNIT [timeout=S]` | splits one army or navy in half, rounded up |
| `unit merge UNITS [timeout=S]` | merges armies, or navies, in one province into the first |
| `map_mode MODE [timeout=S]` | a client call: shows a map mode as the minimap's button does. `MODE` is a mode of the game's tabs by name (`political`, `religion`, `trade_goods`: the game's enum names in lower case) or number, or a plugin mode by its id (`player-cores.cores`, with `anvil-map`); answers its `number`, `client_calls_total` and the state read after it |

With Tongs' **Game calls** module on (`enabled_modules = ["tongs.game_calls"]` in `anvil/anvil.toml`, or its switch in Anvil Manager), also (see [Console](#console)):

| Command | What it does |
| --- | --- |
| `console LINE` | runs one console command line in the game, on its main thread, and answers with the game's text and the state afterwards. **Changes game state** |
| `save load NAME [timeout=S]` | loads a save from `save games` in the running game, as the Load dialog does, or from the main menu, as its Continue does (see [Loading a save](#loading-a-save)). **Discards the running game without saving** |

Input commands answer once the game has reacted (or after 10 s) with
`command`, `presses`, `waited_ms` and the fields of `state`. A pause (also
the one that ends `run_until`) answers once the date has stayed the same for
three frames: the day the game is in when the pause takes hold still ends.
When the game does not react in time, `ok` is `false` with an `error`, and the state is
still included.

`pause`, `resume` and `speed` (also inside `run_until`) press the top bar's
buttons by calling, on the main thread after an outer frame, what each
button's handler calls ([ADR 0005](adr/0005-speed-buttons.md)); `via` in the
answer is `"button"`. In single player that is what Space and keypad `+` /
`-` do too. In multiplayer the game's code posts the same pause and speed
commands as a player's click, while Space does not pause there with more than
one player (read from the code; not yet checked with a second player, see
[design.md](design.md#speed-buttons)). Each press counts as an input, like a
key. While the game ignores its buttons (the byte the buttons check is set)
nothing is pressed and the command answers an error. Without the main thread (no `anvil-game` frames) the commands press
the keys instead, and `via` is `"key"`. `key`, `text` and the mouse commands do not expect a change;
they answer with the state read 0.1 s after the input. Input commands from
different clients take turns. While the game window is minimized the game
leaves its input queued until the window is restored, and acts on it then:
`key`, `text`, the mouse commands, `ui click` and `event choose` (also the
choices of `run_until events=first`) send nothing and answer `the game window
is minimized: …`, while `pause`, `resume` and `speed` work through the buttons.

`wait_until` and `run_until` report `start_date`, `reached` and
`reached_date`, `game_days`, `wall_ms` (from the start of the wait until the
target date was first seen), `days_per_second`, `paused_ms` (time seen paused
while waiting, e.g. an event that pauses the game; with `paused_date`, the
date of the last pause), `first_change_ms`, `inputs` (input commands any
client sent during the wait; `run_until` does not count its own presses
before and after it), `client_calls` (client calls any client made during the
wait, [ADR 0003](adr/0003-client-calls.md): `province open`, `province
goto` and `map_mode`, see [Navigation](#navigation)) and
`game_calls` (console commands and save loads any client made during the
wait); `help` names the type of every command (`types`), so a
measurement can reject a run with any of them above 0, and treat a missing one
(an older Tongs) as unknown;
`run_until` also `start_speed` and `overshoot_hours` (game hours past the
target when the pause took hold, usually 0). Both wait at most 1 hour by
default (`timeout=` up to 14 400 s). `max_pause=S` ends the wait with an error
once the game has stayed paused for `S` seconds, e.g. on an event with a
choice, instead of waiting for the timeout. A wait ends with an error when the
game leaves the session: back to the menu, or a load (the `session` changes).
It also ends when the client that sent it disconnects: `error` `client_left`,
the same end as a `cancel`, so `run_until` pauses the game again. Nobody reads
that answer; the [journal](#journal) has it. Before 0.34, a wait outlived its
client: the game kept running until a `cancel`.

The mouse commands also make the game's interface see the pointer at the given
point (a hook on the game's cursor query), until `release` or until the real
mouse moves; the real cursor itself does not move.

```powershell
.\scripts\tongs.ps1 state
# {"ok":true,"in_game":true,"date":"1719.3.17","date_raw":58860240,"hour":0,"speed":2,"speed_internal":1,"paused":true,"paused_flag":true,"out_of_sync":false,"paused_oos":false,"map_mode":1,"map_mode_id":null,"session":1,"pid":21384,"process_start_ms":1792418572041,"game_calls_total":0,"client_calls_total":0}
.\scripts\tongs.ps1 'run_until 1719.6.1 speed=5'
# {"ok":true,"command":"run_until","target":"1719.6.1","start_date":"1719.3.17","reached":true,"reached_date":"1719.6.1",
#  "game_days":76.000,"days_per_second":12.352,"wall_ms":6153,"paused_ms":0,...,"overshoot_hours":0,...,"paused":true,...}
.\scripts\tongs.ps1 'click 3350 32' screenshot
```

Right after a save loads, the game handles input only once an outer frame of
the new session has run; the commands wait for it, and the
[ready wait](#sessions-and-the-ready-wait) waits for that frame first. While
the Esc menu is open, Space does not resume the game.

## Keys

`key` sends what the player's keyboard sends, in the game's own SDL queue:
the key down, its character, the key up. The key code (`sym`) and the
physical key (`scancode`) are those of a US layout, whatever layout the game
runs with: shortcuts follow the key code, and `key z` is the `z` shortcut and
types `z` on a German or Polish (214) layout too, where the key in that place
is `y`. A key's character is the one it has without modifiers; `text` types
any character from U+0020–U+007E and U+00A0–U+00FF, whatever the layout, so
`key a` and `text a` put the same `a` into a field.

With modifiers (`key shift+left`), the modifier keys go down first (Ctrl,
Alt, Shift), then the key, then the modifier keys come up in reverse order;
each event carries the modifiers held at that moment. The game keeps its own
record of the keys held from these events and asks it for Shift, Ctrl and Alt,
so a chord does what the player's does. Shift alone with a letter types the
capital letter; other chords type nothing (a shifted digit or sign depends on
the layout: use `text`).

`text` types only into a text field with the keyboard focus. With none (no
console open, no field clicked) it sends nothing and answers `no_text_field`.
The check is the game's own text input target, which a field takes when it
gets the focus. It is not always cleared when a window closes: after the save
dialog was closed with Esc the target stayed on its hidden name field, and
`text` was accepted again (and typed into that hidden field). So
`no_text_field` is certain, but an `ok` does not prove that a visible field
took the characters; check with `ui find` or a `screenshot`.

In a text field (an edit box with the focus: the find box, the console, the
year in the lobby) the keys do what the game's own text fields do for a
player (see [design.md](design.md#key-chords)):

- `left` and `right` move the cursor, `backspace` and `delete` delete a
  character, `ctrl+backspace` the word before the cursor, and a `click` in
  the field puts the cursor there;
- `home` and `end` select from the cursor to the start or the end of the
  text, with Shift or without, so the next character typed replaces that
  part;
- the game has no other selection: `shift+left`, `shift+right` and `ctrl+a`
  do nothing in its fields;
- `ctrl+c`, `ctrl+x` and `ctrl+v` are the game's copy, cut and paste (read
  from its code, not tried: they use the Windows clipboard, which is the
  player's).

## Sessions and the ready wait

`session` numbers the games of one game process: 1 for the first game after
the start (from the main menu or with `continue=`), +1 at every load, by the
player or by `save load`. A client pins it like `pid`: a changed
`session` means the game state was replaced, even when the date comes back
the same. Tongs reads the state every 20 ms to see every load. `state`
answers it and every answer that carries the state repeats it; outside a game
it is absent. The `state` notification `entered_game` carries it too, with
`loaded_by` (`"tongs"` after `save load`, else `null`). A load starts
`game_calls_total` and `client_calls_total` again (`game_calls_total` is 1
after `save load`) and ends a
`wait_until` or `run_until` in progress.

The ready wait replaces a fixed sleep after a start or a load. A client waits,
polling, until all of these hold at once:

1. the pipe answers from the expected process (`pid`, `process_start_ms`);
2. `state` answers `in_game: true` with the expected `session`;
3. a `frames` answer has come whose state shows that session, so an outer frame
   of it has run (after a start, `state` says `in_game` several seconds before
   the first outer frame, and input sent in between is handled late);
4. `paused: true` (after a start with `continue=`, the game pauses itself
   about a second after entering).

| After | `session` | Timeout |
| --- | --- | --- |
| a start with `continue=` | 1 | 180 s |
| a load | the one after the last seen | 30 s |
| a start to the main menu | none: `in_game: false` and a `frames` answer instead | 60 s |

It fails at once when the process is another one, when the `session` is past
the expected one (`unexpected_session`), or when the game leaves the session
after reaching it (`left_game`); on a timeout it names the first condition not
met (`pipe`, `in_game`, `session`, `frames`, `paused`, `speed`). The ready wait
promises an outer frame, not handled input: to prove input, set the speed
afterwards with `speed N`, whose confirmed change and `waited_ms` show the
game handled the press (when the speed already is `N` nothing is pressed).
In the game, the first `speed` after the ready wait took 176 ms after a start
and 31 ms after a load. A load by the player from the game's Load dialog
leaves the in-game menu open, and the speed keys do nothing while it is: close
it first (`ui click menu/menu_panel/menu_close_button`). `save load` leaves no
menu. The ready wait does not look at windows.
The Python client and the MCP server implement it as `ready()` and
`tongs_ready` (see [Clients](../README.md#clients)).

## World reads

`province ID`, `country TAG`, `player` and `selection` read the game's world
on the main thread, after an outer frame and between two game days, like
`ui find`. They need only `enabled`, are not counted (no `inputs`,
`client_calls` or `game_calls`, so they never mark a run), and also run
during another connection's `run_until`, in ironman and multiplayer, and
while Anvil has suspended Tongs. Each takes `timeout=S` (default 10 s, up to
60) for the wait for a frame and answers one object. Every answer, refusals
included, carries the fields of `state` read on the same frame, and
`queued_ms`, `run_ms` and `frame` once the read ran.

| Command | Answer |
| --- | --- |
| `province ID` | `id`, `name`, `owner`, `controller` (a tag, or `null` for none), `cores` (tags), `base_tax`, `base_production`, `base_manpower` and `development` (their sum), `land`, `wasteland`, `flags`, `variables` (see below) |
| `country TAG` | `tag`, `exists` (it owns a province), `province_count`, `provinces` (their IDs, in the game's order), `capital` (an ID, or `null` when none), `observer`, `flags`, `variables`, `pending_events`, `diplomacy` |
| `player` | `tag` (the country the game treats as the player's), `player_tag` and `observed_tag` (as the game stores them; `observed_tag` is `null` when none), `observer` |
| `selection` | `last_selected` (a province ID, or `null`) and `view`: `province` (the one the province view shows, or `null`), `visible`, `path` (the widget path of the window that shows it while it is visible, else `null`: `provinceview`, or `colonisationpanel` for a province without an owner) |

- **IDs and tags.** A province ID is a decimal number without sign or
  leading zeros, valid when `1 ≤ id < count`, with `count` the length of the
  game's province vector read on each call (4942 in vanilla: IDs 1–4941;
  entry 0 is a dummy). A tag is three letters or digits, case-insensitive,
  looked up as the game does; answers give it as the game stores it (upper
  case). Special tags (`REB`, `O00`, `D00`, …) are accepted when the game
  knows them. Names are never accepted as input.
- **Values as the game stores them.** The development parts are without the
  capital bonus. `land` is `false` for seas and lakes. A dead country
  answers `ok` with `exists: false` and `province_count: 0`.
- **`name` is for people.** It is the name the game displays, and that
  follows the owner's culture (Stockholm under Russian rule is `Tukholma`,
  Constantinople `Tsargrad`). Match provinces by ID, never by name.
- **`last_selected` is not "selected now".** It survives closing the
  province view and selecting an army or a fleet, and after a save load it
  may still point at the province selected before the load (while
  `view.province` is `null`). Only `view.visible` answers "is a province view
  open".
- **The observer.** `player.tag` follows the game's rule: the observed tag
  when the player is the observer and an observed tag is set, else
  `player_tag`. In the game only `observe` (the spectator `O00`) was seen;
  the observed-tag path was never seen written.
- **Flags and variables.** `flags` lists the
  flags set now, in the game's order, each with its `name` (as the game's
  files and effects write it) and the `date` it was set (`"1719.3.17"`); for a
  province they are what `has_province_flag` sees, its own flags and its
  permanent ones. `variables` lists each variable with its `name` and its
  `value` (a number, as the game's triggers compare it: `1.25`). Both come
  from Anvil's `anvil-game.world` (Anvil 1.20 or newer), not from addresses of
  Tongs, and are as cheap as the rest of the read. When a list cannot be read
  it is `null`, never an empty list, and `missing` says why for each one:
  `{"flags":"anvil-game.world has no flags: they need Anvil 1.20 or newer"}`
  with an older Anvil, `this game build has no flag symbols (...)` on a build
  whose port declared them absent, or the call's result code when a read
  failed. The rest of the answer is not affected; `missing` is left out when
  every list was read.
- **Pending events** (`country` only, Anvil 1.21 or newer). `pending_events`
  lists the events waiting for that country's choice, in the game's order:
  `pending_id` (what `event choose` and the event window know), `key` (the
  event's key, `"flavor_tur.203"`), `title` (its title's localisation key,
  not the text), `province` (the scope's province, `null` for a country event),
  `options` (in the event's file; the window may disable some), `hidden` and
  `auto_select` (the game chooses for the player the day after). Anvil counts an
  entry from its second frame, so an AI's choice and a hidden event that the
  game runs at once never show up, and right after a load the save's waiting
  events count, hidden ones included. `event list` stays the windows' own view,
  with their texts and buttons. With an older Anvil the list is `null` and
  `missing` has `pending_events`.
- **Diplomacy** (`country` only, Anvil 1.22 or newer). `diplomacy` holds the
  country's relations by kind, each a list of `{first, second, start, end}` as
  the game's save writes them (`end` is `null` for most, and may lie in the
  past): `alliance`, `royal_marriage`, `guarantee` (first guarantees second),
  `military_access` (the country is first: it has access to second's land),
  `giving_military_access` (the country is second), `subsidies` (first pays),
  `dependency` (first is the overlord; for the overlord's own entries also
  `subject_type`, `"vassal"`) and `warning` (first warns). A relation two
  countries share shows up in both with the same `first` and `second`. Then
  `rivals` (tags), `overlord` (a tag or `null`), `subjects` (its direct ones),
  `player` (the player's tag, `null` when it is the country itself) and
  `toward_player` / `player_toward`: `{opinion, timed_sum, trust, favors,
  truce}` of the country toward the player and back. `opinion` is the number of
  the diplomacy window as the game caches it (a human's row is refreshed at
  least monthly, an AI's at least yearly); `timed_sum` is the sum of the timed
  opinion modifiers, not clamped, so not the opinion; `favors` is a number as
  the game shows it. A status is `null` when Anvil has none (a country without
  provinces). With an older Anvil `diplomacy` is `null` and `missing` has
  `diplomacy`. In multiplayer this shows every country's trust and opinions,
  which is more than a player sees.
- **Errors** (`error`, with a `detail` for people): `bad_command` (a bad
  option, or a word after `province` other than `open` and `goto`),
  `unknown_province`, `unknown_country`, `not_in_game`, `main thread busy`
  (no frame within `timeout=`, for example while a save loads; every
  `main thread busy` answer has a `hint` to wait with `ready` first),
  `read_failed` (a read of game memory faulted or a guard failed: an entry's
  own ID, a country's own tag; later reads keep working) and `unavailable`
  (main-thread commands are not available).

```powershell
.\scripts\tongs.ps1 'province 1754'
# {"ok":true,"command":"province","id":1754,"name":"Murom","owner":"RUS","controller":"RUS","cores":["RUS"],
#  "base_tax":4,"base_production":7,"base_manpower":4,"development":15,"land":true,"wasteland":false,
#  "flags":[],"variables":[],"in_game":true,...}
.\scripts\tongs.ps1 'country SWE'
# {"ok":true,"command":"country","tag":"SWE","exists":false,"province_count":0,"provinces":[],"capital":1,"observer":false,
#  "flags":[],"variables":[],"pending_events":[],"diplomacy":{...},...}
```

## Units

`units [TAG]` reads a country's armies and navies, and `unit drill`, `unit
split` and `unit merge` post the commands the unit panel's buttons post. Both go
through Anvil's standard plugin anvil-game: the read through `anvil-game.world`
(Anvil 1.15 or newer), the commands through `anvil-game.commands` version 1
(Anvil 1.17 or newer). Tongs keeps no address of its own for them. Without the
service the command answers `unavailable` and says which Anvil it needs.

- **`units [TAG] [timeout=S]`** is a world read: it needs only `enabled`, is not
  counted, and runs while suspended and during a `run_until`. It answers `tag`
  (as asked, or the player's) and `units`, each with `id`, `kind` (`army`,
  `navy`), `name`, `location` (a province ID, or `null`), `mission` (`none`,
  `drill`, `siege`, … the game's own missions, or `unknown`), `subunits`
  (regiments or ships), `strength` (the regiments' strength × 1000: men for an
  army) and `leader` (an id, or `null`). `unknown_country` for a tag the game
  does not know.
- **`unit drill start|stop UNITS`**, **`unit split UNIT`**, **`unit merge
  UNITS`**, each with `timeout=S`:
  - `UNITS` are unit ids of the player's country (from `units`), up to 64, or
    the word `selected` for the armies and navies the player has selected (the
    game's selection, read when the command runs). `merge` takes two or more.
  - Inputs: they need `input = true`, take turns with the other inputs and
    count in `inputs` when something was posted, as an event option chosen
    through Anvil does. They are what a player's click on the button posts,
    through the same session queue, so they are not game calls.
  - `drill` is an intent, not a toggle: armies that already drill (`start`) or
    do not (`stop`) count as `skipped`, and nothing is posted for them.
  - The answer has `units` (how many the command named), `posted`, `skipped`
    and `refused`, and the state. `posted` means queued: the game runs the
    command at the next turn of its queue, also while paused, and checks it
    again then. Read `units` a few frames later to see the result. For example
    `drill start` for a country at war is posted and then dropped by the game,
    which hides the drill button during a war.
  - Refusals: `unknown_unit` (the player's country has no such unit),
    `no_selection`, `not_in_game`, and Anvil's answers `not_found` (the unit is
    gone or not the player's), `invalid_argument` (a navy for `drill`, units of
    two kinds or provinces for `merge`, a unit named twice), `wrong_phase` (a
    hot-join, or not between frames: try again), `limit` (more than 256
    commands in one frame), `suspended`, `not_declared` (this game build lacks
    the command's symbols).

```powershell
.\scripts\tongs.ps1 'units' 'unit drill start 12884901889' 'unit split selected'
# {"ok":true,"command":"units","tag":"RUS","units":[{"id":12884901889,"kind":"army","name":"1st Army",...}],...}
# {"ok":true,"command":"unit drill","units":1,"posted":1,"skipped":0,"refused":0,"in_game":true,...}
```

## Cursor

`cursor [timeout=S]` answers what the game has under its pointer, read on the
main thread on one frame, from the game's own state rather than from a
screenshot. It is a world read: it needs only `enabled`, is not counted, and
runs while suspended and during a `run_until`.

| Field | What it is |
| --- | --- |
| `point` | `{"x": …, "y": …}`: where the interface sees the pointer, in client pixels |
| `pointer` | `"tongs"` while the pointer of a mouse command holds (until `release` or until the real mouse moves), else `"mouse"` |
| `inside` | the point lies in the client area (`false` while the window is minimized) |
| `widget` | the path of the widget the interface hovers there (as `ui find` gives it), or `null` |
| `over_map` | no widget takes the pointer there: a click there goes to the map |
| `province` | the province the map has under the point, `{"id", "name", "owner", "land"}` (`owner` `null` for none; seas and lakes have `land: false`), or `null` off the map (the edge of the world, outside a game) |
| `unit` | the army or navy the map picks at the point, as `units` gives it: `{"id", "kind", "tag", "name", "location"}`, or `null` |

- **To ask about a point**, move the pointer there first: `move X Y`, then
  `cursor` (the interface updates its hover on its next frame, so `cursor`
  waits for one). A mouse command's pointer holds until `release` or until the
  real mouse moves; after that `cursor` reports the real mouse.
- **The province** is the one the game's map shows under the point: the pick
  ray from the camera through the point meets the terrain, and the province
  bitmap answers there. It is there also when a widget covers the point: read
  `widget` and `over_map` to tell whether a click would reach the map.
- **The unit** is what the map's click would select there: an army's or a
  navy's map icon, or its model. It is reported only when the unit is in its
  country's `units` (Anvil 1.15 or newer); without `anvil-game.world` `unit`
  is always `null`.
- **Outside a game** (the menu, the lobby) only `point`, `pointer`, `inside`
  and `widget` have values.
- **Errors**: `bad_command`, `main thread busy` (no frame within `timeout=`,
  default 10 s, up to 60), `read_failed` (the game window was not found, or
  the map's pick faulted or failed a guard), `unavailable`.

```powershell
.\scripts\tongs.ps1 'move 1720 720' 'cursor'
# {"ok":true,"command":"move",...}
# {"ok":true,"command":"cursor","point":{"x":1720,"y":720},"pointer":"tongs","inside":true,"widget":null,"over_map":true,
#  "province":{"id":295,"name":"Moskva","owner":"RUS","land":true},"unit":null,"in_game":true,...}
```

## Navigation

`province open ID` and `province goto ID` move the camera and open a
province's view as the game's own province links do, instead of finding the
province on a screenshot and clicking a pixel. They are **client calls**
([ADR 0003](adr/0003-client-calls.md)): game code run on the main thread
after an outer frame that changes only the camera, the selection and open
views. They need `input = true`, take turns with input commands, and each one
that called the game counts once in `client_calls` (and
`client_calls_total`, and an `activity` notification with
`what: "client_call"`), so a run with one is a marked run. In ironman and
multiplayer they follow input: Anvil's allow lists apply.

- **`province goto ID`** moves the camera over the province, nothing else:
  no selection, no view, no map highlight. It takes any valid ID, seas,
  wastelands and terra incognita included.
- **`province open ID`** does what the religion view's province link does:
  it closes the open views, moves the camera to the province and selects
  it, which shows it in the province view. It answers `ok` only when the
  next outer frame shows it: the selection holds the province, the province
  view shows it and the window that shows it is visible. The answer carries
  `province`, `view` (the widget path of that window: `provinceview` on the
  default interface, `colonisationpanel` for a province without an owner) and `covered_by` (the widget the game hovers at the
  view's centre when it is not part of the view, such as an event window, or
  `null`; Tongs moves the pointer there to ask, as `ui click` does, then
  puts it back where the last mouse command left it, or hands it back to
  the real mouse). Being covered is not a refusal. When the frame after the
  call does not show it, the answer is `not_shown` with what it saw
  (`selected`, `view_province`, `visible`), and the call still counts.
- **The camera** (both): the command waits until the camera's position
  reaches its target, then answers `camera: {"settled": true, "target":
  {"x": …, "z": …}}` (map coordinates). At the timeout it answers `ok` with
  `settled: false`. The camera glides there in about a second, but the game
  moves its target on as the player does: a pointer at the edge of the
  screen scrolls the map, and then the camera never settles on the province
  (`settled: false`). `target` is always the target the call set. With the
  camera locked (`debug_lockcamera`, or while the in-game menu is open),
  `goto` refuses `camera_locked`; `open` still opens and answers
  `"locked": true`.
- **`timeout=S`**: default 10 s, up to 60, for the whole command; `main
  thread busy` when no outer frame comes in time (nothing was called).
- **Opening a view can change the game.** The game's province view makes the
  player discover the capital of a foreign owner whose capital the player
  has not discovered. Tongs checks that case before calling: without
  the `game_calls` module it refuses `needs_game_calls`; with it, the call counts
  as one game call only (in `game_calls`, not `client_calls`), takes the
  console's ironman and multiplayer refusals, and the answer carries
  `"game_call": true` (and `mode` in an ironman or multiplayer game that allows game calls).
- **Refusals before calling** (nothing called, nothing counted; they go to
  the Anvil log):

  | Error | Commands | When |
  | --- | --- | --- |
  | `not_in_game` | both | no game loaded |
  | `input_disabled`, `suspended` | both | `input = false`; Anvil keeps Tongs suspended |
  | `unknown_province` | both | the ID is outside `1 ≤ id < count` |
  | `not_selectable` | `open` | a sea, a lake or a wasteland |
  | `terra_incognita` | `open` | the player has not discovered it (an observer passes) |
  | `selection_blocked` | `open` | the game selects nothing now (a province-picking mode, the tutorial) |
  | `camera_locked` | `goto` | the camera is locked: `debug_lockcamera`, a window that holds it (the in-game menu does), or the game window is minimized (the game holds the camera until it is restored; the detail says so) |
  | `needs_game_calls` | `open` | the discover case above without the `game_calls` module |

  For a province without an owner the game shows its colonisation panel
  instead of the province view: `open` answers `ok` with
  `view: "colonisationpanel"`, and `selection` reports that panel too. Read
  the owner with `province ID`. A fault in the game's code answers `faulted` and turns
  navigation off until the game exits (`navigation_tripped`).

```powershell
.\scripts\tongs.ps1 'province open 295'
# {"ok":true,"command":"province open","province":295,"view":"provinceview","covered_by":null,
#  "camera":{"settled":true,"target":{"x":…,"z":…}},"client_calls_total":1,"waited_ms":…,"in_game":true,...}
```

## Widgets

`ui find` and `ui tree` read the game's interface: windows, buttons, icons,
text, list boxes, check boxes and edit boxes, by their names in the game's
`.gui` files. A selector is a path of names:

- `speed_controls/button_speedup`: a child of a top-level window;
  `EventWindow//event_option_button`: anywhere below it; `//DateText`:
  anywhere. A selector starts with a top-level window's name or with `//`,
  never with a single `/`.
- Names ignore case and take `*` and `?`; there are no character classes.
- Filters in brackets after a name, as many as needed: `[2]` (the second
  match, from 1; one per step), `[type=button]` (`window`, `button`,
  `icon`, `text`, `listbox`, `checkbox`, `editbox`, `other`),
  `[text=...]` (the whole text), `[text~=...]` (contains), `[text^=...]`
  (starts with), `[enabled]`, `[disabled]`, `[checked]`, `[unchecked]`,
  `[visible]`, `[hidden]` (the effective visibility; without `all` every
  match is visible, so `[hidden]` finds widgets only with `all`). Text filters ignore case; put the value in `"..."` when it has `]` in it
  or spaces at its ends (single quotes are part of the value). A step made
  only of filters means any name: `speed_controls/[type=button]`.
- It is not XPath or a glob: `//*[name='text_MIL']` is `//text_MIL`,
  `//*[@text='Save']` is `//[text=Save]`, `**/*[Ss]ave*` is `//*save*`.
  A `bad_selector` answer has the parser's `detail` and a `hint` with this
  grammar in short.
- Hidden widgets are left out unless the command ends with `all`.
- Stale widgets are left out of `ui find` unless it ends with `stale`. They
  were built from a `.gui` definition that a reload (console `reload <file>`)
  freed: windows the game does not rebuild, or leaks when it rebuilds the
  HUD. Tongs never asks such a widget anything that reads the freed
  definition, so a stale window is named only when its parent's child list
  or its own name says so. `ui tree` shows stale widgets with
  `"stale": true`, and `ui click` refuses them.

Every widget in an answer has a `path` that finds it again, with `[n]`
where siblings share a name. `text` is what the game displays, in UTF-8
without its colour codes, so text filters depend on the game's language;
prefer names and `[n]`. Edit boxes have their `text` too, and check boxes
`checked`. `click_point` is the centre of `rect` and can be passed to `click`.
The game lays out its interface in its own coordinates, client pixels divided
by the GUI scale (`game_ui_scale` in the game's settings); Tongs multiplies
them by the scale, so `rect` and `click_point` are client pixels at any
scale, as `click` takes them. `ui find`, `ui tree` and `ui click` end with
`ui_scale`, the scale they used; it is `null` when the game's value could
not be read, and the rects are then the game's coordinates as they are.
Rects lie inside the game window's client area: a window without a size,
or one that does not fit the client area (a fullscreen window), takes the
bounds of its children, and a widget wholly outside the client area (the
game parks some there) counts as hidden. Other window sizes come from the
`.gui` files and may be smaller or larger than the drawn panel. List boxes
create widgets only for the rows they show and refill them as they scroll,
so a row's path stays while its text changes.

The game reports no widget under the pointer on masked shields (country
shields in the lobby and in dialogs), so `ui click` refuses them as
`covered` with `covered_by: null`; `click` at their `click_point` works.

`ui click SEL` resolves the selector, moves the pointer to the centre of the
widget, checks that the click would reach that widget, and only then clicks.
The game hands a click to the widgets under the pointer from the topmost down
until one takes it, so the click goes ahead when the topmost widget is the
target or one inside it, and also when the widgets above the target let it
through: text, windows, list boxes and buttons or icons without a click action
(a label or a background drawn over the target). It refuses without clicking: `not_found`,
`ambiguous` (with up to 10 `matches`), `not_visible` (only hidden widgets
match), `stale` (only stale widgets match), `disabled`, or `covered` (with `covered_by`, the widget that would get
the click: another button, a check box, a window that takes all input such as
the game menu). The answer has `clicked`, `point`, `hovered` (the topmost
widget under the pointer), `through` (the widgets above the target the click
passed, when there are any) and the state after the click.
With `wait=S` (1–600) a dialog that shows later needs no loop of sleeps and
finds: while only `not_found` or `not_visible` comes, or the main thread is too
busy to read the screen (a save loading), it looks again every 250 ms until S
passes, then clicks or answers as without it, with `waited_ms`; the other
refusals come at once, and `cancel` ends the wait (`cancelled`). For example
`ui click DefaultConfirm/AgreeButton wait=120` after a `continue=` start of a
modded game, which waits behind the "Vanilla achievements can no longer be
earned" notice. Other input commands wait their turn meanwhile.
When the game reports no widget at all under the pointer, the move is tried up
to three times (`hover_attempts`). `covered` with `pointer_released: true`
means the real mouse moved meanwhile, so the game took the real pointer again
(see the mouse commands above); the same answer comes from `event choose` when
its click did not take. A click on an event's option button also
answers `event_option` and counts as a choice Tongs knows (see [Events](#events)).

```powershell
.\scripts\tongs.ps1 'ui find speed_controls/button_speedup'
# {"ok":true,"command":"ui find","selector":"speed_controls/button_speedup","count":1,"truncated":false,
#  "matches":[{"path":"speed_controls/button_speedup","type":"button","visible":true,
#  "rect":{"x":3335,"y":18,"w":30,"h":30},"click_point":{"x":3350,"y":33},"enabled":true}],...}
.\scripts\tongs.ps1 'ui tree depth=0'
```

## Events

`event list` reports each open event window, topmost (newest) first, with its
`number`, `id` (the script id, e.g. `flavor_rus.1`), `pending_id` (the id of
the game's pending event, which `anvil-game.commands` takes), `title`, `description`,
widget `path`, and its options: `number` on screen (from 1), `text`,
`enabled`, `option_index` (the option in the event file; options hidden by
their trigger are skipped on screen but keep their index), `option_key` (the
option's `name`, which may be empty or repeated) and `path`.

`event choose` clicks an option, as a player would: by number
(`event choose 2`) or by text (`event choose text^="Accept"`, also `text=` and
`text~=`). With several events open it needs `event=N` or
`event=title^="..."`, and refuses `ambiguous` otherwise. The game keeps the
options disabled for half a second after the window opens; meanwhile
`event choose` refuses `disabled`. An option outside the event's list box
(the box shows six rows) is scrolled in with the list's scroll bar buttons
first (`scrolled`, the number of clicks). The answer has `choice_date` (the game date
of the click), the `event` and `option`, `closed` (the event left the game's
lists within 3 s; it took 3 frames in the live check) and the state after.

`run_until DATE events=first` does the same for every event that opens during
the run: it waits for the first enabled option of the topmost event, clicks it
and lists the choices in `events`; when no option becomes enabled, the run
ends with `no_enabled_option` and the event. An event that paused the game is
resumed after the choice. Every click counts in `inputs`.
With an Anvil whose `anvil-game.commands` has version 2 (Anvil 1.17), the choice
is made through that service instead of a click: it does what the option's button
does, so it works with the game window minimised or covered. The choice counts in
`inputs` all the same, and its entry in `events` and the `event` notification at
the close have `via: "service"`. While the service says the button is not enabled
yet, the run tries again as it does for a click. Without the service, or on a game
build that lacks its symbols, the run clicks as before; in ironman or multiplayer
without the player's opt-in the service answers that Tongs is suspended and the run
ends with that error.
A choice that does not take (`covered`, or the event still waiting for a choice
3 s after the click) is tried again a quarter of a second later, from whatever
event is then on top, while the game goes on. `choice_retries` counts these
tries in the run, and a choice that needed more than one click has `attempts`
and `missed`: how many clicks were `covered` (of them `nothing_hovered`, with
`covered_by: null`), how many left the event `still_waiting`, and in how many
of both the real mouse had the pointer (`pointer_released`).
Only when the same event has not taken a click for 30 s does the run end with
that error, the event and its `attempts`. Like every `run_until`, it ends
paused.

## Notifications

`subscribe KINDS [date=day|month|year]` turns the connection into a stream:
Tongs sends a line whenever something changes, without being asked. `KINDS`
is `all` or a list such as `state,date,event`:

| Kind | Sent when |
| --- | --- |
| `state` | the game is entered or left (`change`: `entered_game` with `session`, `left_game` with `reason`; a load is `left_game` then `entered_game`), paused or resumed (`paused`, `resumed`), the speed changes (`speed`, with `previous_speed`) or a multiplayer game goes out of sync (`out_of_sync`, with `out_of_sync` and `paused_oos`); each with `speed`, `paused`, `paused_flag`. Also while a save loads |
| `date` | a new day, month (the default) or year begins, with `unit` and `previous` |
| `event` | an event window opens (`opened`, with `id`, `title` and `options`) or closes (`closed`, with `chosen`: the option, with `by` saying who chose it: `tongs` for `event choose`, `run_until events=first` or `ui click` on the option button (`via: "ui click"`), `player` for any other click on an option (the real mouse, a plain `click`), `auto` for the game's own choice at the deadline; `via: "service"` when `run_until events=first` chose through `anvil-game.commands`; `null` when nobody's choice is known) |
| `error_log` | the game writes a `line` to its `error.log`; `restarted` when the file was replaced. A line of Anvil's `anvil-log-throttle` also has `throttle` and its count (below) |
| `activity` | any client sends an input command (`what: "input"`, `command`), makes a client call (`what: "client_call"`, `command`) or a game call (`what: "game_call"`, `line`, `name`), with the `client` (pipe instance) and the running count (`inputs`, `client_calls`, `game_calls`), so a marked run shows as it happens |

```powershell
.\scripts\tongs.ps1 -Subscribe 'state,date,activity date=day' -Seconds 30
# {"ok":true,"command":"subscribe","kinds":["state","date","activity"],"date_unit":"day","queue":1000,"in_game":true,"date":"1719.3.17",...}
# {"notification":"state","seq":1,"date":"1719.3.17","wall_ms":2210,"change":"resumed","speed":3,"paused":false,"paused_flag":false}
# {"notification":"date","seq":2,"date":"1719.3.18","wall_ms":2304,"unit":"day","previous":"1719.3.17"}
```

- Every notification has `notification` (its kind), `seq` (from 1 on this
  connection, without gaps), the game `date` (`null` outside a game) and
  `wall_ms` since the subscription. Answers always have `command`, so the two
  never mix up.
- The `subscribe` answer lists the `kinds`, kinds the plugin cannot serve as
  `unavailable`, for `error_log` and `event` the file each follows
  (`error_log`, `game_log`), and the current state,
  which the first notifications change. Only changes after the subscription
  are reported.
- `chosen` with `by` `player` or `auto` comes from the game's `game.log`,
  which names the option's index; `number`, `text` and `option_key` are those
  the event opened with (`null` for an option that was not on screen). Such a
  `closed` may come up to a second late while Tongs waits for the line. When
  two events with the same id close at once and the log names different
  options, `chosen` is `null` for both.
- `error_log` sends the lines that reach the file. From Anvil 1.6, its
  standard plugin `anvil-log-throttle` (on by default for `error.log`) writes
  a message that repeats 10 times, then a notice and periodic reports of the
  count in place of the repeats; with the plugin's `full_log` the player gets
  every line again. The notifications of the plugin's lines keep `line` as it
  is in the file and add
  `throttle`, the count and `text`, the line as the game wrote it each time
  (the game's `[<file>:<line>]: ` and its message, so it equals the `line`
  of the message's ordinary notifications):
  - `"throttle":"report"` with `repeated` (lines counted, not written) and
    `window_s` (the seconds they were counted in), for
    `[anvil-log-throttle] repeated <n> more times in the last <s> s: <text>`;
  - `"throttle":"notice"` with `logged` (lines written before counting began)
    and `report_s` (how often the plugin reports), for
    `[anvil-log-throttle] logged <n> times; further repeats are counted and reported every <s> s: <text>`.
    The notice stands in for the first counted line, which its first report counts.

  So the game wrote a message as many times as its ordinary lines plus the
  `repeated` of its reports; a notice adds nothing. Counts reach the file
  only when a report is due (at a line of the log once the count is
  `report_s` old) or when the game closes the log at exit, and a killed game
  loses the counts since its last report. A message longer than the 4096
  bytes of a line has its `text` cut with it; a message of several lines has
  its first line in `text` and its other lines as ordinary notifications after
  the report.
- While subscribed, the connection answers `ping` and `unsubscribe` and
  refuses everything else (`subscribed`). `unsubscribe` answers with `sent`,
  `dropped` and `discarded` (still queued, not sent) and the connection takes
  ordinary requests again.
- A client that reads slowly does not hold anyone up: each subscription keeps
  up to 1000 notifications, and when that is full the oldest are dropped and
  `{"notification":"overflow","dropped":N}` comes before the next one.
- A subscription holds one of the 8 pipe instances, so use a connection of its
  own for it.

## Journal

With `journal = true` (the default) Tongs writes every request any client
sends on the pipe to a journal, so the course of a test can be read and
repeated afterwards. It is a file of its own for each game start,
`anvil/data/tongs/journal/journal-<process_start_ms>-<pid>.jsonl`, and the
newest 20 are kept. It lives only in that file: nothing of it is in the
game's memory, its saves or what multiplayer compares, and it records the
same whether the commands are reads or inputs.

The first line names the file's process:
`{"journal":1,"tongs":"0.32.0","pid":21384,"process_start_ms":1792418572041,"opened":1792418573311}`.
Then one line per request, in the order the answers were sent:

| Field | What it is |
| --- | --- |
| `seq` | from 1, without gaps |
| `t` | when the request came, Unix ms |
| `client` | the pipe instance that sent it (connections keep theirs while they last) |
| `kind` | `read`, `input` (the input commands, `ui click`, `event choose`, `unit`), `client_call` (`province open`, `province goto`, `map_mode`), `game_call` (`console`, `save load`) or `stream` (`subscribe`, `unsubscribe`) |
| `request` | the line as sent |
| `ms` | from the request to its answer |
| `ok`, `error` | the answer's; `error` is `null` when it was `ok` |
| `date`, `session` | the game date and session the answer carries, or `null` |

`kind` is taken from the command word, before the command runs: a refused
input is an `input` with `ok: false`. Notifications are not written; their
`subscribe` is. Each line is flushed as it is written, so a game that crashes
keeps what came before. A file stops at 64 MB with
`{"truncated":true,"seq":N}`. `journal` answers where the file is:

```powershell
.\scripts\tongs.ps1 journal
# {"ok":true,"command":"journal","enabled":true,"path":"C:\\...\\anvil\\data\\tongs\\journal\\journal-1792418572041-21384.jsonl","lines":12,"truncated":false}
```

To repeat a test, send the `request` of its `input`, `client_call` and
`game_call` lines in order, after the same save and the ready wait, and
compare the `date` of each answer.

## Loading a save

`save load NAME [timeout=S]` loads a save in the running game, as the in-game
Load dialog does, or from the main menu, as the menu's Continue does, without
clicking through either. It is a **game call**
([ADR 0001](adr/0001-game-calls-in-tongs.md)): it needs the `game_calls` module,
takes the console's refusals in ironman and multiplayer, is logged to the
Anvil log and counts once in `game_calls` (with an `activity` notification)
when Tongs called the game's loader. A run with it is a marked run.

- **NAME** is a file at the top level of `save games`: `mp_Russia1719_03_17`
  or `mp_Russia1719_03_17.eu4` (a trailing `.eu4`, any case, is optional),
  quoted when it has spaces: `save load "Russia test"`. No display names, no
  subfolders, no Steam cloud saves. A name that is empty or has `/`, `\`, `..`
  or characters outside printable ASCII and Latin-1 is `bad_name`.
- **In a game, paused; or in the main menu.** In a game it must be paused, as
  it is in the Load dialog (`not_paused` otherwise). In the main menu it needs
  no pause (see below). Other menu screens (the single-player lobby, the
  settings) refuse.
- **From the main menu** Tongs makes the save the menu's Continue save and asks
  the menu to continue it, as a start with `continue=` does: the game reads the
  save's header, then the menu loads it on the frames after. The answer comes
  once a frame shows the new session in game and not loading, with the save's
  date and player, and has `via: "menu"`. The menu's Continue button names
  that save afterwards; `continue_game.json` is left as it was.
  The menu meant here is the one the game starts to: without a plugin that
  keeps the process (a menu-patch plugin), "Exit to Menu" and "Back"
  from the country selection restart the game in a new process, so a client
  that went back to the menu waits for the new `pid` first.
  Refusals: `not_in_menu` (the game shows neither a game nor its main menu;
  nothing called), `not_taken` (the main menu did not take the load on its
  next frame because another screen is open; Tongs took it back and nothing
  was loaded, `called: true`).
- **What it discards.** The running game is discarded **without saving and
  without a warning**, as the Load dialog does; Tongs never saves the game.
  Tongs then hides the in-game windows, as the game does when it resets a
  session: no in-game menu and no province or country view of the old game
  stay open, `covered_by` is `null` and `speed` works at once. This is where it
  differs from the dialog, whose load leaves the in-game menu open (see
  [Sessions](#sessions-and-the-ready-wait)).
- **Before calling**, Tongs reads the save's metadata itself (text saves,
  compressed saves with a text or binary `meta`): its date, player, whether it
  is an ironman save and the versions it was saved with. An ironman save is
  refused (`ironman_save`), because the game writes to it afterwards, unless
  Tongs loads it from an ironman game in which Anvil lets the `game_calls`
  module act (`allow_ironman = ["tongs", "tongs.game_calls"]`): whether the
  player allows game calls in ironman is Anvil's, and Tongs learns it only in
  an ironman session, so from the main menu it always refuses one. A save from another version, or with other DLC or
  mods, is not refused: `compatible` says whether its versions list this one,
  as the dialog's version warning does.
- **One answer, after the load and its check.** The game loads the save on
  its main thread in one go (about 8 s for a 1719 save); no frame runs
  meanwhile. `ok` comes only when the loader returned 0 and the next outer
  frame shows a game that is not loading, with the save's date and player.
  The answer carries `save`, `save_date`, `player`, `compatible`, `load_ms`,
  `session` (the new one), `covered_by` (the widget the game hovers in the
  middle of the window when one covers the map, or `null`), `game_calls_total`
  (1: the session came from a game call), `save_versions` and `game_version`
  (what `compatible` compared) and the fields of `state`. Pause and
  speed are not changed: the game stays paused, with the save's speed.
- **`timeout=S`**: default 120 s, up to 600, for the whole command. When no
  frame comes before it, nothing was called (`main thread busy`). When the load
  is still running at the timeout, the answer is `timeout` with
  `started: true`, and its effect is unknown; the load counts, and its
  `activity` notification comes, when it ends.
- **Refusals before calling** (nothing called or counted): `bad_name`,
  `no_such_save`, `unreadable_save` (Tongs could not read the metadata),
  `ironman_save`, `not_in_game`, `not_paused`, `game_calls_disabled`,
  `suspended`, `game_calls_tripped`, the console's `ironman` / `multiplayer`
  refusals for the running game. **After calling** (`called: true`, one game
  call): `bad_name`, `no_such_save` or `rejected` for the loader's return codes
  1, 2 and 3 (3: the metadata was rejected, such as an ironman save in a
  modded game), `not_loaded` (with what the frame showed, `seen`), `faulted`.
  In an ironman or multiplayer game that allows game calls, these answers
  carry `mode`.
- **Afterwards** the client waits for input with the ready wait for the new
  `session` (see [Sessions](#sessions-and-the-ready-wait)); the `state`
  notification `entered_game` says `loaded_by: "tongs"`, and `null` after a
  load by the player or a start.
- **Several scenarios on one start.** A test that would start the game once
  per save can start it once, to the main menu (no `continue=`), and load each
  scenario's save in turn: `save load A` from the menu, the ready wait for its
  `session`, the scenario; then `save load B` from the paused game, the ready
  wait for the next `session`, and so on. Each load discards the game before
  it and starts `game_calls_total` and `client_calls_total` again, so a
  scenario's waits count only its own calls. The speed comes from the save;
  what lives outside it, such as the game's settings, stays as the last
  scenario left it.

```powershell
.\scripts\tongs.ps1 'save load mp_Russia1719_03_17'
# {"ok":true,"command":"save load","save":"mp_Russia1719_03_17","called":true,"load_ms":7817,"save_date":"1719.3.17",
#  "player":"RUS","compatible":true,"save_versions":["1.37.5.0"],"game_version":"1.37.5.0","session":2,
#  "covered_by":null,"game_calls_total":1,...,"paused":true,...}
```

## Console

`console LINE` runs a console command the way the game's console does, but
without the console window: the line is split on spaces and passed to the
game's command dispatcher on its main thread, after a frame of the main loop.
It needs the `game_calls` module and a loaded game (`in_game: true`), and takes
turns with the input commands.

```powershell
.\scripts\tongs.ps1 'console echo hello' 'console speed 3'
# {"ok":true,"command":"console","line":"echo hello","name":"echo","console":{...},"text":"hello","success_hint":true,"game_calls_total":1,...}
```

The answer has `name` (the command the game found, also for an alias),
`text` (the game's answer, UTF-8 without colour codes), `success_hint` (the
game's success byte; some commands never set it, so check the state instead),
`game_calls_total`, `queued_ms`, `run_ms`, `frame` and the fields of `state`
read after the call. Nothing is added to the console window, its history or
`console_history.txt`; Tongs writes each call to the Anvil log.

Tongs refuses, without calling the game:

- `refused`: `enable_all_commands`, `debug_achievements_clear`,
  `test_achievement`, `debug_achievements`, `social_*` and
  `CrashReporter.UploadCrash`, always; the other dangerous commands (crashes,
  memory leaks, desyncs, rewriting the map or the world, install and save
  rules, `run_commands` and `runyear`) unless the settings list them:
  `console_allow = "leak"`;
- `ironman`, `multiplayer`: every game call in an ironman or multiplayer game,
  unless Anvil lets the `game_calls` module act in it: the player allows it in
  `anvil/anvil.toml` (`allow_ironman` / `allow_multiplayer = ["tongs.game_calls"]`,
  next to `"tongs"`) or with the module's switches in Anvil Manager. Tongs
  knows that only once Anvil has judged the session (`anvil-game.session`),
  so a call before that is refused too. In a game that allows them, answers
  carry `mode`: `ironman` and `multiplayer` as the game reads them, and
  `game_calls_ironman` / `game_calls_multiplayer`, the session's modes in which
  Anvil lets the module act;
- `suspended`: Anvil suspended Tongs in this ironman or multiplayer game (see
  [Safety](../README.md#safety)), before any of Tongs' own switches are asked;
- `game_calls_disabled` (the module is off), `not_in_game`, `unknown_command`, `bad_command`
  (printable ASCII only), `console_unavailable`;
- `game_calls_tripped`: a game call that faulted answers `faulted`, and every
  later one is refused until the game exits. Reads keep working.

The game's own checks still apply: developer-only commands answer "Command
available only for developers." in `text`.
