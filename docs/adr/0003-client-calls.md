# Client calls run beside input and game calls, behind `input`

Status: Proposed (2026-10-28)

Tests need to open a province's view by ID and move the camera to it instead of
finding the province on a screenshot and clicking a pixel. The game's own links
do this with two calls (camera, then select and open the view), which Tongs can
make from its main-thread dispatcher. They run game code, but change only what
the game shows. Treating them as game calls (ADR 0001) would make a mod tester
switch on state-changing console commands just to navigate; treating them as
input would break what input means (what a player's keyboard and mouse would
do). So Tongs adds a third kind, the **client call**: game code run on the main
thread after an outer frame that changes only the camera, the selection and
open views, never the state of the game, and reaches no further than a player
clicking in the interface.

## Considered Options

- Everything that runs game code is a game call: rejected, because navigating
  would need `game_calls = true` and count as a change of game state.
- Navigation is input: rejected, because input is what the player's devices
  would send; a direct call skips the interface the input would go through.
- A fourth switch `client_calls`: rejected, one more switch to remember for
  what is a player's action; `input` already means "act like a player".
- Client calls under `enabled` alone: rejected, a client that only reads would
  then move the player's camera.

## Consequences

- **Switch:** client calls work only with `input = true`; its description in
  `plugin/settings.toml` says keyboard, mouse and navigation.
- **Reach:** a client call refuses what a player could not do in the interface,
  such as a province in terra incognita or a selection the game blocks; each
  command's contract lists its refusals.
- **Promotion to a game call:** when a call would change game state, Tongs
  checks that before calling and treats it as a game call: refused with
  `needs_game_calls` without `game_calls = true`, otherwise counted and logged
  as one, with ADR 0001's ironman and multiplayer refusals. The first known
  case: opening the view of a foreign province whose owner's capital the
  player has not discovered posts a discover command.
- **Loading a save** by name replaces the session, so it is a game call under
  ADR 0001, not a client call.
- **Counting:** client calls have their own count, `client_calls` in
  `wait_until` and `run_until`, `client_calls_total` in `state` and
  `what: "client_call"` in `activity` notifications. A marked run is one with
  any game call, client call or input; clients that reject marked runs check
  all three counts.
- **Ironman and multiplayer:** client calls follow input: Tongs adds no refusal
  of its own, Anvil's `allow_ironman` and `allow_multiplayer` still apply. The
  camera and the selection are local; a call that would post a command is a
  game call and is refused there.
- **Logging:** as input, not one line per call; refusals and promotions to a
  game call go to the Anvil log.
