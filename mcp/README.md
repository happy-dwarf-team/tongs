# Tongs MCP server

An [MCP](https://modelcontextprotocol.io) server that exposes the Tongs pipe
(`\\.\pipe\tongs`) as typed tools, so agents such as Claude Code can drive a
running Europa Universalis IV without scripts. It speaks MCP over **stdio
only** (no network listener) and runs on the same Windows machine as the game.

The server adds no power the pipe does not have. Every tool sends one Tongs
command and returns the plugin's answer unchanged; the plugin's config still
decides what is allowed: `enabled = true` for the pipe, `input = true` for
keyboard and mouse input, `game_calls = true` for `console`. See the
[Tongs README](../README.md) for the commands and their answers.

## Requirements

- Windows, EU4 1.37.5 with [Anvil](https://github.com/happy-dwarf-team/anvil)
  and Tongs installed, `enabled = true` in `anvil/data/tongs/config.toml`.
- [uv](https://docs.astral.sh/uv/) (recommended), or Python 3.11+ with pip.

The server starts without a game; tools then answer with an error until the
pipe is there.

## Install

Clone the Tongs repository (or download the source of a release) and let uv
create the environment from the lock file, which pins every dependency:

```powershell
git clone https://github.com/happy-dwarf-team/tongs
cd tongs\mcp
uv sync --frozen
```

The server is then `uv run --frozen --directory C:\path\to\tongs\mcp tongs-mcp`.

Without a clone, uv can run a release tag directly. This pins only the direct
dependency (`mcp==2.3.0`); the rest is resolved at install time:

```powershell
uvx --from "git+https://github.com/happy-dwarf-team/tongs@v0.9.0#subdirectory=mcp" tongs-mcp
```

With pip instead of uv (direct dependency pinned only):

```powershell
py -3.11 -m venv .venv
.venv\Scripts\pip install .
# the server: .venv\Scripts\tongs-mcp.exe
```

### Claude Code

```powershell
claude mcp add --transport stdio --scope user tongs -- uv run --frozen --directory C:\path\to\tongs\mcp tongs-mcp
```

`--scope` is `local` (default: you, this project), `project` (a `.mcp.json`
shared with the repository) or `user` (you, every project). Everything after
`--` is the server command. The same as a project `.mcp.json`:

```json
{
  "mcpServers": {
    "tongs": {
      "type": "stdio",
      "command": "uv",
      "args": ["run", "--frozen", "--directory", "C:/path/to/tongs/mcp", "tongs-mcp"]
    }
  }
}
```

### Other MCP clients

Any client that starts stdio servers takes the same command and arguments,
e.g. Claude Desktop's `claude_desktop_config.json` (same `mcpServers` block as
above), VS Code's `.vscode/mcp.json` (`"servers"` instead of `"mcpServers"`),
or the MCP Inspector:

```powershell
npx @modelcontextprotocol/inspector uv run --frozen --directory C:\path\to\tongs\mcp tongs-mcp
```

## Tools

| Tool | Command | Notes |
| --- | --- | --- |
| `tongs_state`, `tongs_window`, `tongs_threads` | `state`, `window`, `threads` | read only |
| `tongs_frames` | `frames [timeout=S]` | read on the main thread |
| `tongs_screenshot` | `screenshot [NAME]` | answers with `path`; `image=true` attaches the PNG when small enough |
| `tongs_wait_until` | `wait_until DATE [timeout=S] [max_pause=S]` | long; progress (below) |
| `tongs_run_until` | `run_until DATE [speed=N] [timeout=S] [max_pause=S] [events=first\|stop]` | input; long; progress |
| `tongs_cancel` | `cancel` | ends every wait in progress |
| `tongs_pause`, `tongs_resume`, `tongs_speed`, `tongs_key`, `tongs_text` | `pause`, `resume`, `speed N`, `key NAME`, `text STRING` | input |
| `tongs_move`, `tongs_click_point`, `tongs_scroll`, `tongs_release` | `move X Y`, `click X Y [button] [double]`, `scroll STEPS [X Y]`, `release` | input |
| `tongs_ui_find`, `tongs_ui_tree` | `ui find SEL ...`, `ui tree [SEL] ...` | read on the main thread |
| `tongs_ui_click` | `ui click SEL [timeout=S]` | input |
| `tongs_event_list` | `event list [timeout=S]` | read on the main thread |
| `tongs_event_choose` | `event choose N\|text^="..." [event=...] [timeout=S]` | input |
| `tongs_console` | `console LINE` | **game call**: changes game state, needs `game_calls = true` |
| `tongs_raw` | any one line | escape hatch; same config switches apply; no `subscribe` |

- **Answers.** The text content is the plugin's answer line exactly as sent;
  the same object is in `structuredContent`. An answer with `"ok": false` is
  returned with `isError: true`, so the agent sees the plugin's `error` field.
  Errors of the adapter itself (no pipe, a refused argument) are plain text.
- **Input** tools work only with `input = true` and, like the pipe commands,
  count in `inputs` of every `wait_until` / `run_until` in progress.
  `tongs_console` works only with `game_calls = true`, changes game state and
  counts in `game_calls` and `game_calls_total`: the run becomes a marked run
  that measurements reject. The tool descriptions say so, and the tool
  annotations mark the reads `readOnlyHint` and `tongs_console` /
  `tongs_raw` `destructiveHint`.
- **Arguments** are checked before anything is sent: values with a line break
  or NUL are refused (one request is one line), event texts may not contain
  `"`, and numbers keep to the plugin's ranges.
- **Connections.** Each call opens its own pipe connection and closes it after
  the answer, so one tool call can wait in `run_until` while another reads
  `tongs_state` or sends `tongs_cancel`. A wait with progress uses a second
  connection; the pipe has 8.

## Long waits

`tongs_wait_until` and `tongs_run_until` return only when the date is reached,
the wait's `timeout` passes (plugin default 1 hour, at most 4 hours),
`max_pause` ends it, or something cancels it. The client's own limits must be
longer:

- **Progress.** When the client sends a progress token, the server opens a
  second connection with `subscribe date date=day` and reports the game days
  done (of the days to the target) as MCP progress, at most twice a second,
  with the game date as the message. `progress=false` turns this off, e.g. to
  keep a measurement to one pipe connection.
- **Heartbeat.** While no new game day comes (the game is paused, by the
  player or by a dialog), the server sends `ping` on the progress connection
  every `TONGS_MCP_HEARTBEAT_S` seconds (default 60; `0` turns it off) and, if
  the plugin answers, reports progress anyway with the message
  `game date D, no new day for N s`. The MCP spec requires every progress
  value to be larger than the one before, so each heartbeat adds 0.001 days;
  the 240 heartbeats of the longest wait (4 hours at the default 60 s) add less
  than a day, and the next new day reports the real count again. A paused
  wait therefore stays alive until its own `timeout`; to end it early
  instead, give `max_pause`.
  If the game or the plugin stops answering, the heartbeat stops, and the
  client's idle timeout applies again.
- **Claude Code** gives a stdio tool call 100 000 000 ms (about 28 hours) by
  default (`MCP_TOOL_TIMEOUT`, or `"timeout"` per server in `.mcp.json`), and
  aborts a call whose server sends neither an answer nor progress for 30
  minutes (`CLAUDE_CODE_MCP_TOOL_IDLE_TIMEOUT`); progress, heartbeats included,
  resets that idle timer. A call running longer than 2 minutes moves to a
  background task. Without progress (`progress=false`, or a client that sends
  no progress token) a wait that is paused for 30 minutes is aborted; give it a
  `max_pause` below that.
- **MCP Inspector** times requests out after 10 s by default; raise its
  request timeout and maximum total timeout and turn on "reset timeout on
  progress" in its configuration.
- **Python SDK** clients: `Client(..., read_timeout_seconds=...)` (no limit by
  default).
- **Cancelling** the tool call (the client's `notifications/cancelled`) makes
  the server send `cancel`, which ends **every** wait in progress, like
  `tongs_cancel`.

## Screenshots

`tongs_screenshot` always returns the plugin's answer with the PNG's `path`
(in the game's `anvil/data/tongs/screenshots/`). With `image=true` the PNG is
also attached as image content when it is at most `TONGS_MCP_MAX_IMAGE_BYTES`
bytes (default 3 000 000; set the variable in the server's environment);
otherwise a second text block says why it was left out. A 3440×1440 window
gives about 8 MB, so on large screens read the file at `path` instead.

## Development

```powershell
cd mcp
uv run --frozen pytest      # fake pipe, no game needed; also runs in CI
uvx ruff format src tests   # style (line length 120)
```

`src/tongs_mcp/tongs.py` is a verbatim copy of [`scripts/tongs.py`](../scripts/tongs.py),
so the package works on its own; a test fails when the two differ. The
version follows the Tongs release: release-please updates `pyproject.toml` and
`uv.lock` with `version.txt`.

[`live_check.py`](live_check.py) drives a loaded game through the server over
stdio, as an MCP client would. It presses keys, runs the game for a month and
makes one game call, so use it only on a test session.
[`live_check_events.py`](live_check_events.py) checks the heartbeat on a
paused wait, then runs the game until events open and chooses options by
number and by text; it changes the game the same way, without game calls.

## Live check (2026-10-19)

EU4 1.37.5 (the lab's known `eu4.exe` hash), Tongs 0.8.0 built from `main`
with `enabled`, `input` and `game_calls` on, the Russia save of 1719.3.17
started by the lab launcher (`anvil-play.ps1 -Scenario tongs -TongsInput
-TongsGameCalls`), process on CPUs 1–7. `live_check.py` with MCP Python SDK
2.3.0 over stdio:

| Step | Result |
| --- | --- |
| `list_tools` | 24 tools |
| `tongs_state`, `tongs_window`, `tongs_threads`, `tongs_frames`, `tongs_raw ping` | answers passed through unchanged (`ping`: version 0.8.0) |
| `tongs_screenshot image=true` | `path` absolute in the game folder; 7.8 MB PNG not attached (over 3 MB), note added; with the limit at 20 MB a second run attached it (`image/png`, 10.7 MB base64) |
| `tongs_ui_find speed_controls/button_speedup` | 1 match, `click_point` (3350, 33) |
| `tongs_ui_click` speed up, then speed down | speed 2 → 3 → 2, `hovered` = the clicked button |
| `tongs_event_list` | no open events |
| `tongs_run_until` 30 days, `speed=5 max_pause=30 events=first` with a progress callback | reached 1719.4.16, 11.1 days/s, `inputs` 0, `game_calls` 0; 5 progress notifications (4/30 … 25/30 days); the tool call took 3.0 s against `wall_ms` 2708: the presses before the run and the progress stream's last read (up to 0.5 s) |
| `tongs_wait_until` 10 years ahead, then `tongs_cancel` from a concurrent call | the wait answered `cancelled` after 1.5 s |
| MCP call of `tongs_wait_until` cancelled by the client after 1.5 s | the server sent `cancel`: a `wait_until` of another pipe client (`scripts/tongs.py`) ended `cancelled` after 2.0 s |
| `tongs_console echo x` | `text` "x", `game_calls_total` 0 → 1 |
| `tongs_text` with a line break | refused by the adapter, nothing sent |

After the session: save unchanged, settings difference `graceful_exit` only
(restored), nothing left installed. `tongs_event_choose` was not checked in
this session: no event opened in 30 days (see the second session below).

### Events and the heartbeat

A second session the same day, set up as above but with `input` on and
`game_calls` off (Tongs 0.8.0 build, plugin unchanged in 0.9.0). The script was
[`live_check_events.py`](live_check_events.py), whose server ran with
`TONGS_MCP_HEARTBEAT_S=2`, then one more `tongs_event_choose` call through the
same server:

| Step | Result |
| --- | --- |
| `tongs_wait_until` 10 years ahead with progress, game paused, `tongs_cancel` after 7.5 s | 3 heartbeats 2 s apart, progress 0, 0.001, 0.002 of 3650 days, message `game date 1719.3.17, no new day for 2 s` (4 s, 6 s); the wait answered `cancelled`, `paused_ms` 7512 |
| `tongs_run_until` in 30-day steps (`speed=5 max_pause=10`), `tongs_event_list` after each | first event on 1720.10.20 after 19 steps: `colonial_nation.112` "Tariffs on Salt", 2 enabled options; the events did not pause the game |
| `tongs_event_choose option=1` | "Tax it!" chosen, `closed` true after 61 ms (3 frames), call 188 ms |
| next step to 1720.11.19 | 3 events open: `ideagroups.2000`, `parlaments.1`, `innovativeness_events.14` |
| `tongs_event_choose option_text="Alas, some may have to pay..." event=1` | option 1 of `ideagroups.2000` chosen, `closed` true after 55 ms, 2 events left |
| `tongs_event_choose option_text="Landowners have too many privileges already." option_match=equals event_title="Physiocrats" event_match=equals` | option 2 of the lower event (`innovativeness_events.14`, number 2) chosen, `closed` true after 60 ms, 1 event left |

The run lasted 2.4 minutes; the launcher closed the game after checking PID,
path and creation time. Save unchanged, settings difference `graceful_exit`
only (restored), nothing left installed. Not checked live here: Claude Code
itself as the client (see the next section), the MCP Inspector, a wait longer
than the idle timeouts (the heartbeat was checked at 2 s, not against a real
30-minute idle timer).

### Claude Code as the client (2026-10-19)

The hardening pass of 0.10.0 (`docs/design.md`, "Hardening pass (0.10.0)")
drove the game from a Claude Code session in the Claude desktop app (Code
tab), with the server registered at user scope as `tongs` (`claude mcp add
--transport stdio --scope user tongs -- uv run --frozen --directory …
tongs-mcp`, as above), status Connected. Lab runs `tongs-harden-1` … `-4`.

- **Tools.** All 24 `tongs_*` tools were there as `mcp__tongs__tongs_*`.
  The session loads MCP tools on demand: right after start the server was
  still "connecting", and a tool search for "tongs" waited for it and
  returned the schemas. No `claude mcp get` was needed.
- **Used:** `state`, `window`, `threads` (through `raw`), `frames`,
  `ui_tree`, `ui_find`, `ui_click` (dozens of clicks, menus, dialogs, list
  rows), `scroll`, `move`, `release`, `key`, `text`, `speed`, `resume`,
  `pause`, `run_until`, `event_list`, `event_choose`, `console`,
  `screenshot` (path only; the PNGs were 4–8 MB) and `raw`. Not used:
  `wait_until`, `cancel`, `click_point`, `screenshot image=true`.
- **Answers.** Each answer came back as the plugin's JSON text. `"ok": false`
  answers came back as tool errors carrying the same JSON, so the error
  fields (`covered_by`, `hover_attempts`, `queued_ms`) were readable. When
  the game exited, the next call failed with "the pipe closed without an
  answer …; did the game exit?".
- **Long calls.** `run_until 1721.1.1 speed=5 max_pause=5` ran 49 s
  (520 days, 10.5 days/s) and `run_until 1723.1.1` ended after 15.7 s with
  "the game stayed paused longer than max_pause" as a tool error, both within
  one tool call; no client timeout was reached. No progress was shown in the
  transcript.
- **Not checked from Claude Code:** waits longer than a few minutes, and
  cancelling a tool call from the client.
