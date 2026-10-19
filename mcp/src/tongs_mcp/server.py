"""MCP server (stdio) for the Tongs pipe.

Each tool builds one request line, sends it over its own connection to
\\\\.\\pipe\\tongs and returns the plugin's JSON answer unchanged: the answer
line as text, the parsed object as structured content, and `isError` when the
answer has `"ok": false`. Later text items can follow: notes on the outcome of
calls the client cancelled. The adapter adds nothing the pipe does not offer:
input and game calls still depend on `input` and `game_calls` in the plugin's
config.
"""

import base64
import inspect
import json
import os
import threading
from collections.abc import Callable, Iterator
from dataclasses import dataclass
from importlib.metadata import PackageNotFoundError, version
from typing import Annotated, Any, Literal, Protocol

import anyio
import anyio.to_thread
from mcp.server.mcpserver import Context, MCPServer
from mcp.server.mcpserver.exceptions import ToolError
from mcp_types import CallToolResult, ImageContent, TextContent, ToolAnnotations
from pydantic import Field

PIPE = r"\\.\pipe\tongs"
# Screenshots larger than this are not inlined (base64 adds a third).
MAX_IMAGE_BYTES = int(os.environ.get("TONGS_MCP_MAX_IMAGE_BYTES", 3_000_000))
PROGRESS_POLL_S = 0.5  # how long the progress stream reads before reporting
# With no new game day for this long, ping the plugin and report progress anyway, so clients
# with an idle timeout (Claude Code: 30 minutes) keep a paused wait; 0 turns it off.
HEARTBEAT_S = float(os.environ.get("TONGS_MCP_HEARTBEAT_S", "60"))
# Progress values must increase (MCP spec), so each heartbeat adds this many days: well
# under one day for the 240 heartbeats of the longest wait (4 hours at 60 s).
HEARTBEAT_STEP = 0.001
# A late answer is quoted in its note up to this many characters.
NOTE_ANSWER_CHARS = 2000


class Connection(Protocol):
    """What the server needs from a pipe connection (scripts/tongs.py's Tongs)."""

    def raw(self, command: str) -> str: ...

    def notifications(self, seconds: float | None = None, count: int | None = None) -> Iterator[dict[str, Any]]: ...

    def close(self) -> None: ...


def open_pipe() -> Connection:
    from .tongs import Tongs  # Windows only (msvcrt, kernel32)

    return Tongs(timeout_s=2.0)


# --- request lines -----------------------------------------------------------


def _check(*values: str | None) -> None:
    """One request is one line: refuse values that would end it early."""
    for value in values:
        if value is not None and any(c in value for c in "\r\n\0"):
            raise ToolError("arguments must not contain line breaks or NUL")


def _line(*words: Any, **options: Any) -> str:
    """Words in order, then `name=value` for options that are set."""
    parts = [str(w) for w in words if w is not None and w != ""]
    parts += [f"{k}={v}" for k, v in options.items() if v is not None]
    return " ".join(parts)


def _answer(text: str) -> CallToolResult:
    try:
        answer = json.loads(text)
    except json.JSONDecodeError:
        raise ToolError(f"the plugin's answer is not JSON: {text[:200]}") from None
    content: list[TextContent | ImageContent] = [TextContent(type="text", text=text)]
    return CallToolResult(
        content=content,
        structured_content=answer if isinstance(answer, dict) else None,
        is_error=not (isinstance(answer, dict) and answer.get("ok") is True),
    )


@dataclass
class Abandoned:
    """A call whose MCP request was cancelled while the plugin still had its line: the plugin
    may carry it out anyway, so its outcome is told in the notes of later answers."""

    line: str
    outcome: tuple[str, str] | None = None  # ("answer", the answer line) or ("failure", why), once known
    told: str | None = None  # what the notes said last: None, "pending", "answer" or "failure"


# --- game dates (EU4: 365 days, no leap years) -------------------------------

_MONTH_START = (0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334)


