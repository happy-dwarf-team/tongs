# Speed and pause press the top bar's buttons through their own calls

Status: Accepted (2026-11-02)

`pause`, `resume` and `speed` pressed Space and keypad `+` / `-`. In a
multiplayer game they timed out ("no change within 10000 ms") while clicking
the top bar's buttons worked, so scripts fell back to finding the buttons and
clicking pixels. In `eu4.exe` 1.37.5 the buttons and the keys end in the same
functions, but the keys get there only through the session's key handler,
which in multiplayer with more than one player does not pause on Space
(docs/design.md, "Speed buttons"). So Tongs now presses the buttons: on the
main thread, after an outer frame, it makes the call each button's handler
makes. It stays **input**: it is what a player's click on the button does,
the game's own guards run, and in multiplayer the call posts the same pause
and speed commands as the click.

## Considered Options

- Keep the keys and press the Pause key in multiplayer: rejected, it covers
  only the pause, and the speed keys did not reach the game either.
- Click the widgets (`ui click speed_controls/button_speedup`): rejected, it
  moves the pointer, needs the top bar visible and uncovered (the pause
  button lies under the date), and goes through the same handlers anyway.
- Set the speed and the pause byte directly: rejected, it skips the game's
  guards, and in multiplayer the other players would never see it.
- A client call (ADR 0003): rejected, the speed and the pause are game state;
  the call is what the button does, so it counts as the button's input.

## Consequences

- **Switch and counting:** as before, the commands need `input = true` and
  each press counts in `inputs`; `run_until` does not count its own presses.
- **Ironman and multiplayer:** Tongs adds no refusal of its own; Anvil's
  `allow_ironman` and `allow_multiplayer` still apply. Whether the player may
  change the speed in a multiplayer game is the game's own check.
- **Guards:** while the byte the buttons check is set, Tongs calls nothing and
  answers an error instead of waiting for a change. A press that raises a
  structured exception turns the buttons off until the game exits.
- **Fallback:** without the main thread the commands press the keys, as
  Tongs always did. Answers say which with `via`.
- **Symbols:** the three functions and the button's guard are in
  `plugin/symbols.toml`, so Anvil skips the plugin on a build where they
  changed.
