# Changelog

## [0.19.0](https://github.com/happy-dwarf-team/tongs/compare/v0.18.0...v0.19.0) (2026-10-28)


### Features

* sessions, client-call counts and the public client with ready() ([#115](https://github.com/happy-dwarf-team/tongs/issues/115)) ([18d3852](https://github.com/happy-dwarf-team/tongs/commit/18d3852457c7bf22cd6f5c81c2b9d575fa04ce83))

## [0.18.0](https://github.com/happy-dwarf-team/tongs/compare/v0.17.0...v0.18.0) (2026-10-27)


### Features

* the definition line of each widget in tongs.ui ([#92](https://github.com/happy-dwarf-team/tongs/issues/92)) ([0379d5c](https://github.com/happy-dwarf-team/tongs/commit/0379d5c11b99df589df8c1092d4c0e972da8747e))

## [0.17.0](https://github.com/happy-dwarf-team/tongs/compare/v0.16.0...v0.17.0) (2026-10-26)


### Features

* a hovered call cheap enough for every frame ([#90](https://github.com/happy-dwarf-team/tongs/issues/90)) ([365e37e](https://github.com/happy-dwarf-team/tongs/commit/365e37edee2a9da740397c57777a1a32f947da7d))
* register the widget tree as the service tongs.ui version 1 ([#88](https://github.com/happy-dwarf-team/tongs/issues/88)) ([f63142e](https://github.com/happy-dwarf-team/tongs/commit/f63142e0bebdea2a0a046e5ebec535d392f1ee92))
* the definition file of each widget in tongs.ui ([#91](https://github.com/happy-dwarf-team/tongs/issues/91)) ([6efac0f](https://github.com/happy-dwarf-team/tongs/commit/6efac0f7da2cf22f3d1b3d1728e40a4997601442))

## [0.16.0](https://github.com/happy-dwarf-team/tongs/compare/v0.15.0...v0.16.0) (2026-10-25)


### Features

* publish Tongs as a Steam Workshop mod ([#76](https://github.com/happy-dwarf-team/tongs/issues/76)) ([4947cbc](https://github.com/happy-dwarf-team/tongs/commit/4947cbc5ab19d287491df74164974a49166357e8))

## [0.15.0](https://github.com/happy-dwarf-team/tongs/compare/v0.14.1...v0.15.0) (2026-10-25)


### Features

* **events:** name the player's own choices and the deadline auto-selection in chosen ([#71](https://github.com/happy-dwarf-team/tongs/issues/71)) ([ef06fd1](https://github.com/happy-dwarf-team/tongs/commit/ef06fd1035cfc73698b1d0a9e90290fcb68f6cdf))


### Bug Fixes

* **input:** let the pointer settle before scroll turns the wheel ([#75](https://github.com/happy-dwarf-team/tongs/issues/75)) ([2832828](https://github.com/happy-dwarf-team/tongs/commit/28328285c3854806d40ef0bd9912e0171fe48737))
* **ui:** fit widget rects to what the game draws ([#74](https://github.com/happy-dwarf-team/tongs/issues/74)) ([bfaf024](https://github.com/happy-dwarf-team/tongs/commit/bfaf0248518ef89f8858da3e0c6b6da61b55495b))
* **ui:** read the children of overlapping elements boxes and grid boxes ([#72](https://github.com/happy-dwarf-team/tongs/issues/72)) ([68309f1](https://github.com/happy-dwarf-team/tongs/commit/68309f1b9f209eff55fbf7b1b6a596ecca6bd15a))

## [0.14.1](https://github.com/happy-dwarf-team/tongs/compare/v0.14.0...v0.14.1) (2026-10-21)


### Bug Fixes

* follow pointer fields to live button definitions ([#63](https://github.com/happy-dwarf-team/tongs/issues/63)) ([1b456d1](https://github.com/happy-dwarf-team/tongs/commit/1b456d1544f81b292f9acd9e5371fb46c3796fea))

## [0.14.0](https://github.com/happy-dwarf-team/tongs/compare/v0.13.0...v0.14.0) (2026-10-20)


### Features

* mark stale widgets and never call through a freed definition ([#61](https://github.com/happy-dwarf-team/tongs/issues/61)) ([b8dd6b3](https://github.com/happy-dwarf-team/tongs/commit/b8dd6b31015a0c3875652fb0b1183807528b777b))

## [0.13.0](https://github.com/happy-dwarf-team/tongs/compare/v0.12.0...v0.13.0) (2026-10-20)


### ⚠ BREAKING CHANGES

* Tongs needs Anvil 1.0 (plugin API 2) with anvil-game; settings move from `config.toml` to `settings.toml` (migrated once), and `console_allow` is a string of names.

### Features

* migrate to Anvil plugin API 2 ([#58](https://github.com/happy-dwarf-team/tongs/issues/58)) ([ba1cc96](https://github.com/happy-dwarf-team/tongs/commit/ba1cc967a574d8cfe0b2fbea0a14e39102bf9d56))


### Build

* take the Anvil SDK from the 1.0.0 release ([#60](https://github.com/happy-dwarf-team/tongs/issues/60)) ([76d1bce](https://github.com/happy-dwarf-team/tongs/commit/76d1bce54d5afb389b6151545a07e18f038373a3))

## [0.12.0](https://github.com/happy-dwarf-team/tongs/compare/v0.11.0...v0.12.0) (2026-10-19)


### Features

* **state:** report the game process id and start time with the state ([#54](https://github.com/happy-dwarf-team/tongs/issues/54)) ([4dbdc4e](https://github.com/happy-dwarf-team/tongs/commit/4dbdc4ed7538a73796f71ec80ddd03e18b3a002e))


### Bug Fixes

* **mcp:** declare tool effects truthfully and report cancelled calls ([#55](https://github.com/happy-dwarf-team/tongs/issues/55)) ([dcd06b5](https://github.com/happy-dwarf-team/tongs/commit/dcd06b59dcd391cfc3ca4afa13d3251a52e62638))

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