def game_days(date: str | None) -> float | None:
    """`1720.1.1` or `1720.1.1.12` as days (and hours) since year 0; None if not a date."""
    try:
        parts = [int(p) for p in str(date).split(".")]
        if len(parts) not in (3, 4) or not 1 <= parts[1] <= 12:
            return None
        hours = parts[3] if len(parts) == 4 else 0
        return parts[0] * 365 + _MONTH_START[parts[1] - 1] + parts[2] - 1 + hours / 24
    except (TypeError, ValueError):
        return None


# --- the server --------------------------------------------------------------

INSTRUCTIONS = """\
Tongs controls a running Europa Universalis IV 1.37.5 (with the Anvil loader) through
the local pipe \\\\.\\pipe\\tongs. Every tool sends one command and returns the plugin's
JSON answer unchanged; "ok": false answers come back as errors with an "error" field.

- Start with tongs_state (in_game, date, speed, paused, pid). Use tongs_ui_find before
  clicking, and prefer tongs_ui_click (by widget name) over tongs_click_point.
- Answers that carry the state include pid and process_start_ms: when they change, the
  game was restarted and earlier answers describe another process.
- Input tools (pause, resume, speed, key, text, mouse, ui click, event choose, run_until)
  work only with input = true in the plugin config and are counted in "inputs" of any
  wait in progress. tongs_console works only with game_calls = true, changes game
  state and is counted in "game_calls" (a marked run).
- tongs_wait_until / tongs_run_until can take hours of wall time: the call returns when
  the date is reached, the timeout passes or tongs_cancel ends it. Progress
  notifications report the game date when the client asks for progress, and a heartbeat
  while the game is paused; max_pause ends a wait the game keeps paused.
- Cancelling a call does not take it back: the plugin may still carry it out. Later
  answers then carry a note: first that its effect is unknown, then the plugin's answer
  (or why none came) once it is known.
"""

READ = ToolAnnotations(read_only_hint=True, destructive_hint=False, idempotent_hint=True, open_world_hint=False)
# Writes a PNG and deletes the oldest beyond the newest 100.
FILES = ToolAnnotations(read_only_hint=False, destructive_hint=True, idempotent_hint=False, open_world_hint=False)
# Touches nothing, but cancelling it sends `cancel`, which ends the waits of every client.
WAIT = ToolAnnotations(read_only_hint=False, destructive_hint=False, idempotent_hint=False, open_world_hint=False)
# Input that can be taken back: pause, speed, the pointer, the wheel, typing into a field.
INPUT = ToolAnnotations(read_only_hint=False, destructive_hint=False, idempotent_hint=False, open_world_hint=False)
# Input that acts as the player: a key or click can confirm a dialog, choose an option or leave the game.
PLAYER = ToolAnnotations(read_only_hint=False, destructive_hint=True, idempotent_hint=False, open_world_hint=False)
# Ends the waits of every client and withdraws their queued main-thread jobs.
CONTROL = ToolAnnotations(read_only_hint=False, destructive_hint=True, idempotent_hint=True, open_world_hint=False)
GAME_CALL = ToolAnnotations(read_only_hint=False, destructive_hint=True, idempotent_hint=False, open_world_hint=False)

INPUT_NOTE = (
    '\n\nNeeds input = true in the plugin config; counts in "inputs" of every wait_until / run_until in progress.'
)

Timeout = Annotated[
    int | None, Field(ge=1, le=600, description="Seconds to wait for a main-loop frame (plugin default 10).")
]
WaitTimeout = Annotated[
    int | None, Field(ge=1, le=14400, description="Wall-clock limit in seconds (plugin default 3600, at most 14400).")
]
MaxPause = Annotated[
    int | None,
    Field(ge=1, le=14400, description="End with an error once the game has stayed paused this many seconds."),
]
Date = Annotated[
    str,
    Field(
        pattern=r"^\d{1,4}\.\d{1,2}\.\d{1,2}(\.\d{1,2})?$",
        description="Target game date, e.g. 1720.1.1 or 1720.1.1.12.",
    ),
]
Progress = Annotated[
    bool,
    Field(
        description="Report the game date as MCP progress (when the client asks for progress) through a "
        "`subscribe date` stream on a second pipe connection. Set false to keep a measurement to one connection."
    ),
]


X = Annotated[int, Field(ge=0, description="Client x in pixels (see tongs_window, ui click_point).")]

