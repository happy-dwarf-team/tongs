# Security policy

Tongs is an Anvil plugin: native code that runs inside the game process and,
when enabled, accepts commands from other programs through a named pipe.
Please report security problems privately, not in public issues.

## Reporting a vulnerability

Use GitHub's private vulnerability reporting:
[Report a vulnerability](https://github.com/happy-dwarf-team/tongs/security/advisories/new).
Include the plugin and Anvil versions (from `anvil/anvil.log`), your
`anvil/data/tongs/config.toml`, how to reproduce the problem, and what an
attacker gains.

This is a volunteer project; reports are handled on a best-effort basis. We
will confirm the report, agree on a fix and a disclosure date with you, and
credit you in the advisory unless you prefer otherwise.

## Supported versions

Only the latest release receives security fixes.

## In scope

- Access to the pipe: connections from another Windows user or another
  machine, and a pipe that opens without `enabled = true`.
- Input without permission: keyboard or mouse commands that work without
  `input = true`, or game code patched without it.
- Game calls without permission: a console command that runs without
  `game_calls = true`; one that Tongs always refuses (such as
  `enable_all_commands`) or refuses by default without `console_allow`, but
  runs anyway; a game call in an ironman or multiplayer game without the
  switch that allows it; a game call that is not counted or not logged.
- Memory safety of the command parser, the state reader and the cursor hook.
- Files written outside `anvil/data/tongs/` (for example through a
  `screenshot` name).
- Release integrity: release assets that do not match `SHA256SUMS.txt` or
  their build provenance attestation.

## Out of scope

- What a program of the same Windows user can do through the pipe once you
  enable it. That is the purpose of the plugin; enable `input = true` and
  `game_calls = true` only for scripts you trust.
- Problems in Anvil itself: report them to
  [Anvil](https://github.com/happy-dwarf-team/anvil/security/advisories/new).
- Vulnerabilities in Europa Universalis IV itself; report those to Paradox
  Interactive.
