# Changelog

## [0.31.0](https://github.com/happy-dwarf-team/tongs/compare/v0.30.0...v0.31.0) (2026-11-10)


### Features

* **input:** drill, split and merge units through anvil-game.commands ([#180](https://github.com/happy-dwarf-team/tongs/issues/180)) ([cf8fafd](https://github.com/happy-dwarf-team/tongs/commit/cf8fafdc56a401940aeb79a0fb8e741e5cb1d478))

## [0.30.0](https://github.com/happy-dwarf-team/tongs/compare/v0.29.0...v0.30.0) (2026-11-10)

### Features

* **input:** with Anvil 1.17.0 or later, `run_until events=first` chooses the first option of the player's events through Anvil's `anvil-game.commands` service, the same way a click on the button does, instead of clicking the window. This also works with the game window minimised, where clicks cannot get through. With an older Anvil, or when the service refuses, Tongs clicks as before. `event list` now gives each event's `pending_id`. Tested in the lab: on Steam 1.37.5 eight events in a row were chosen through the service, two of them with the window minimised, and one on GOG 1.37.5.2. ([#175](https://github.com/happy-dwarf-team/tongs/issues/175))

## [0.29.0](https://github.com/happy-dwarf-team/tongs/compare/v0.28.0...v0.29.0) (2026-11-09)

### Features

* **state:** `state` and every answer that carries it report the current map mode as `map_mode`, the game's own number. ([#172](https://github.com/happy-dwarf-team/tongs/issues/172))
* **state:** with Anvil 1.16's `anvil-map`, `map_mode_id` names the plugin map mode on screen by its id, or is `null` in the game's own modes. The new `map_mode MODE` command (`tongs_map_mode` in MCP) shows a map mode by number or by a plugin mode's id, as the minimap's button does; it needs `input = true` and counts as a client call. Tongs runs as before without `anvil-map`. Tested in the lab on GOG 1.37.5.2 with `anvil-map` and three plugin modes. ([#173](https://github.com/happy-dwarf-team/tongs/issues/173))

## [0.28.0](https://github.com/happy-dwarf-team/tongs/compare/v0.27.0...v0.28.0) (2026-11-09)

### Features

* **plugin:** Tongs runs on the GOG build 1.37.5.0, the version GOG Galaxy installs when you roll EU4 back. It needs Anvil 1.15.0 or later, which knows that build. Tested in the lab on a copy of a GOG 1.37.5.0 install together with Bellows, Hamweard and Mei. ([#167](https://github.com/happy-dwarf-team/tongs/issues/167))

### Bug Fixes

* **ui:** Chinese, Japanese and Korean text from a mod such as Mei now comes back readable from `ui find`, `ui tree` and the text reads. Before, it came back garbled (`å›½åº“` instead of `国库`). Text of the base game reads as before. ([#168](https://github.com/happy-dwarf-team/tongs/issues/168))
* **ui:** clicks by widget name work at any GUI scale. At a raised `game_ui_scale`, `ui click` used to fail with `covered`; widget rectangles and click points are now in window pixels, and the answers carry `ui_scale`. ([#169](https://github.com/happy-dwarf-team/tongs/issues/169))
* **input:** `ui click` gets through widgets the game itself lets the click pass. ([#165](https://github.com/happy-dwarf-team/tongs/issues/165))
* **input:** while the game window is minimized, keys, clicks and text are refused with an error instead of waiting in the game's queue until the window comes back. Reads and `run_until` work as before. ([#170](https://github.com/happy-dwarf-team/tongs/issues/170))

## [0.27.0](https://github.com/happy-dwarf-team/tongs/compare/v0.26.1...v0.27.0) (2026-11-08)


### Features

* **state:** report out-of-sync state ([#156](https://github.com/happy-dwarf-team/tongs/issues/156)) ([40cd905](https://github.com/happy-dwarf-team/tongs/commit/40cd905ae45fc17fef8d36b218d20984224c1644))


### Bug Fixes

* **pipe:** close the windows after a save load instead of opening the menu ([#155](https://github.com/happy-dwarf-team/tongs/issues/155)) ([681c31c](https://github.com/happy-dwarf-team/tongs/commit/681c31c007ad33078e2026e38d629e130692b963))

## [0.26.1](https://github.com/happy-dwarf-team/tongs/compare/v0.26.0...v0.26.1) (2026-11-05)


### Bug Fixes

* **client:** refuse a pipe another Windows user created under this name ([#151](https://github.com/happy-dwarf-team/tongs/issues/151)) ([2939b42](https://github.com/happy-dwarf-team/tongs/commit/2939b42e49e1fadcc592ae52d82e781033e40219))
* **pipe:** report every save load as a game call ([#149](https://github.com/happy-dwarf-team/tongs/issues/149)) ([3772845](https://github.com/happy-dwarf-team/tongs/commit/3772845b1b47022997373021c8a7f55b2c0c4731))

## [0.26.0](https://github.com/happy-dwarf-team/tongs/compare/v0.25.1...v0.26.0) (2026-11-02)


### ⚠ BREAKING CHANGES

* **plugin:** run on the GOG build 1.37.5.2 ([#137](https://github.com/happy-dwarf-team/tongs/issues/137))

### Features

* **plugin:** run on the GOG build 1.37.5.2 ([#137](https://github.com/happy-dwarf-team/tongs/issues/137)) ([ec95b19](https://github.com/happy-dwarf-team/tongs/commit/ec95b194e37df8f8ffd09255df1726c6fa2071a3))

## [0.25.1](https://github.com/happy-dwarf-team/tongs/compare/v0.25.0...v0.25.1) (2026-11-02)


### Bug Fixes

* **input:** pause, resume and speed press the speed buttons, also in multiplayer ([#134](https://github.com/happy-dwarf-team/tongs/issues/134)) ([e2b0c05](https://github.com/happy-dwarf-team/tongs/commit/e2b0c0540fdb4ca1d4db904635b5252fe0b81bee))

## [0.25.0](https://github.com/happy-dwarf-team/tongs/compare/v0.24.0...v0.25.0) (2026-11-01)


### Features

* error_log notifications carry the counts of anvil-log-throttle ([#131](https://github.com/happy-dwarf-team/tongs/issues/131)) ([c37247c](https://github.com/happy-dwarf-team/tongs/commit/c37247cf3c69a352ce355a55238f3bba96e81ff4))

## [0.24.0](https://github.com/happy-dwarf-team/tongs/compare/v0.23.0...v0.24.0) (2026-11-01)


### Features

* tongs.navigation, a service that selects a province for other plugins ([#129](https://github.com/happy-dwarf-team/tongs/issues/129)) ([3c7b5e9](https://github.com/happy-dwarf-team/tongs/commit/3c7b5e9917339674a1c8406a6eac7f0206639b87))

## [0.23.0](https://github.com/happy-dwarf-team/tongs/compare/v0.22.0...v0.23.0) (2026-11-01)


### Features

* **ui:** answer a bad selector with the selector grammar ([#125](https://github.com/happy-dwarf-team/tongs/issues/125)) ([a5bbae1](https://github.com/happy-dwarf-team/tongs/commit/a5bbae10911c63bb80e6609d40e926c43cf2679f))


### Bug Fixes

* **input:** run_until events=first tries a choice that did not take again ([#128](https://github.com/happy-dwarf-team/tongs/issues/128)) ([6acaeec](https://github.com/happy-dwarf-team/tongs/commit/6acaeecf290d50a923c2976d62bf64b03b18b106))

## [0.22.0](https://github.com/happy-dwarf-team/tongs/compare/v0.21.0...v0.22.0) (2026-10-31)


### Features

* save load, a game call that loads a save by name ([#122](https://github.com/happy-dwarf-team/tongs/issues/122)) ([126e221](https://github.com/happy-dwarf-team/tongs/commit/126e221002dea9677a1acfc66ac80ffb6bc8d82c))
* the text with its colour codes of each widget in tongs.ui ([#123](https://github.com/happy-dwarf-team/tongs/issues/123)) ([a742ebe](https://github.com/happy-dwarf-team/tongs/commit/a742ebed989a8d317ca261fd2ac6b9627d210798))

## [0.21.0](https://github.com/happy-dwarf-team/tongs/compare/v0.20.0...v0.21.0) (2026-10-31)


### Features

* province open and province goto, client calls that navigate ([#120](https://github.com/happy-dwarf-team/tongs/issues/120)) ([5b74e61](https://github.com/happy-dwarf-team/tongs/commit/5b74e61cd001db8326a950b6a742f62c04a3ee13))

## [0.20.0](https://github.com/happy-dwarf-team/tongs/compare/v0.19.0...v0.20.0) (2026-10-31)


### Features

* world reads province, country, player and selection ([#118](https://github.com/happy-dwarf-team/tongs/issues/118)) ([24648aa](https://github.com/happy-dwarf-team/tongs/commit/24648aaed0dfcd7d3a32034153cbf111a897c0b1))

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
