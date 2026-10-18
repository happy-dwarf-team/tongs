# Changelog

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
