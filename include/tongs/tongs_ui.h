/*
 * tongs.ui v1: the game's widget tree, read for other Anvil plugins
 * (consumers) inside the game's process. Tongs registers it on every load
 * whose symbols Anvil verified, whatever its settings say (also with
 * enabled = false, which opens no pipe). It only reads: it sends no input,
 * makes no game call, and counts in neither `inputs` nor `game_calls` of
 * Tongs' pipe. While Anvil suspends Tongs (ironman, multiplayer) it keeps
 * reading. Decision and contract: Tongs' docs/adr/0002, design in
 * docs/design.md ("The tongs.ui service").
 *
 * Look it up with get_service(self, TONGS_UI_SERVICE,
 * TONGS_UI_SERVICE_VERSION) after declaring a dependency on "tongs" in
 * plugin.toml; it is NULL with a Tongs older than the first release that
 * registers it, and a consumer then works on without it. Vendor this header.
 *
 * Thread. Call only on the game's main thread, after an outer frame: from a
 * handler of anvil-game's ANVIL_GAME_EVENT_FRAME or from work queued with
 * anvil-game's run_on_main_thread. A call from another thread answers
 * ANVIL_ERROR_WRONG_PHASE and reads nothing, and so does a call made from
 * inside a visit. Calls are synchronous: every visit happens before the call
 * returns.
 *
 * Cost. find, and hovered with TONGS_UI_PATHS, read the whole tree (about
 * 16 000 widgets in a game, 10-14 ms): not for every frame. hovered without
 * TONGS_UI_PATHS reads only the hovered widget and its ancestors, and may run
 * every frame; ask for the paths when the hovered `id` changes.
 *
 * Identity. A widget's `id` is its address in the game. It stays the same
 * while the widget lives, across calls and frames. Once the game frees a
 * widget, a new one may get the same address.
 *
 * Borrowed data. A visit's TongsUiWidget and its strings live only for that
 * visit, as an anvil-game.frame payload lives for its handler: copy what you
 * keep. They are not pointers handed out in the sense of Anvil's lifetime
 * rule.
 *
 * Selectors and widget paths are those of Tongs' pipe (docs/design.md,
 * "Widget tree"): `a/b`, `a//b`, `//b`, names ignoring case with * and ?,
 * and the filters [n], [type=...], [text=...], [text~=...], [text^=...],
 * [enabled], [disabled], [checked], [unchecked]. A filter added within
 * version 1 only turns ANVIL_ERROR_INVALID_ARGUMENT into an answer; a changed
 * meaning of a selector, a path or a field comes as version 2, registered
 * next to version 1.
 *
 * Stale widgets (built from a .gui definition a reload freed, and everything
 * inside one) are left out unless TONGS_UI_STALE asks for them, and are never
 * worth a click: hovering one may make the game read its freed definition.
 *
 * Plain C11, like Anvil's anvil_game.h. Within version 1 fields are only
 * appended to the end of TongsUiWidget and functions to the end of
 * TongsUiService; check with TONGS_UI_WIDGET_HAS / TONGS_UI_HAS.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TONGS_UI_H
#define TONGS_UI_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TONGS_ID "tongs"
#define TONGS_UI_SERVICE "tongs.ui"
#define TONGS_UI_SERVICE_VERSION 1

/* Flags of a call. Any other bit answers ANVIL_ERROR_INVALID_ARGUMENT. */
#define TONGS_UI_HIDDEN 0x1u /* also hidden widgets and those inside hidden windows, as the pipe's `all` */
#define TONGS_UI_STALE 0x2u  /* also stale widgets */
#define TONGS_UI_PATHS 0x4u  /* hovered: also widget paths (reads the whole tree); find always gives them */

/* Flags of a widget. */
#define TONGS_UI_VISIBLE 0x1u   /* the widget and all its ancestors are shown */
#define TONGS_UI_ENABLED 0x2u   /* buttons: not disabled; set for every other widget */
#define TONGS_UI_CHECKED 0x4u   /* check boxes: ticked */
#define TONGS_UI_IS_STALE 0x8u  /* a stale widget (only with TONGS_UI_STALE) */
#define TONGS_UI_HOVERED 0x10u  /* the topmost widget under the game's pointer */
#define TONGS_UI_MATCH 0x20u    /* find: the selector names this widget (the others are its ancestors) */

