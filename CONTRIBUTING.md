# Contributing to Tongs

The conventions are the same as in
[Anvil](https://github.com/happy-dwarf-team/anvil/blob/main/CONTRIBUTING.md);
this file lists what differs.

## Pull requests and titles

Every change goes to `main` through a squash-merged pull request whose title is
a [Conventional Commit](https://www.conventionalcommits.org/) (checked by the
`pr-title` workflow); the title becomes the commit on `main` and the changelog
entry. Types and their effect on the version:

| Type                                  | Release | Changelog                          |
| ------------------------------------- | ------- | ---------------------------------- |
| `feat`                                | minor   | Features                           |
| `fix`, `perf`, `revert`               | patch   | Bug Fixes / Performance / Reverts  |
| `build`                               | patch   | Build                              |
| `docs`, `refactor`, `test`, `ci`, `chore` | none | hidden                           |

A release is proposed only when the changelog would get a visible entry;
commits of hidden types alone never start one and ship with the next release.

Scopes: the area of the plugin (`pipe`, `state`, `input`, `cursor`, `window`,
`threads`, `screenshot`, `client`), `config`, `plugin` (manifest, packaging).
A new command is a `feat`; a command that sends input to the game says so in
the description. Breaking changes (a command or answer field renamed or
removed, a config key renamed, a different Anvil requirement) take `!` and a
`BREAKING CHANGE:` paragraph; before 1.0.0 they bump the minor version.

A change to the addresses or code patterns the plugin relies on updates the
`[[expect]]` entries in `plugin/plugin.toml` and [docs/design.md](docs/design.md),
and says how it was checked in the game.

## Versions and releases

[release-please](https://github.com/googleapis/release-please) maintains a
`chore(main): release x.y.z` pull request that bumps `version.txt`, the
`version` in `plugin/plugin.toml` and `CHANGELOG.md`. Merging it tags `vX.Y.Z`
and creates the GitHub release; the release workflow then builds the tag and
attaches `tongs-windows-x64.zip` (the `tongs/` plugin folder) and
`SHA256SUMS.txt` with a build provenance attestation.

Do not edit `version.txt`, the manifest `version` or `CHANGELOG.md` by hand;
CMake refuses to configure when `version.txt` and `plugin/plugin.toml`
disagree. To force a version, put `Release-As: x.y.z` in a commit body.

## Building and testing

```powershell
cmake -S . -B build -G "Visual Studio 17 2022" -A x64
cmake --build build --config Release
ctest --test-dir build -C Release --output-on-failure
```
