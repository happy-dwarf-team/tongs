# Changelog

## [0.11.0](https://github.com/happy-dwarf-team/tongs/compare/v0.10.0...v0.11.0) (2026-10-19)


### Features

* check box state, edit box text, ui click choices and option scrolling ([#52](https://github.com/happy-dwarf-team/tongs/issues/52)) ([4aade21](https://github.com/happy-dwarf-team/tongs/commit/4aade2107d846f5908860b6e736ba369cfd0365e))


### Bug Fixes

* reset game_calls_total on a reloaded save and harden ui click ([#47](https://github.com/happy-dwarf-team/tongs/issues/47)) ([808ea25](https://github.com/happy-dwarf-team/tongs/commit/808ea259ba26b9dd6d14773977ab84def78505c9))

## [0.10.0](https://github.com/happy-dwarf-team/tongs/compare/v0.9.0...v0.10.0) (2026-10-19)


### Features

* **mcp:** keep paused waits alive with a progress heartbeat ([#45](https://github.com/happy-dwarf-team/tongs/issues/45)) ([8a19a83](https://github.com/happy-dwarf-team/tongs/commit/8a19a8357639a080df9646e46b660da8df2b0542))

## [0.9.0](https://github.com/happy-dwarf-team/tongs/compare/v0.8.0...v0.9.0) (2026-10-19)


### Features

* **mcp:** add an MCP server that exposes the pipe as tools ([#43](https://github.com/happy-dwarf-team/tongs/issues/43)) ([7c4abb5](https://github.com/happy-dwarf-team/tongs/commit/7c4abb596b77df6bc619a9c8de469d6b525ef22f))

## [0.8.0](https://github.com/happy-dwarf-team/tongs/compare/v0.7.0...v0.8.0) (2026-10-19)


### Features

* notifications with subscribe and 8 pipe instances ([#41](https://github.com/happy-dwarf-team/tongs/issues/41)) ([ba70972](https://github.com/happy-dwarf-team/tongs/commit/ba709724fdf6a0ad874ea03b24a51dc62fb0d2d6))

## [0.7.0](https://github.com/happy-dwarf-team/tongs/compare/v0.6.0...v0.7.0) (2026-10-19)


### Features

* console game calls behind game_calls ([#39](https://github.com/happy-dwarf-team/tongs/issues/39)) ([79cd25a](https://github.com/happy-dwarf-team/tongs/commit/79cd25a2a2626524750919e958bd7c01ca735d1f))

## [0.6.0](https://github.com/happy-dwarf-team/tongs/compare/v0.5.0...v0.6.0) (2026-10-19)


### Features

* ui click, event list and event choose, run_until events=first and inputs ([#37](https://github.com/happy-dwarf-team/tongs/issues/37)) ([d677e36](https://github.com/happy-dwarf-team/tongs/commit/d677e36d0d49fd3d42f2f5bc22f5adaa6e15b042))

## [0.5.0](https://github.com/happy-dwarf-team/tongs/compare/v0.4.0...v0.5.0) (2026-10-18)


### Features

* widget tree with ui tree and ui find ([#35](https://github.com/happy-dwarf-team/tongs/issues/35)) ([ff66091](https://github.com/happy-dwarf-team/tongs/commit/ff660918573c96fada5f4058f7a2ec8f3586396c))

## [0.4.0](https://github.com/happy-dwarf-team/tongs/compare/v0.3.0...v0.4.0) (2026-10-18)


### Features

* main-thread dispatcher and frames ([#33](https://github.com/happy-dwarf-team/tongs/issues/33)) ([93c8a0d](https://github.com/happy-dwarf-team/tongs/commit/93c8a0d7e6c39bb3f1e5e4ab46aaa37d212ae288))

## [0.3.0](https://github.com/happy-dwarf-team/tongs/compare/v0.2.0...v0.3.0) (2026-10-17)


### Features

* max_pause for run_until and wait_until ([#11](https://github.com/happy-dwarf-team/tongs/issues/11)) ([ed89bee](https://github.com/happy-dwarf-team/tongs/commit/ed89beec5d39179187014d9089758222d4406570))

## [0.2.0](https://github.com/happy-dwarf-team/tongs/compare/v0.1.0...v0.2.0) (2026-10-17)


### Features

First public release. Off by default: `enabled = true` in
`anvil/data/tongs/config.toml` (created on the first start) opens the local pipe
`\\.\pipe\tongs` for the Windows user running the game; `input = true` also allows
the keyboard and mouse commands, which add one hook on the interface's cursor query.
Includes the read-only `ping`, `state` and `help` of the unreleased 0.1.0. Checked in
the game in the lab (see docs/design.md). Requires Anvil 0.2.0 or newer; supported game
build: EU4 1.37.5 (Steam, Windows 64-bit), other builds are skipped by Anvil.

* **client:** several commands per connection, a Python client and the console key ([#7](https://github.com/happy-dwarf-team/tongs/issues/7)) ([f8d123e](https://github.com/happy-dwarf-team/tongs/commit/f8d123eea255daa8aecb50773956117635682233))
* pause, resume, speed and key commands through the game's SDL input ([#2](https://github.com/happy-dwarf-team/tongs/issues/2)) ([20d2f60](https://github.com/happy-dwarf-team/tongs/commit/20d2f60059824663bbc765a95caa4aef64e6bd94))
* run_until, wait_until and cancel; serve several clients at once ([#3](https://github.com/happy-dwarf-team/tongs/issues/3)) ([a948b37](https://github.com/happy-dwarf-team/tongs/commit/a948b3708b00b9960101904671b1bdfbd2ced74b))
* screenshot command writing a PNG of the game window ([#6](https://github.com/happy-dwarf-team/tongs/issues/6)) ([31327ec](https://github.com/happy-dwarf-team/tongs/commit/31327ecd78b188d80173b64f9592f77b11eb5ba7))
* text, move, click, scroll and window commands with a virtual GUI cursor ([#4](https://github.com/happy-dwarf-team/tongs/issues/4)) ([d8b46c7](https://github.com/happy-dwarf-team/tongs/commit/d8b46c791cdd0bf8051bdcb6bcbc9619c765ef4d))
* threads command with the worker pools and CPU masks ([#5](https://github.com/happy-dwarf-team/tongs/issues/5)) ([f3055ca](https://github.com/happy-dwarf-team/tongs/commit/f3055ca177311787eeacd1cbae628c186eca3d41))


### CI

* automate releases with release-please ([#9](https://github.com/happy-dwarf-team/tongs/issues/9)) ([11d5c3b](https://github.com/happy-dwarf-team/tongs/commit/11d5c3bc3d4dc64e3d22ab53834a31dca803c361))