Y = Annotated[int, Field(ge=0, description="Client y in pixels.")]

Selector = Annotated[
    str,
    Field(
        min_length=1,
        description="Widget selector: a path of .gui names such as speed_controls/button_speedup, "
        "EventWindow//event_option_button or //DateText; * and ? wildcards; filters [2], [type=button], "
        "[text~=...], [text^=...], [enabled], [disabled], [checked], [unchecked].",
    ),
]

Limit = Annotated[int | None, Field(ge=1, le=5000, description="Most widgets to return (plugin default 50 / 200).")]

All = Annotated[bool, Field(description="Include hidden widgets.")]

TextMatch = Annotated[
    Literal["starts", "equals", "contains"], Field(description="How the text is compared (case-insensitive).")
]
_MATCH = {"starts": "^=", "equals": "=", "contains": "~="}


def build_server(connect: Callable[[], Connection] = open_pipe) -> MCPServer:
    try:
        server_version = version("tongs-mcp")
    except PackageNotFoundError:  # pragma: no cover - running from a source tree
        server_version = "0"
    mcp = MCPServer("tongs", version=server_version, instructions=INSTRUCTIONS)

    def tool(annotations: ToolAnnotations, note: str = "") -> Callable[[Any], Any]:
        def register(fn: Any) -> Any:
            description = inspect.cleandoc(fn.__doc__) + note
            mcp.add_tool(fn, description=description, annotations=annotations, structured_output=False)
            return fn

        return register

    def exchange(line: str) -> str:
        try:
            conn = connect()
        except FileNotFoundError:
            raise ToolError(
                f"the pipe {PIPE} is not there: start EU4 with Anvil and Tongs, enabled = true in "
                "anvil/data/tongs/settings.toml"
            ) from None
        except ModuleNotFoundError:
            raise ToolError("the Tongs pipe client runs on Windows only") from None
        except OSError as e:
            raise ToolError(f"cannot open {PIPE} ({e}); all 8 pipe instances may be busy") from None
        try:
            return conn.raw(line)
        except (ConnectionError, OSError) as e:
            raise ToolError(
                f"the pipe closed without an answer ({e}); did the game exit? The command may or may not "
                "have been carried out."
            ) from None
        finally:
            conn.close()

    abandoned: list[Abandoned] = []
    abandoned_lock = threading.Lock()

    def late_notes() -> list[str]:
        """Notes on cancelled calls: each is told once while the plugin has not answered it and
        once when its outcome is known."""
        notes = []
        with abandoned_lock:
            for entry in list(abandoned):
                kind = entry.outcome[0] if entry.outcome else "pending"
                if kind == entry.told:
                    continue
                entry.told = kind
                if entry.outcome is None:
                    notes.append(
                        f"An earlier call `{entry.line}` was cancelled before the plugin answered; the plugin "
                        "may still carry it out, so its effect is unknown until the answer comes."
                    )
                    continue
                abandoned.remove(entry)
                text = entry.outcome[1]
                if len(text) > NOTE_ANSWER_CHARS:
                    text = text[:NOTE_ANSWER_CHARS] + "..."
                if kind == "answer":
                    notes.append(
                        f"An earlier call `{entry.line}` was cancelled; the plugin answered it afterwards: {text}"
                    )
                else:
                    notes.append(
                        f"An earlier call `{entry.line}` was cancelled and ended without an answer ({text}); "
                        "its effect is unknown."
                    )
        return notes

    def finish(result: CallToolResult) -> CallToolResult:
        """Appends the notes on cancelled calls to an answer."""
        result.content.extend(TextContent(type="text", text=note) for note in late_notes())
        return result

    async def send(line: str, *, cancel_on_abort: bool = False) -> str:
        """Sends one line on a connection of its own and returns the answer line.
        When the MCP request is cancelled, the line stays with the plugin and its outcome goes into
        the notes of later answers; with `cancel_on_abort` it also sends `cancel`."""
        entry = Abandoned(line)

        def run() -> tuple[str, str]:
            try:
                outcome = ("answer", exchange(line))
            except ToolError as e:
                outcome = ("failure", str(e))
            except Exception as e:  # noqa: BLE001 - kept for the note on a cancelled call, and raised below
                outcome = ("failure", f"{type(e).__name__}: {e}")
            with abandoned_lock:
                entry.outcome = outcome
            return outcome

        try:
            kind, text = await anyio.to_thread.run_sync(run, abandon_on_cancel=True)
        except anyio.get_cancelled_exc_class():
            with abandoned_lock:
                abandoned.append(entry)
            if cancel_on_abort:
                with anyio.CancelScope(shield=True), anyio.move_on_after(5):
                    try:
                        await anyio.to_thread.run_sync(exchange, "cancel", abandon_on_cancel=True)
                    except ToolError:
                        pass
            raise
        if kind == "failure":
            raise ToolError(" ".join([text, *late_notes()]))
        return text

    async def call(line: str) -> CallToolResult:
        return finish(_answer(await send(line)))

    async def report_dates(ctx: Context, target: float | None) -> None:
        """Reports the game date as progress until cancelled; quietly stops on any pipe trouble.
        While no new day comes (the game is paused), a heartbeat every HEARTBEAT_S seconds pings
        the plugin on the same connection and, if it answers, reports progress a tiny step on."""
        try:
            conn = await anyio.to_thread.run_sync(connect)
        except (OSError, ModuleNotFoundError):
            return
        try:
            first = json.loads(await anyio.to_thread.run_sync(conn.raw, "subscribe date date=day"))
            start = game_days(first.get("date"))
            if not first.get("ok") or start is None:
                return
            total = target - start if target is not None and target > start else None
            last = first["date"]
            sent: float | None = None  # the last progress value reported
            new_day_at = reported_at = anyio.current_time()
            while True:
                notes = await anyio.to_thread.run_sync(lambda: list(conn.notifications(seconds=PROGRESS_POLL_S)))
                dates = [n["date"] for n in notes if n.get("notification") == "date" and n.get("date")]
                now = anyio.current_time()
                if dates and dates[-1] != last and (done := game_days(dates[-1])) is not None:
                    last, new_day_at = dates[-1], now
                    value = done - start if sent is None else max(done - start, sent + HEARTBEAT_STEP)
                    message = f"game date {last}"
                elif HEARTBEAT_S > 0 and now - reported_at >= HEARTBEAT_S:
                    # `ping` is one of the two requests a subscribed connection answers.
                    if not json.loads(await anyio.to_thread.run_sync(conn.raw, "ping")).get("ok"):
                        return
                    value = 0.0 if sent is None else sent + HEARTBEAT_STEP
                    message = f"game date {last}, no new day for {now - new_day_at:.0f} s"
                else:
                    continue
                await ctx.report_progress(value, total, message)
                sent, reported_at = value, now
        except (OSError, ConnectionError, ValueError):
            return
        finally:
            conn.close()  # reads above are not abandoned, so nothing else uses it

    async def wait(line: str, date: str, progress: bool, ctx: Context) -> CallToolResult:
        wants_progress = progress and (ctx.request_context.meta or {}).get("progress_token") is not None
        if not wants_progress:
            return finish(_answer(await send(line, cancel_on_abort=True)))
        failure: ToolError | None = None
        async with anyio.create_task_group() as tg:
            tg.start_soon(report_dates, ctx, game_days(date))
            try:
                text = await send(line, cancel_on_abort=True)
            except ToolError as e:
                failure = e  # raised outside the group, which would wrap it in an ExceptionGroup
            finally:
                tg.cancel_scope.cancel()
        if failure is not None:
            raise failure
        return finish(_answer(text))

    # --- reads ---

    @tool(READ)
    async def tongs_state() -> CallToolResult:
        """Game state: in_game, and with a loaded game date ("1719.3.17"), hour, speed (1-5 as in
        the UI), paused, paused_flag (the player pause) and game_calls_total; always pid and
        process_start_ms (Unix ms) of the game process. Cheap; call it first."""
        return await call("state")

    @tool(READ)
    async def tongs_window() -> CallToolResult:
        """The game window: client width/height, screen position, focused, minimized, dpi and the
        main thread id. Click coordinates are client pixels of this window."""
        return await call("window")

    @tool(READ)
    async def tongs_threads() -> CallToolResult:
        """Worker threads in the game's two pools, the `threads` option, process and main-thread CPU
        masks, ideal CPU and priority of the main thread."""
        return await call("threads")

    @tool(READ)
    async def tongs_frames(timeout: Timeout = None) -> CallToolResult:
        """Frame statistics read on the game's main thread after a main-loop frame: frames per second,
        counts and the longest gap since the previous call. While a save loads the answer is
        `main thread busy`."""
        return await call(_line("frames", timeout=timeout))

    @tool(FILES)
    async def tongs_screenshot(
        name: Annotated[
            str | None, Field(pattern=r"^[A-Za-z0-9_.-]{1,64}$", description="Optional file name stem.")
        ] = None,
        image: Annotated[
            bool,
            Field(description=f"Also return the PNG as image content when it is at most {MAX_IMAGE_BYTES} bytes."),
        ] = False,
    ) -> CallToolResult:
        """Writes a PNG of the game window (works while it is covered) to the plugin's data folder and
        answers with its `path`, size and capture method. With image=true the picture is attached
        when small enough; otherwise read the file at `path`."""
        text = await send(_line("screenshot", name))
        result = _answer(text)
        answer = result.structured_content or {}
        if image and not result.is_error:
            path = answer.get("path")
            try:
                size = os.path.getsize(path)
                if size > MAX_IMAGE_BYTES:
                    note = f"image not attached: {size} bytes is over TONGS_MCP_MAX_IMAGE_BYTES={MAX_IMAGE_BYTES}"
                    result.content.append(TextContent(type="text", text=note))
                else:
                    with open(path, "rb") as f:
                        data = f.read()
                    result.content.append(
                        ImageContent(type="image", data=base64.b64encode(data).decode(), mime_type="image/png")
                    )
            except (OSError, TypeError) as e:
                result.content.append(TextContent(type="text", text=f"image not attached: cannot read {path!r} ({e})"))
        return finish(result)

    # --- waits ---

    @tool(WAIT)
    async def tongs_wait_until(
        date: Date,
        ctx: Context,
        timeout: WaitTimeout = None,
        max_pause: MaxPause = None,
        progress: Progress = True,
    ) -> CallToolResult:
        """Waits, without touching the game, until the game date reaches `date`, then answers with the
        timing: reached, reached_date, game_days, wall_ms, days_per_second, paused_ms, inputs and
        game_calls (input commands and game calls any client sent meanwhile; a measurement should
        reject a run with either above 0). Blocks for as long as the game takes: set the MCP
        client's tool timeout above `timeout`. tongs_cancel ends it; cancelling this call sends
        `cancel`, which ends every wait in progress."""
        return await wait(_line("wait_until", date, timeout=timeout, max_pause=max_pause), date, progress, ctx)

    @tool(PLAYER, INPUT_NOTE)
    async def tongs_run_until(
        date: Date,
        ctx: Context,
        speed: Annotated[int | None, Field(ge=1, le=5, description="Game speed 1-5 to run at.")] = None,
        timeout: WaitTimeout = None,
        max_pause: MaxPause = None,
        events: Annotated[
            Literal["first", "stop"] | None,
            Field(description="first: choose the first enabled option of every event that opens on the way."),
        ] = None,
        progress: Progress = True,
    ) -> CallToolResult:
        """Sets the speed, resumes, waits until the game date reaches `date` and pauses again; answers
        with the timing of tongs_wait_until plus start_speed, overshoot_hours and, with
        events=first, the `events` chosen. Its own presses are not counted in its `inputs`.
        Blocks for as long as the game takes (up to hours): set the MCP client's tool timeout
        above `timeout`. tongs_cancel ends it; cancelling this call sends `cancel`, which ends
        every wait in progress."""
        line = _line("run_until", date, speed=speed, timeout=timeout, max_pause=max_pause, events=events)
        return await wait(line, date, progress, ctx)

    @tool(CONTROL)
    async def tongs_cancel() -> CallToolResult:
        """Ends every wait_until / run_until in progress (of any client) and withdraws main-thread jobs
        still waiting for a frame. Does not press anything."""
        return await call("cancel")

    # --- keyboard ---

    @tool(INPUT, INPUT_NOTE)
    async def tongs_pause() -> CallToolResult:
        """Presses Space unless the player pause is already on, then waits until it is."""
        return await call("pause")

    @tool(INPUT, INPUT_NOTE)
    async def tongs_resume() -> CallToolResult:
        """Presses Space if the player pause is on, then waits until the game runs; refuses when
        something else (an event, a menu) holds the game paused."""
        return await call("resume")

    @tool(INPUT, INPUT_NOTE)
    async def tongs_speed(speed: Annotated[int, Field(ge=1, le=5, description="Target speed 1-5.")]) -> CallToolResult:
        """Presses keypad +/- until the game speed is `speed` (1-5 as in the UI); works while paused."""
        return await call(_line("speed", speed))

    @tool(PLAYER, INPUT_NOTE)
    async def tongs_key(
        name: Annotated[
            str,
            Field(
                pattern=r"^[a-z0-9_]{1,16}$",
                description="a-z, 0-9, f1-f12, space, return, escape, tab, backspace, delete, minus, equals, grave, "
                "kp_plus, kp_minus, kp_enter, up, down, left, right, home, end, page_up, page_down.",
            ),
        ],
    ) -> CallToolResult:
        """Presses one key in the game (through its SDL queue; the window needs no focus) and answers
        with the state 0.1 s later."""
        return await call(_line("key", name))

    @tool(INPUT, INPUT_NOTE)
    async def tongs_text(
        text: Annotated[str, Field(min_length=1, description="Characters U+0020-U+007E and U+00A0-U+00FF.")],
    ) -> CallToolResult:
        """Types text into the focused text field of the game (click the field first)."""
        _check(text)
        return await call("text " + text)

    # --- mouse ---

    @tool(INPUT, INPUT_NOTE)
    async def tongs_move(x: X, y: Y) -> CallToolResult:
        """Points the game's mouse at client coordinates (hover shows tooltips); the real cursor does
        not move. The game sees this point until tongs_release or until the real mouse moves."""
        return await call(_line("move", x, y))

    @tool(PLAYER, INPUT_NOTE)
    async def tongs_click_point(
        x: X,
        y: Y,
        button: Literal["left", "right", "middle"] = "left",
        double: bool = False,
    ) -> CallToolResult:
        """Moves the game's mouse to client coordinates and clicks. Prefer tongs_ui_click, which checks
        what is under the pointer first."""
        return await call(_line("click", x, y, button, "double" if double else None))

    @tool(INPUT, INPUT_NOTE)
    async def tongs_scroll(
        steps: Annotated[int, Field(description="Wheel steps; positive scrolls up (zooms in on the map).")],
        x: Annotated[int | None, Field(ge=0, description="Optional client x to scroll at (with y).")] = None,
        y: Annotated[int | None, Field(ge=0, description="Optional client y to scroll at (with x).")] = None,
    ) -> CallToolResult:
        """Turns the mouse wheel, optionally at a point."""
        if (x is None) != (y is None):
            raise ToolError("give both x and y, or neither")
        return await call(_line("scroll", steps, x, y))

    @tool(INPUT, INPUT_NOTE)
    async def tongs_release() -> CallToolResult:
        """Hands the pointer back to the real mouse after move/click/scroll."""
        return await call("release")

    # --- widgets ---

    @tool(READ)
    async def tongs_ui_find(selector: Selector, all: All = False, limit: Limit = None) -> CallToolResult:
        """Widgets matching a selector: count, truncated and matches, each with path, type, visible,
        rect and click_point in client pixels, and text / enabled where they apply."""
        _check(selector)
        return await call(_line("ui find", selector, "all" if all else None, limit=limit))

    @tool(READ)
    async def tongs_ui_tree(
        selector: Annotated[
            str | None, Field(description="Start from the matches of this selector instead of the top-level windows.")
        ] = None,
        depth: Annotated[
            int | None, Field(ge=0, le=64, description="Levels to expand; deeper ones only as children_count.")
        ] = None,
        limit: Limit = None,
        all: All = False,
    ) -> CallToolResult:
        """The interface's widget tree as nested `widgets`, from the top-level windows or from the
        matches of `selector`. Use depth=0 or 1 first; the full tree is large."""
        _check(selector)
        return await call(_line("ui tree", selector, "all" if all else None, depth=depth, limit=limit))

    @tool(PLAYER, INPUT_NOTE)
    async def tongs_ui_click(selector: Selector, timeout: Timeout = None) -> CallToolResult:
        """Clicks the centre of the one widget the selector matches, after checking that the game sees
        it under the pointer. Refuses without clicking: not_found, ambiguous (with matches),
        not_visible, disabled, covered (with covered_by)."""
        _check(selector)
        return await call(_line("ui click", selector, timeout=timeout))

    # --- events ---

    @tool(READ)
    async def tongs_event_list(timeout: Timeout = None) -> CallToolResult:
        """Open events, topmost first: number, id (e.g. flavor_rus.1), title, description, path and
        options (number on screen from 1, text, enabled, option_index, option_key, path)."""
        return await call(_line("event list", timeout=timeout))

    @tool(PLAYER, INPUT_NOTE)
    async def tongs_event_choose(
        option: Annotated[int | None, Field(ge=1, description="Option number on screen, from 1.")] = None,
        option_text: Annotated[
            str | None, Field(min_length=1, description="Choose the option by its text instead.")
        ] = None,
        option_match: TextMatch = "starts",
        event: Annotated[
            int | None, Field(ge=1, description="Event number from tongs_event_list, when several are open.")
        ] = None,
        event_title: Annotated[str | None, Field(min_length=1, description="Or the event by its title.")] = None,
        event_match: TextMatch = "starts",
        timeout: Timeout = None,
    ) -> CallToolResult:
        """Clicks an option of an open event, as a player would: by `option` number or `option_text`.
        With several events open, name one with `event` or `event_title`. Options stay disabled for
        half a second after the window opens (refused as `disabled`). Answers with choice_date,
        event, option and closed."""
        if (option is None) == (option_text is None):
            raise ToolError("give either option or option_text")
        if event is not None and event_title is not None:
            raise ToolError("give event or event_title, not both")
        _check(option_text, event_title)
        if any(v is not None and '"' in v for v in (option_text, event_title)):
            raise ToolError("texts must not contain a double quote")
        choice = str(option) if option is not None else f'text{_MATCH[option_match]}"{option_text}"'
        which = None
        if event is not None:
            which = f"event={event}"
        elif event_title is not None:
            which = f'event=title{_MATCH[event_match]}"{event_title}"'
        return await call(_line("event choose", choice, which, timeout=timeout))

    # --- game calls and the escape hatch ---

    @tool(GAME_CALL)
    async def tongs_console(
        line: Annotated[
            str, Field(min_length=1, description="One console command line, e.g. `echo hello` (printable ASCII).")
        ],
    ) -> CallToolResult:
        """Runs one console command in the game on its main thread and answers with the game's text,
        success_hint and the state afterwards. CHANGES GAME STATE. Works only with
        game_calls = true in the plugin config (and a loaded game, not ironman or multiplayer
        unless allowed); dangerous commands are refused by the plugin. Every call counts in
        "game_calls" of waits in progress and in game_calls_total: the run becomes a marked run
        that measurements reject."""
        _check(line)
        return await call("console " + line)

    @tool(GAME_CALL)
    async def tongs_raw(
        line: Annotated[str, Field(min_length=1, description="One Tongs command line, e.g. `help` or `ping`.")],
    ) -> CallToolResult:
        """Escape hatch: sends one command line to the pipe as it is and returns the answer. Use the
        typed tools when one fits; `help` lists the commands. The same config switches apply
        (input commands need input = true, `console` needs game_calls = true and changes game
        state). `subscribe` is not available here: every call uses a connection of its own."""
        _check(line)
        if line.split()[:1] in (["subscribe"], ["unsubscribe"]):
            raise ToolError("subscribe needs a connection of its own; the MCP tools report progress instead")
        return await call(line.strip())

    return mcp


def main() -> None:
    """Runs the server on stdio (no network transport)."""
    anyio.run(build_server().run_stdio_async)


if __name__ == "__main__":
    main()
