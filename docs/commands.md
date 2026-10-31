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
| `state` | `in_game`; with a loaded game also `date` (`"1719.3.17"`), `date_raw`, `hour`, `speed` (1–5 as in the UI), `speed_internal` (0–4), `paused` (the game's own answer), `paused_flag` (the player pause byte), `session` (see [Sessions](#sessions-and-the-ready-wait)), and `game_calls_total` and `client_calls_total` (game calls and client calls since the save was loaded); always `pid` and `process_start_ms`, which every answer that carries the state repeats, so a client can tell a restarted game apart |
| `window` | the game window: client `width` / `height`, `screen_x` / `screen_y`, `focused`, `minimized`, `thread_id` (the game's main thread), `dpi` |
| `threads` | worker threads in each of the game's two pools, the `threads` option, process and main-thread CPU masks, ideal CPU and priority of the main thread, number of threads |
| `screenshot [NAME]` | writes a PNG of the game window to `anvil/data/tongs/screenshots/` (keeps the newest 100) and answers with its `path`, size and capture `method`; works while the window is covered |
| `frames [timeout=S]` | read on the game's main thread after a frame of its main loop: frames per second (`outer_per_s`, and `nested_per_s` for frames drawn inside a game update or a save load), counts and the longest gap between main-loop frames since the previous `frames`, and the main thread's `thread_id`; with `queued_ms`, `run_ms`, the game's `frame` counter and the fields of `state` read on that frame, so its `session` says which session the frame belongs to. Waits up to 10 s (`timeout=` up to 600) for a frame; while a save loads none comes, and the answer is `main thread busy` |
| `ui find SEL [all] [stale] [limit=N]` | widgets that match the selector `SEL` (see [Widgets](#widgets)): `count`, `truncated` and up to 50 `matches` (`limit=` up to 5000), each with `path`, `type`, `visible`, `rect` and `click_point` in client pixels, and `text` / `enabled` / `stale` where they apply. Read on the main thread, like `frames` |
| `ui tree [SEL] [depth=N] [limit=N] [all]` | the widget tree from the top-level windows, or from the matches of `SEL`, as nested `widgets` (up to 200, `limit=` up to 5000); `depth=N` gives deeper levels only as `children_count` |
| `event list [timeout=S]` | the open events, topmost first (see [Events](#events)), read on the main thread |
| `province ID [timeout=S]` | one province by ID: name, owner, controller, cores, development, land and wasteland (see [World reads](#world-reads)) |
| `country TAG [timeout=S]` | one country by tag: whether it exists, its provinces and its capital |
| `player [timeout=S]` | the player's country |
| `selection [timeout=S]` | the last selected province and whether a province view shows one |
| `wait_until DATE [timeout=S] [max_pause=S]` | waits (without touching the game) until the date reaches `DATE` (`1720.1.1` or `1720.1.1.12`); answers with the timing below |
| `cancel` | ends every `wait_until` / `run_until` in progress and withdraws main-thread jobs still waiting for a frame (`withdrawn`) |
| `subscribe KINDS [date=day\|month\|year]` | turns the connection into a stream of notifications (see [Notifications](#notifications)); `unsubscribe` ends it |
| `help` | list of commands, whether input commands are enabled and whether Anvil suspended Tongs (`suspended`, see [Safety](../README.md#safety)) |

With `input = true` in the settings, also:

| Command | What it does |
| --- | --- |
| `pause` | presses Space unless the player pause is already on, then waits until it is |
| `resume` | presses Space if the player pause is on, then waits until the game runs; refuses when something else holds the game paused |
| `speed N` | presses keypad `+` / `-` one step at a time until the speed is `N` (1–5, as in the UI); works while paused |
| `run_until DATE [speed=N] [timeout=S] [max_pause=S] [events=first\|stop]` | sets the speed, resumes, waits for the date and pauses again; for measuring how fast the game runs. `events=first` chooses the first enabled option of every event that opens on the way |
| `key NAME` | presses one key: `a`–`z`, `0`–`9`, `f1`–`f12`, `space`, `return`, `escape`, `tab`, `backspace`, `delete`, `minus`, `equals`, `grave` (the console key on a US layout), `kp_plus`, `kp_minus`, `kp_enter`, arrows (`up`, `down`, `left`, `right`), `home`, `end`, `page_up`, `page_down` |
| `text STRING` | types the rest of the line into the focused text field (characters U+0020–U+007E and U+00A0–U+00FF) |
| `move X Y` | points the mouse at client coordinates `X Y` (hovering shows tooltips) |
| `click X Y [left\|right\|middle] [double]` | moves there and clicks |
| `scroll STEPS [X Y]` | turns the wheel over the list or map under the pointer; positive scrolls up (zooms in on the map), negative down. With `X Y` it moves there first and lets the game see the pointer, as `click` does |
| `release` | hands the pointer back to the real mouse |
| `ui click SEL [timeout=S]` | clicks the centre of the one widget the selector matches (see [Widgets](#widgets)) |
| `event choose N\|text^="..." [event=N\|event=title^="..."] [timeout=S]` | clicks an option of an open event (see [Events](#events)) |
| `province open ID [timeout=S]` | opens the province's view and moves the camera to it, as the game's links do (see [Navigation](#navigation)) |
| `province goto ID [timeout=S]` | moves the camera to the province |

With `game_calls = true` in the settings, also (see [Console](#console)):

| Command | What it does |
| --- | --- |
| `console LINE` | runs one console command line in the game, on its main thread, and answers with the game's text and the state afterwards. **Changes game state** |

Input commands answer once the game has reacted (or after 10 s) with
`command`, `presses`, `waited_ms` and the fields of `state`. When the game
does not react in time, `ok` is `false` with an `error`, and the state is
still included. `key`, `text` and the mouse commands do not expect a change;
they answer with the state read 0.1 s after the input. Input commands from
different clients take turns.

`wait_until` and `run_until` report `start_date`, `reached` and
`reached_date`, `game_days`, `wall_ms` (from the start of the wait until the
target date was first seen), `days_per_second`, `paused_ms` (time seen paused
while waiting, e.g. an event that pauses the game; with `paused_date`, the
date of the last pause), `first_change_ms`, `inputs` (input commands any
client sent during the wait; `run_until` does not count its own presses
before and after it), `client_calls` (client calls any client made during the
wait, [ADR 0003](adr/0003-client-calls.md): `province open` and `province
goto`, see [Navigation](#navigation)) and
`game_calls` (console commands any client ran during the wait), so a
measurement can reject a run with any of them above 0, and treat a missing one
(an older Tongs) as unknown;
`run_until` also `start_speed` and `overshoot_hours` (game hours past the
target when the pause took hold, usually 0). Both wait at most 1 hour by
default (`timeout=` up to 14 400 s). `max_pause=S` ends the wait with an error
once the game has stayed paused for `S` seconds, e.g. on an event with a
choice, instead of waiting for the timeout. A wait ends with an error when the
game leaves the session: back to the menu, or a load (the `session` changes).

The mouse commands also make the game's interface see the pointer at the given
point (a hook on the game's cursor query), until `release` or until the real
mouse moves; the real cursor itself does not move.

```powershell
.\scripts\tongs.ps1 state
# {"ok":true,"in_game":true,"date":"1719.3.17","date_raw":58860240,"hour":0,"speed":2,"speed_internal":1,"paused":true,"paused_flag":true,"session":1,"pid":21384,"process_start_ms":1792418572041,"game_calls_total":0,"client_calls_total":0}
.\scripts\tongs.ps1 'run_until 1719.6.1 speed=5'
# {"ok":true,"command":"run_until","target":"1719.6.1","start_date":"1719.3.17","reached":true,"reached_date":"1719.6.1",
#  "game_days":76.000,"days_per_second":12.352,"wall_ms":6153,"paused_ms":0,...,"overshoot_hours":0,...,"paused":true,...}
.\scripts\tongs.ps1 'click 3350 32' screenshot
```

Right after a save loads, the game handles input only once an outer frame of
the new session has run; the commands wait for it, and the
[ready wait](#sessions-and-the-ready-wait) waits for that frame first. While
the Esc menu is open, Space does not resume the game.

## Sessions and the ready wait

`session` numbers the games of one game process: 1 for the first game after
the start (from the main menu or with `continue=`), +1 at every load, by the
player or a later Tongs command. A client pins it like `pid`: a changed
`session` means the game state was replaced, even when the date comes back
the same. Tongs reads the state every 20 ms to see every load. `state`
answers it and every answer that carries the state repeats it; outside a game
it is absent. The `state` notification `entered_game` carries it too. A load
starts `game_calls_total` and `client_calls_total` again and ends a
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
and 31 ms after a load. A load from the in-game menu leaves that menu open,
and the speed keys do nothing while it is: close it first
(`ui click menu/menu_panel/menu_close_button`); the ready wait does not look
at windows.
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
| `province ID` | `id`, `name`, `owner`, `controller` (a tag, or `null` for none), `cores` (tags), `base_tax`, `base_production`, `base_manpower` and `development` (their sum), `land`, `wasteland` |
| `country TAG` | `tag`, `exists` (it owns a province), `province_count`, `provinces` (their IDs, in the game's order), `capital` (an ID, or `null` when none), `observer` |
| `player` | `tag` (the country the game treats as the player's), `player_tag` and `observed_tag` (as the game stores them; `observed_tag` is `null` when none), `observer` |
| `selection` | `last_selected` (a province ID, or `null`) and `view`: `province` (the one the province view shows, or `null`), `visible`, `path` (the view's widget path while it is visible, else `null`) |

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
- **Errors** (`error`, with a `detail` for people): `bad_command` (a bad
  option, or a word after `province` other than `open` and `goto`),
  `unknown_province`, `unknown_country`, `not_in_game`, `main thread busy`
  (no frame within `timeout=`, for example while a save loads),
  `read_failed` (a read of game memory faulted or a guard failed: an entry's
  own ID, a country's own tag; later reads keep working) and `unavailable`
  (main-thread commands are not available).

```powershell
.\scripts\tongs.ps1 'province 1754'
# {"ok":true,"command":"province","id":1754,"name":"Murom","owner":"RUS","controller":"RUS","cores":["RUS"],
#  "base_tax":4,"base_production":7,"base_manpower":4,"development":15,"land":true,"wasteland":false,"in_game":true,...}
.\scripts\tongs.ps1 'country SWE'
# {"ok":true,"command":"country","tag":"SWE","exists":false,"province_count":0,"provinces":[],"capital":1,"observer":false,...}
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
  view shows it and the view's window is visible. The answer carries
  `province`, `view` (the widget path of that window, `provinceview` on the
  default interface) and `covered_by` (the widget the game hovers at the
  view's centre when it is not part of the view, such as an event window, or
  `null`; Tongs moves the pointer there to ask, as `ui click` does, and
  hands it back). Being covered is not a refusal. When the frame after the
  call does not show it, the answer is `not_shown` with what it saw
  (`selected`, `view_province`, `visible`), and the call still counts.
- **The camera** (both): the command waits until the camera's position
  reaches its target, then answers `camera: {"settled": true, "target":
  {"x": …, "z": …}}` (map coordinates). At the timeout it answers `ok` with
  `settled: false`. With the camera locked (`debug_lockcamera`), `goto`
  refuses `camera_locked`; `open` still opens and answers `"locked": true`.
- **`timeout=S`**: default 10 s, up to 60, for the whole command; `main
  thread busy` when no outer frame comes in time (nothing was called).
- **Opening a view can change the game.** The game's province view makes the
  player discover the capital of a foreign owner whose capital the player
  has not discovered. Tongs checks that case before calling: without
  `game_calls = true` it refuses `needs_game_calls`; with it, the call counts
  as one game call only (in `game_calls`, not `client_calls`), takes the
  console's ironman and multiplayer refusals, and the answer carries
  `"game_call": true`.
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
  | `camera_locked` | `goto` | `debug_lockcamera` is on |
  | `needs_game_calls` | `open` | the discover case above without `game_calls = true` |

  An uncolonised province is opened like any other; read its owner with
  `province ID`. A fault in the game's code answers `faulted` and turns
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
  anywhere.
- Names ignore case and take `*` and `?`.
- Filters in brackets: `[2]` (the second match, from 1), `[type=button]`
  (`window`, `button`, `icon`, `text`, `listbox`, `checkbox`,
  `editbox`, `other`), `[text=...]`, `[text~=...]` (contains),
  `[text^=...]` (starts with), `[enabled]`, `[disabled]`, `[checked]`,
  `[unchecked]`. A step made only of filters means any name:
  `speed_controls/[type=button]`.
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
widget, checks that the game sees that widget (or one inside it) under the
pointer, and only then clicks. It refuses without clicking: `not_found`,
`ambiguous` (with up to 10 `matches`), `not_visible` (only hidden widgets
match), `stale` (only stale widgets match), `disabled`, or `covered` (with `covered_by`, the widget that would get
the click). The answer has `clicked`, `point` and the state after the click.
When the game reports no widget at all under the pointer, the move is tried up
to three times (`hover_attempts`). A click on an event's option button also
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
`number`, `id` (the script id, e.g. `flavor_rus.1`), `title`, `description`,
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
resumed after the choice. Every choice counts in `inputs`.

## Notifications

`subscribe KINDS [date=day|month|year]` turns the connection into a stream:
Tongs sends a line whenever something changes, without being asked. `KINDS`
is `all` or a list such as `state,date,event`:

| Kind | Sent when |
| --- | --- |
| `state` | the game is entered or left (`change`: `entered_game` with `session`, `left_game` with `reason`; a load is `left_game` then `entered_game`), paused or resumed (`paused`, `resumed`) or the speed changes (`speed`, with `previous_speed`); each with `speed`, `paused`, `paused_flag`. Also while a save loads |
| `date` | a new day, month (the default) or year begins, with `unit` and `previous` |
| `event` | an event window opens (`opened`, with `id`, `title` and `options`) or closes (`closed`, with `chosen`: the option, with `by` saying who chose it: `tongs` for `event choose`, `run_until events=first` or `ui click` on the option button (`via: "ui click"`), `player` for any other click on an option (the real mouse, a plain `click`), `auto` for the game's own choice at the deadline; `null` when nobody's choice is known) |
| `error_log` | the game writes a `line` to its `error.log`; `restarted` when the file was replaced |
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
- While subscribed, the connection answers `ping` and `unsubscribe` and
  refuses everything else (`subscribed`). `unsubscribe` answers with `sent`,
  `dropped` and `discarded` (still queued, not sent) and the connection takes
  ordinary requests again.
- A client that reads slowly does not hold anyone up: each subscription keeps
  up to 1000 notifications, and when that is full the oldest are dropped and
  `{"notification":"overflow","dropped":N}` comes before the next one.
- A subscription holds one of the 8 pipe instances, so use a connection of its
  own for it.

## Console

`console LINE` runs a console command the way the game's console does, but
without the console window: the line is split on spaces and passed to the
game's command dispatcher on its main thread, after a frame of the main loop.
It needs `game_calls = true` and a loaded game (`in_game: true`), and takes
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
  unless `game_calls_ironman = true` or `game_calls_multiplayer = true`; with
  either switch on, answers carry `mode`;
- `suspended`: Anvil suspended Tongs in this ironman or multiplayer game (see
  [Safety](../README.md#safety)), before any of Tongs' own switches are asked;
- `game_calls_disabled`, `not_in_game`, `unknown_command`, `bad_command`
  (printable ASCII only), `console_unavailable`;
- `game_calls_tripped`: a game call that faulted answers `faulted`, and every
  later one is refused until the game exits. Reads keep working.

The game's own checks still apply: developer-only commands answer "Command
available only for developers." in `text`.
