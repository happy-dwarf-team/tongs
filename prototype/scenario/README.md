# PROTOTYPE: what does a scenario look like? (tongs#104)

Throwaway. Never merged. It answers one question from the map
[#94](https://github.com/happy-dwarf-team/tongs/issues/94): what does a lab scenario look like
to the agent or person who writes one?

Two real lab tests, each written twice, as **TOML** (declarative) and as **Python** (code):

| Test | TOML | Python |
| --- | --- | --- |
| Moscow (295) is Russian on 17 March 1719 and `province open` shows it | `scenarios/moscow-owner.toml` | `scenarios/moscow-owner.py` |
| SIMULATION: warm-up to 1720.1.1, measured year to 1721.1.1, then a world read | `scenarios/simulation-year.toml` | `scenarios/simulation-year.py` |
| A **marked** run: answers events inside its stretch on purpose | | `scenarios/simulation-year-events.py` |

Everything runs against `fakegame.py`, which answers commands Tongs does not have yet
(`province`, `country`, `player`, `selection`, `province open`, `client_calls`). `ready()` is a
stub (its predicate is [#109](https://github.com/happy-dwarf-team/tongs/issues/109)). Nothing
starts or touches a game.

```sh
python runner.py needs scenarios/simulation-year.py      # what run-scenario.ps1 reads before the start (JSON)
python runner.py run scenarios/moscow-owner.toml
python runner.py run scenarios/simulation-year.py --fault event-pause      # stretch rejected
python runner.py run scenarios/simulation-year.toml --fault old-tongs      # missing count -> rejected
python runner.py run scenarios/simulation-year.py --fault main-thread-moved  # lab check fails
python runner.py run scenarios/moscow-owner.toml --fault slow-view         # read-until polls
python runner.py run scenarios/simulation-year-events.py                   # marked, still passes
```

## What it shows (from [#103](https://github.com/happy-dwarf-team/tongs/issues/103))

1. **Launch needs as data, read before the start.** TOML: the `[launch]` table. Python: the
   module-level `LAUNCH` / `MARKED` literals, read with `ast` so no scenario code runs.
   Either way PowerShell gets them as JSON from `python runner.py needs FILE`
   (PowerShell has `ConvertFrom-Json` but no TOML parser):

   ```powershell
   $needs = python -I scripts/scenario/runner.py needs $Scenario | ConvertFrom-Json
   Invoke-GameLaunch ... -Prepare { <install Tongs with $needs.launch.plugins.tongs> } `
       -Start { <continue=<file of $needs.launch.start.save>, mask of $needs.launch.cpu> } `
       -Step { param($game) python -I scripts/scenario/runner.py run $Scenario --pid $game.Id --start-ms ... }
   ```

2. **Measured stretches.** A wait is setup unless it names a stretch (`measure = "year-1720"`).
   A stretch is valid only when it reached its date unpaused and answers `inputs`,
   `client_calls`, `game_calls` all 0 (a missing field rejects), and `client_calls_total` /
   `game_calls_total` in `state` did not move. Setup may use input, client and game calls.
3. **Marked runs** say so up front (`MARKED = "why"` / `marked = "why"`): their stretches record
   the counts, are `marked`, never `valid`, and the run still passes or fails on its assertions.
4. **One launch per scenario**: a file is one launch; `start` is a save (lab alias) or the menu.
5. **Verdicts** as in #103: `passed`, `failed` (an assertion), `rejected` (conditions), `error`.

## Where the shapes differ (look for these)

- **Assertions.** TOML can only say "these fields equal these values" (nested by subset).
  "295 is in Russia's provinces", "RUS is among the cores", "the stretch starts at the warm-up
  date or the day after" need code. Python says them in one line each.
- **Lab rules that are not one read** (both pools at N workers, masks) become a *named lab check*
  in TOML (`check = "simulation-threads"`) and a helper in Python (`s.lab.simulation_threads()`).
  Either way the rule lives in the runner, not in the scenario.
- **Read once vs read until.** One read after a wait is `read` + `expect`; polling is the same
  step with `within_s` (TOML) or `within_s=` (Python). No separate "wait for" verb.
- **Branching** (the old drivers' "resume, else press Return") has no TOML form at all. The
  prototype's answer is that a scenario does not branch on pauses: a pause in a stretch rejects
  it, and a scenario that must answer events uses `events=first` and is marked.
- **Launch needs** cost the same: a table vs a literal dict.

## Open (for the grilling)

- TOML, Python, or Python with launch needs as literals (as here)?
- Save as a lab alias (`russia-1719`) or a file name? Mask as a lab profile (`cpu = "simulation"`)
  or explicit masks and workers?
- Where scenario files live in the lab, and whether "scenario" is a Tongs or a lab glossary term.
