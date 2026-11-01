/*
 * tongs.navigation v1: the province selection for other Anvil plugins
 * (consumers) inside the game's process: which province the province view
 * shows, and opening a province's view as the game's own province links do.
 * Tongs registers it on every load whose symbols Anvil verified, whatever its
 * settings say (also with enabled = false and input = false, which open no
 * pipe and send no input). Decision and contract: Tongs' docs/adr/0004,
 * design in docs/design.md ("The tongs.navigation service").
 *
 * open_province is a client call (docs/adr/0003): game code that changes only
 * the camera, the selection and the open views. It refuses what a player could
 * not do in the interface (terra incognita, a sea, a selection the game
 * blocks), and it never makes a game call: when opening the view would post
 * the discover-capital command, it refuses with TONGS_NAV_DISCOVER and calls
 * nothing. It counts in `client_calls` of Tongs' pipe when the pipe is on.
 * While Anvil suspends Tongs (ironman, multiplayer) it refuses with
 * TONGS_NAV_SUSPENDED; province_view keeps reading.
 *
 * Look it up with get_service(self, TONGS_NAVIGATION_SERVICE,
 * TONGS_NAVIGATION_SERVICE_VERSION) after declaring a dependency on "tongs"
 * in plugin.toml; it is NULL with a Tongs older than 0.24.0, and a consumer
 * then works on without it. Vendor this header.
 *
 * Thread. Call only on the game's main thread, after an outer frame: from a
 * handler of anvil-game's ANVIL_GAME_EVENT_FRAME or from work queued with
 * anvil-game's run_on_main_thread. A call from another thread answers
 * ANVIL_ERROR_WRONG_PHASE and reads or calls nothing. Calls are synchronous;
 * what open_province calls shows from the next frame on, which a consumer
 * checks with province_view then.
 *
 * Cost. province_view reads a few fields of the game's selection object and
 * calls the game's own "is a view shown" test: cheap enough for every frame.
 *
 * Identity. view_id and colonisation_id are the window widgets' addresses,
 * the `id` tongs.ui gives the same widgets.
 *
 * Plain C11, like Anvil's anvil_game.h. Within version 1 fields are only
 * appended to the end of TongsProvinceView and functions to the end of
 * TongsNavigationService; check with TONGS_NAV_VIEW_HAS / TONGS_NAV_HAS.
 * An outcome or a flag added within version 1 is a new value; a consumer
 * treats an outcome it does not know as a refusal.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef TONGS_NAVIGATION_H
#define TONGS_NAVIGATION_H

#include <anvil/anvil_plugin.h>

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define TONGS_NAVIGATION_SERVICE "tongs.navigation"
#define TONGS_NAVIGATION_SERVICE_VERSION 1

/* Flags of TongsProvinceView. */
#define TONGS_NAV_VIEW_SHOWN 0x1u         /* the province view's window is shown */
#define TONGS_NAV_COLONISATION_SHOWN 0x2u /* the colonisation panel the game shows for a province without an owner */
#define TONGS_NAV_VIEWS_OPEN 0x4u         /* one of the session's views (country view, ledger, ...) is shown */

typedef struct TongsProvinceView {
    uint32_t size;            /* sizeof(TongsProvinceView) as known by Tongs */
    uint32_t flags;           /* TONGS_NAV_VIEW_SHOWN ... */
    int32_t selected;         /* the province the selection holds, 0 for none; it stays when the view closes */
    int32_t shown;            /* the province the province view shows, 0 for none; it stays when the view closes */
    uint64_t view_id;         /* the province view's window widget, 0 before the view is built */
    uint64_t colonisation_id; /* the colonisation panel's window widget, 0 before it is built */
} TongsProvinceView;

/* Nonzero when `view` (a const TongsProvinceView*) carries `field`. */
#define TONGS_NAV_VIEW_HAS(view, field) ((view)->size >= offsetof(TongsProvinceView, field) + sizeof((view)->field))

/* Flags of open_province. Any other bit answers ANVIL_ERROR_INVALID_ARGUMENT. */
#define TONGS_NAV_KEEP_VIEWS 0x1u /* leave a shown session view open; the province links close it first */
#define TONGS_NAV_NO_CAMERA 0x2u  /* select only, as a click on the map does: the camera stays */

/* What open_province did. */
typedef enum TongsNavigationOutcome {
    TONGS_NAV_OPENED = 0,           /* the game's calls were made: the view shows the province from the next frame */
    TONGS_NAV_NOT_IN_GAME = 1,      /* no game is loaded */
    TONGS_NAV_UNKNOWN_PROVINCE = 2, /* not 1 <= id < the number of provinces */
    TONGS_NAV_NOT_SELECTABLE = 3,   /* a sea, a lake or a wasteland */
    TONGS_NAV_TERRA_INCOGNITA = 4,  /* the player has not discovered it: the game's links do nothing */
    TONGS_NAV_SELECTION_BLOCKED = 5, /* the game selects nothing now (a province-picking mode, the tutorial) */
    TONGS_NAV_DISCOVER = 6,  /* opening it would post the discover-capital command, a game call: not made */
    TONGS_NAV_SUSPENDED = 7, /* Anvil suspended Tongs in this ironman or multiplayer game */
    TONGS_NAV_TRIPPED = 8,   /* a navigation call faulted earlier: navigation is off until the game exits */
    TONGS_NAV_FAULTED = 9,   /* the game's code raised an exception in this call; navigation is now off */
    TONGS_NAV_READ_FAILED = 10 /* what the call depends on could not be read (Tongs logs why); nothing called */
} TongsNavigationOutcome;

typedef struct TongsNavigationService {
    uint32_t size; /* sizeof(TongsNavigationService) as known by Tongs */

    /* The selection and the province view now. Outside a game every field is
     * 0 and the call answers ANVIL_OK. ANVIL_ERROR_INVALID_ARGUMENT for a NULL
     * `out` or an `out->size` smaller than the fields of version 1 (set it to
     * sizeof(TongsProvinceView) before the call); ANVIL_ERROR_WRONG_PHASE off
     * the main thread; ANVIL_ERROR_INTERNAL when the selection cannot be read
     * (Tongs logs why). Tongs writes `out->size` with its own size. */
    AnvilResult (*province_view)(TongsProvinceView *out);

    /* Opens the view of province `id` as the game's province links do: closes
     * a shown session view (not with TONGS_NAV_KEEP_VIEWS), moves the camera to
     * the province (not with TONGS_NAV_NO_CAMERA) and selects it, which shows
     * it in the province view, or in the colonisation panel without an owner.
     * Selecting the province the view shows already shows it again. *outcome
     * gets a TongsNavigationOutcome; every refusal comes before anything is
     * called. ANVIL_ERROR_INVALID_ARGUMENT for unknown flags or a NULL
     * `outcome`; ANVIL_ERROR_WRONG_PHASE off the main thread; nothing is
     * called then. */
    AnvilResult (*open_province)(int32_t id, uint32_t flags, uint32_t *outcome);
} TongsNavigationService;

/* Nonzero when `service` (a const TongsNavigationService*) provides `field`. */
#define TONGS_NAV_HAS(service, field) \
    ((service)->size >= offsetof(TongsNavigationService, field) + sizeof((service)->field))

#ifdef __cplusplus
}
#endif

#endif /* TONGS_NAVIGATION_H */