typedef enum TongsUiType { /* the pipe's `type` */
    TONGS_UI_WINDOW = 1,
    TONGS_UI_BUTTON = 2,
    TONGS_UI_ICON = 3,
    TONGS_UI_TEXT = 4,
    TONGS_UI_LISTBOX = 5,
    TONGS_UI_CHECKBOX = 6,
    TONGS_UI_EDITBOX = 7,
    TONGS_UI_OTHER = 8
} TongsUiType;

typedef struct TongsUiWidget {
    uint32_t size;  /* sizeof(TongsUiWidget) as known by Tongs */
    uint32_t type;  /* TongsUiType */
    uint32_t flags; /* TONGS_UI_VISIBLE ... */
    int32_t parent; /* index of the parent among this call's visits (0 = the first), -1 for a top-level window */
    int32_t x, y, width, height; /* client pixels of the game window */
    uint64_t id;                 /* the widget's address: the same while the widget lives */
    const char *name;            /* the .gui name, UTF-8, case kept; "" when the widget has no usable name */
    const char *path;            /* the widget path, as the pipe gives it; NULL from hovered without TONGS_UI_PATHS */
    const char *text;            /* displayed text, UTF-8 without colour codes; NULL for a widget without text */
    const char *definition_file; /* the game path of the defining .gui file (e.g. "interface/topbar.gui", the
                                  * name `reload` matches, not the mod folder that supplied it); NULL for a stale
                                  * widget and when the definition is not live */
    uint32_t definition_line;    /* the 1-based line of the `{` that opens the widget's block in that file (the
                                  * line guibounds shows); 0 under the same rule as definition_file */
} TongsUiWidget;

/* Nonzero when `widget` (a const TongsUiWidget*) carries `field`. */
#define TONGS_UI_WIDGET_HAS(widget, field) \
    ((widget)->size >= offsetof(TongsUiWidget, field) + sizeof((widget)->field))

/* Called once per widget; return 0 to stop (the call then returns ANVIL_OK).
 * `widget` and its strings are valid only during the call. */
typedef int (*TongsUiVisit)(void *context, const TongsUiWidget *widget);

typedef struct TongsUiService {
    uint32_t size; /* sizeof(TongsUiService) as known by Tongs */

    /* The widgets `selector` names (UTF-8), in the order of the pipe's
     * `ui find`, each after its ancestors: a match's ancestors are visited
     * too, once, right before the first match inside them, without
     * TONGS_UI_MATCH. So every visit's parent is an earlier visit, and the
     * visits are the matches and their ancestors in the tree's walk order.
     * *count (may be NULL) gets the number of matches, also when a visit
     * stopped early. ANVIL_ERROR_INVALID_ARGUMENT for a bad selector, unknown
     * flags or a NULL visit; ANVIL_ERROR_WRONG_PHASE off the main thread or
     * inside a visit; ANVIL_ERROR_INTERNAL when the tree cannot be read
     * (Tongs logs why); nothing is visited then. */
    AnvilResult (*find)(const char *selector, uint32_t flags, TongsUiVisit visit, void *context, uint32_t *count);

    /* The hovered widget (TONGS_UI_HOVERED), then its ancestors up to its
     * top-level window: each visit's parent is the next one visited (-1 for
     * the last). Nothing is visited when nothing is hovered, or when the
     * hovered widget is hidden or stale and the flags do not ask for it.
     * Without TONGS_UI_PATHS it reads only these widgets and `path` is NULL;
     * the widgets, names, text and flags are those a call with it gives, and
     * so are the rects, except that a window without a size of its own takes
     * the bounds of the children its containers hold, not of those the
     * game's own code adds to it. Errors as find. */
    AnvilResult (*hovered)(uint32_t flags, TongsUiVisit visit, void *context);
} TongsUiService;

/* Nonzero when `service` (a const TongsUiService*) provides `field`. */
#define TONGS_UI_HAS(service, field) \
    ((service)->size >= offsetof(TongsUiService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* TONGS_UI_H */
