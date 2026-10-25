# Tongs: repository rules

- Anvil plugin that lets scripts and agents control EU4 1.37.5 over the local pipe
  `\\.\pipe\tongs`, plus the MCP server in `mcp/`. Input to the game stays off unless
  `input = true`. Commands in [docs/commands.md](docs/commands.md), design and addresses in
  [docs/design.md](docs/design.md), decisions in [docs/adr/](docs/adr/), terms in
  [GLOSSARY.md](GLOSSARY.md).
- [CONTRIBUTING.md](CONTRIBUTING.md) lists what differs from Anvil's conventions: titles and
  scopes, releases, the build and tests, and `plugin/symbols.toml` with its lock checked
  against the real `eu4.exe`. Run its build and `ctest` before finishing a change; CI runs
  them.
- A change to addresses, the UI tree or input is checked in the game, and the PR says how.
- English in code and docs. Do not imply affiliation with Paradox Interactive.
- The agent owns the pull request flow: it works on a branch with Conventional Commits,
  pushes it and opens the PR as a draft. Once ready (green CI, the change tested as above,
  no blocking review findings, not put on hold by the user) it marks the PR ready and
  squash-merges it; GitHub does not allow auto-merge on a draft. Pull requests from other
  contributors get a review and a recommendation; the maintainer decides on merging them.
  Do not bypass CI or branch protection, and do not change repository settings beyond what
  auto-merge needs.
