# Game calls live in Tongs, behind their own switch and hard refusals

Tongs used to promise that it never changes game state. Agents testing mods
need game calls (console commands first), so Tongs now runs them itself, but
only with `game_calls = true` in the config, separate from `enabled` and
`input`. We did not move them to a separate plugin, because it would duplicate
the pipe and the protocol. Main-thread reads such as the widget tree are not
game calls and need only `enabled`. `plugin.toml` declares
`affects_gameplay = true`, because the plugin can change the simulation, even
though it does not by default.

## Considered Options

- A separate plugin for game calls, with Tongs kept read-only and limited to
  input: rejected, because it duplicates the transport.
- An allowlist of console commands: rejected, because there are 376 commands
  and agents testing mods need most of them.
- Relying on the game's own gates alone: rejected. They cover only the
  console, and `enable_all_commands` opens them.

## Consequences

- **Refused always, whatever the config says:**
  - `enable_all_commands`, so Tongs never opens the game's developer gate or
    its multiplayer/ironman gate;
  - commands that act on the user's account outside the game:
    `debug_achievements_clear`, `test_achievement`, `debug_achievements`,
    `social_*`, `CrashReporter.UploadCrash`.
- **Refused by default:** the other `danger` commands (crashes, leaks, desync,
  rewriting the map or the world). `console_allow = [...]` allows them one by
  one, by name.
- **Ironman and multiplayer:** Tongs refuses every game call there by itself,
  checked before each call on the main thread.
  - `game_calls_ironman = true` lifts the refusal for ironman and
    `game_calls_multiplayer = true` for multiplayer; both are off by default.
  - With either switch on, the answer carries `mode` and the log a warning.
  - The game's own console gate still applies.
- **Marked runs:**
  - `wait_until` and `run_until` report `game_calls` and `inputs`, the counts
    from all clients during the measured stretch;
  - `state` reports `game_calls_total` since the save was loaded;
  - every console command counts, whatever its class, so a measurement can
    reject a marked run.
- **Logging:** every game call and every refusal goes to the Anvil log, with
  the command, the pipe instance, the game date and the result.
