# Province navigation as an in-process service, beside `input`

Status: Accepted (2026-11-01)

Etch's live game (an Anvil plugin in the game's process, like Tongs) needs to
bring the province view back on the province the player was editing: after it
restarts the game, and after a reload of `provinceview.gui`, whose rebuild
leaves the province's name blank until the view is refreshed. Tongs' pipe has
`province open` (ADR 0003), but Etch's private install loads Tongs with
`enabled = false` and `input = false`, so there is no pipe and no client call.
Etch reads Tongs only through `tongs.ui` (ADR 0002). Etch could call the
game's selection itself, but then two plugins would hold the same game
addresses and the same rules (terra incognita, the session mode, the tutorial
guard, the discover-capital case), and only Tongs' are checked live. So Tongs
offers its navigation to other plugins in the process: the service
`tongs.navigation` version 1, with `province_view` (what the selection and the
province view hold) and `open_province` (the province links' calls).

## Considered Options

- Each consumer calls the game's selection itself: rejected, a second copy of
  14 addresses and of rules that Tongs read in the game's code and checked live
  (`research/province-navigation`, tongs#96, and the 0.21.0 live check).
- `open_province` only with `input = true`, as the pipe's client calls:
  rejected. `input` is the player's switch for what clients outside the
  process may do; a consumer plugin is installed beside Tongs (by the player
  or by an installer such as Etch's) and answers for its own calls, as it
  would if it called the game itself. Etch also keeps `input` off, so that no
  pipe client can act in its game.
- The pipe's promotion of the discover case to a game call: rejected for the
  service. A game call needs `game_calls = true` (ADR 0001), a switch of the
  pipe; the service has no such switch, so it never makes one.
- Adding the calls to `tongs.ui`: rejected, its contract says it only reads.

## Consequences

- **Switches:** Tongs registers `tongs.navigation` on every load, whatever
  `enabled`, `input` and `game_calls` say, as it does `tongs.ui`.
- **What it calls:** `open_province` makes the client call of the pipe's
  `province open` (ADR 0003), each step optional: close a shown session view
  (`TONGS_NAV_KEEP_VIEWS` leaves it), move the camera (`TONGS_NAV_NO_CAMERA`
  selects only, as a click on the map does), select. The checks are the pipe's:
  every refusal comes before anything is called.
- **No game calls:** the case that would post the discover-capital command is
  refused with `TONGS_NAV_DISCOVER`.
- **Ironman and multiplayer:** while Anvil suspends Tongs, `open_province`
  refuses with `TONGS_NAV_SUSPENDED`; `province_view` reads on, like
  `tongs.ui`.
- **Faults:** a fault in the game's code turns navigation off for the pipe and
  the service alike until the game exits (`navigation_tripped`).
- **Counting:** with the pipe on, a call that called the game counts in
  `client_calls` and `client_calls_total`, with an `activity` notification, as
  a client call of the pipe does: a run with a consumer's navigation is a
  marked run.
- **Thread:** only on the game's main thread after an outer frame (a frame
  handler or `run_on_main_thread` work), as `tongs.ui`.
