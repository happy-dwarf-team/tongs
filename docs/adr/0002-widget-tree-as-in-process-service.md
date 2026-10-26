# Tongs offers the widget tree to other plugins as `tongs.ui`, and nothing else

Status: Proposed (2026-10-26)

Anvil leaves knowledge that only some plugins need (the GUI tree, the console,
text) in those plugins and their services (Anvil's `docs/design.md` §4), and
under plugin API 2 a service version only grows and its documented behaviour is
part of the contract (Anvil's ADR 0001). Of what Tongs knows, only the widget
tree is worth that contract now. It is the deepest part: the registry, fifteen
widget kinds told apart by vtable, a child container per class, widgets held by
the game's code, and stale widgets, where one wrong call ends the process
through `_purecall` and no `__try` catches it. It also has a consumer: the
pick mode of Etch 2.0 needs the widget under the cursor with its ancestors and,
by its research (`research/pick-mode`, claims 7–11 and 16), would copy Tongs'
reader into Etch's own plugin. So Tongs registers one service, `tongs.ui`
version 1: main-thread reads of the widget tree, with the selectors and widget
paths of the pipe. Input, events, game calls, state and notifications stay on
the pipe.

## The service

A C header, `include/tongs/tongs_ui.h`, plain C11 like Anvil's `anvil_game.h`,
in the repository and in the release archive; consumers vendor it. A sketch:

```c
#define TONGS_UI_SERVICE "tongs.ui"
#define TONGS_UI_SERVICE_VERSION 1

/* Flags of a call. */
#define TONGS_UI_HIDDEN 0x1u /* also hidden widgets, as the pipe's `all` */
#define TONGS_UI_STALE  0x2u /* also stale widgets (never clickable) */

/* Flags of a widget. */
#define TONGS_UI_VISIBLE 0x1u /* the widget and all its ancestors */
#define TONGS_UI_ENABLED 0x2u /* buttons; others always set */
#define TONGS_UI_CHECKED 0x4u /* check boxes */
#define TONGS_UI_IS_STALE 0x8u
#define TONGS_UI_HOVERED 0x10u /* the topmost widget under the pointer */

typedef enum TongsUiType { /* the pipe's `type` */
    TONGS_UI_WINDOW = 1, TONGS_UI_BUTTON, TONGS_UI_ICON, TONGS_UI_TEXT,
    TONGS_UI_LISTBOX, TONGS_UI_CHECKBOX, TONGS_UI_EDITBOX, TONGS_UI_OTHER
} TongsUiType;

typedef struct TongsUiWidget {
    uint32_t size;      /* sizeof(TongsUiWidget) as known by Tongs */
    uint32_t type;      /* TongsUiType */
    uint32_t flags;
    int32_t parent;     /* index of the parent in this call's order, -1 at the top */
    int32_t x, y, width, height; /* client pixels of the game window */
    uint64_t id;        /* identity within this call only */
    const char *name;   /* UTF-8; "" when the widget has no usable name */
    const char *path;   /* the widget path, as the pipe gives it */
    const char *text;   /* UTF-8 without colour codes; NULL without text */
} TongsUiWidget;

/* Called once per widget; return 0 to stop. `widget` and its strings are
 * valid only during the call: copy what you keep. */
typedef int (*TongsUiVisit)(void *context, const TongsUiWidget *widget);

typedef struct TongsUiService {
    uint32_t size;
    /* The widgets the selector names, in the order of the pipe's `ui find`,
     * each after its ancestors. *count (may be NULL) gets the number of
     * matches. ANVIL_ERROR_INVALID_ARGUMENT for a bad selector,
     * ANVIL_ERROR_WRONG_PHASE off the main thread. */
    AnvilResult (*find)(const char *selector, uint32_t flags, TongsUiVisit visit,
                        void *context, uint32_t *count);
    /* The hovered widget, then its ancestors up to its top-level window.
     * Nothing visited when nothing is hovered. Errors as find. */
    AnvilResult (*hovered)(uint32_t flags, TongsUiVisit visit, void *context);
} TongsUiService;
```

What the header promises, and so the contract:

- **Thread.** Only on the game's main thread, after an outer frame: from a
  handler of `anvil-game.frame` or from work queued with `anvil-game`'s
  `run_on_main_thread`. A call from another thread answers
  `ANVIL_ERROR_WRONG_PHASE` and reads nothing. Calls are synchronous; each
  reads the tree once, which took 6–7 ms for about 16 000 widgets (live,
  0.5.0), so it is not meant for every frame.
- **Borrowed data.** What a visit receives lives for that visit, as an
  `anvil-game.frame` payload lives for its handler. This is not a pointer
  "handed out" in the sense of Anvil's lifetime rule, and the header says so.
- **Selectors and paths** are the pipe's ([design](../design.md), "Widget
  tree"). A new filter may be added within version 1, because it only turns
  `ANVIL_ERROR_INVALID_ARGUMENT` into an answer; a changed meaning of a
  selector, a path or a field needs version 2, registered next to version 1.
- **Stale widgets** are left out unless `TONGS_UI_STALE` asks for them, and
  then come without their definition's name, as on the pipe.
- **Switches.** The service is registered on every load whose symbols Anvil
  verified, whatever `enabled`, `input` and `game_calls` say: it only reads,
  sends no input, makes no game call and counts in neither `inputs` nor
  `game_calls`. Today Tongs does nothing with `enabled = false`; it would then
  register the service and still open no pipe and install no hook.
- **Suspension.** While Anvil suspends Tongs (ironman, multiplayer), the
  service keeps reading, as the pipe's reads do.

The pipe's `ui find` / `ui tree` and the service are two adapters of one
module: the main-thread read (`src/game_ui.cpp`) and the view over it
(`src/ui_tree.cpp`, `UiView`) stay as they are; the pipe turns a view into
JSON, the service into `TongsUiWidget` visits. Paths, rectangles and text are
the same in both, and a fix to the reader fixes both.

## What stays on the pipe

- **Input** (`ui click`, `event choose`, keys, the mouse): it goes through
  SDL's queue and the cursor hook over several frames, under the input lock,
  and counts for marked runs. A plugin in the process has the window itself
  (`anvil-overlay` subclasses it).
- **Game calls** (`console`): the knowledge is shallow (Etch's plugin calls the
  dispatcher with three symbols and about 130 lines), while the risk is not.
  Through a service, a plugin with `affects_gameplay = false` could change the
  simulation, Tongs cannot see the caller's manifest, and ADR 0001's refusals,
  ironman checks and counts were made for pipe clients.
- **Events** (`event list`): a main-thread read like the tree, but no plugin
  needs it now. When one does, it gets its own service, `tongs.events`.
- **State** (date, speed, pause): knowledge any plugin may need, close to what
  `anvil-game` already reads for its session event; it belongs there, as an
  addition to `anvil-game.game`, if Anvil wants it.
- **Notifications**: in the process they would be Anvil events; none until a
  consumer needs one.

## Considered Options

- **A library plugin for the widget tree** (no pipe, `affects_gameplay =
  false`), on which Tongs and Etch both depend: consumers would not install a
  gameplay plugin with a control pipe to read the interface. Not now: a new
  repository, release and Workshop item for one consumer, and Tongs' reader
  would move out of Tongs. It is the right shape once a public plugin on the
  Workshop wants the tree; Tongs would then hand `tongs.ui` over by depending
  on it.
- **Shared source**: Etch builds Tongs' reader into its own DLL. No runtime
  contract, but both plugins declare the same symbols and a fix needs two
  releases.
- **Etch copies the reader**, as its research proposes: the duplicate is the
  most fragile code Tongs has (stale widgets).
- **Every part as a service** (`tongs.ui`, `tongs.events`, `tongs.console`,
  input): rejected for the reasons in "What stays on the pipe".
- **No service at all**: the pipe stays the only way in, and Etch keeps pick
  mode in its own plugin. This is the decision if Etch does not take Tongs
  into its private install (below).

## Consequences

- **Etch decides.** Its private install loads only the plugins its manifest
  lists, which is why its research puts pick mode in Etch's plugin (claim 16).
  Using `tongs.ui` means listing Tongs there and declaring a dependency on
  `tongs`; without that the service has no consumer and this ADR is rejected.
- **Fields Etch needs that Tongs does not read**: the definition's file and
  line (`+0x30` and `+0xA0` of the definition). They are appended to
  `TongsUiWidget` within version 1 once Etch's live check H2 settles the line
  base, or are in the first release if it is checked before.
- **Versions.** The service version is apart from Tongs' version: Tongs is
  before 1.0.0 and may still break pipe commands in a minor release, while
  `tongs.ui` version 1 follows Anvil's ADR 0001 from its first release.
  Consumers depend on the first Tongs release that registers it.
- **`affects_gameplay`** stays `true` in Tongs' manifest because of game
  calls. A consumer declares its own; depending on Tongs does not make it a
  gameplay plugin, but players in multiplayer see Tongs in the hash of the
  gameplay plugin set.
- **Live check** before the release (the repository's rule for the tree): the
  same paths and rectangles from the service and from `ui find` in one frame,
  `hovered` against `mgr+0x130`, a call from another thread refused, and
  `enabled = false` with the service working and no pipe.
- **Questions for the review of this ADR:**
  1. Will Etch take Tongs into its private install for pick mode?
  2. Tongs or a library plugin, given who else might want the tree?
  3. Main thread only, or any thread with a limit, as the pipe waits?
  4. Selectors in version 1, or only `hovered` and a full walk?
  5. Is an `id` that holds only within one call enough for pick mode, which
     follows the pointer across frames?
