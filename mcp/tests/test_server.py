"""The MCP tools against a fake pipe: request lines, answers, refusals, progress, cancel."""

import itertools
import json
import threading
import time
from pathlib import Path

import anyio
import pytest
from mcp import Client

from tongs_mcp import server
from tongs_mcp.server import build_server, game_days

pytestmark = pytest.mark.anyio


@pytest.fixture
def anyio_backend():
    return "asyncio"


class FakePipe:
    """Records request lines; answers with `handler(line)` (a dict, or a str sent as it is)."""

    def __init__(self, handler=None):
        self.handler = handler or (lambda line: {"ok": True, "echo": line})
        self.sent: list[str] = []
        self.notes: list[dict] = []  # served to a subscribed connection
        self.closed = 0
        self.lock = threading.Lock()

    def connect(self):
        return FakeConnection(self)


class FakeConnection:
    def __init__(self, pipe: FakePipe):
        self.pipe = pipe

    def raw(self, command: str) -> str:
        with self.pipe.lock:
            self.pipe.sent.append(command)
        answer = self.pipe.handler(command)
        return answer if isinstance(answer, str) else json.dumps(answer)

    def notifications(self, seconds=None, count=None):
        deadline = time.monotonic() + (seconds or 0)
        while time.monotonic() < deadline:
            with self.pipe.lock:
                note = self.pipe.notes.pop(0) if self.pipe.notes else None
            if note is None:
                time.sleep(0.01)
            else:
                yield note

    def close(self) -> None:
        with self.pipe.lock:
            self.pipe.closed += 1


async def call(pipe: FakePipe, tool: str, args: dict | None = None, **kwargs):
    async with Client(build_server(pipe.connect)) as client:
        return await client.call_tool(tool, args or {}, **kwargs)


ALL_TOOLS = {
    "tongs_state", "tongs_window", "tongs_threads", "tongs_frames", "tongs_screenshot",
    "tongs_wait_until", "tongs_run_until", "tongs_cancel",
    "tongs_pause", "tongs_resume", "tongs_speed", "tongs_key", "tongs_text",
    "tongs_move", "tongs_click_point", "tongs_scroll", "tongs_release",
    "tongs_ui_tree", "tongs_ui_find", "tongs_ui_click",
    "tongs_event_list", "tongs_event_choose", "tongs_console", "tongs_raw",
}  # fmt: skip

INPUT_TOOLS = {
    "tongs_run_until", "tongs_pause", "tongs_resume", "tongs_speed", "tongs_key", "tongs_text",
    "tongs_move", "tongs_click_point", "tongs_scroll", "tongs_release", "tongs_ui_click", "tongs_event_choose",
}  # fmt: skip

LINES = [
    ("tongs_state", {}, "state"),
    ("tongs_window", {}, "window"),
    ("tongs_threads", {}, "threads"),
    ("tongs_frames", {}, "frames"),
    ("tongs_frames", {"timeout": 30}, "frames timeout=30"),
    ("tongs_screenshot", {}, "screenshot"),
    ("tongs_screenshot", {"name": "menu-1"}, "screenshot menu-1"),
    ("tongs_wait_until", {"date": "1720.1.1"}, "wait_until 1720.1.1"),
    ("tongs_wait_until", {"date": "1720.1.1.12", "timeout": 60, "max_pause": 5}, "wait_until 1720.1.1.12 timeout=60 max_pause=5"),
    ("tongs_run_until", {"date": "1719.6.1"}, "run_until 1719.6.1"),
    (
        "tongs_run_until",
        {"date": "1719.6.1", "speed": 5, "timeout": 600, "max_pause": 10, "events": "first"},
        "run_until 1719.6.1 speed=5 timeout=600 max_pause=10 events=first",
    ),
    ("tongs_cancel", {}, "cancel"),
    ("tongs_pause", {}, "pause"),
    ("tongs_resume", {}, "resume"),
    ("tongs_speed", {"speed": 3}, "speed 3"),
    ("tongs_key", {"name": "f1"}, "key f1"),
    ("tongs_text", {"text": "Hello world"}, "text Hello world"),
    ("tongs_move", {"x": 10, "y": 20}, "move 10 20"),
    ("tongs_click_point", {"x": 3350, "y": 32}, "click 3350 32 left"),
    ("tongs_click_point", {"x": 1, "y": 2, "button": "right", "double": True}, "click 1 2 right double"),
    ("tongs_scroll", {"steps": -3}, "scroll -3"),
    ("tongs_scroll", {"steps": 2, "x": 100, "y": 200}, "scroll 2 100 200"),
    ("tongs_release", {}, "release"),
    ("tongs_ui_find", {"selector": "speed_controls/button_speedup"}, "ui find speed_controls/button_speedup"),
    ("tongs_ui_find", {"selector": "//[text~=a b]", "all": True, "limit": 5}, "ui find //[text~=a b] all limit=5"),
    ("tongs_ui_find", {"selector": "//text", "all": True, "stale": True}, "ui find //text all stale"),
    ("tongs_ui_tree", {}, "ui tree"),
    ("tongs_ui_tree", {"selector": "speed_controls", "depth": 1, "limit": 10, "all": True}, "ui tree speed_controls all depth=1 limit=10"),
    ("tongs_ui_tree", {"depth": 0}, "ui tree depth=0"),
    ("tongs_ui_click", {"selector": "speed_controls/button_speedup"}, "ui click speed_controls/button_speedup"),
    ("tongs_ui_click", {"selector": "x", "timeout": 20}, "ui click x timeout=20"),
    ("tongs_event_list", {}, "event list"),
    ("tongs_event_list", {"timeout": 5}, "event list timeout=5"),
    ("tongs_event_choose", {"option": 2}, "event choose 2"),
    ("tongs_event_choose", {"option_text": "Accept"}, 'event choose text^="Accept"'),
    (
        "tongs_event_choose",
        {"option_text": "the tsar", "option_match": "contains", "event_title": "A New", "event_match": "equals"},
        'event choose text~="the tsar" event=title="A New"',
    ),
    ("tongs_event_choose", {"option": 1, "event": 2, "timeout": 9}, "event choose 1 event=2 timeout=9"),
    ("tongs_console", {"line": "echo x"}, "console echo x"),
    ("tongs_raw", {"line": "help"}, "help"),
    ("tongs_raw", {"line": "  ping "}, "ping"),
]  # fmt: skip


async def test_lists_every_tool_with_safety_notes():
    async with Client(build_server(FakePipe().connect)) as client:
        tools = {t.name: t for t in (await client.list_tools()).tools}
    assert set(tools) == ALL_TOOLS
    for name in INPUT_TOOLS:
        assert "input = true" in tools[name].description, name
        assert '"inputs"' in tools[name].description, name
        assert tools[name].annotations.read_only_hint is False
    console = tools["tongs_console"]
    assert "CHANGES GAME STATE" in console.description
    assert "game_calls = true" in console.description
    assert "marked run" in console.description
    assert console.annotations.destructive_hint is True
    assert tools["tongs_state"].annotations.read_only_hint is True


# Tools that act as the player (a key or click can confirm, choose or quit) or remove something.
DESTRUCTIVE_TOOLS = {
    "tongs_run_until", "tongs_key", "tongs_click_point", "tongs_ui_click", "tongs_event_choose",
    "tongs_cancel", "tongs_screenshot", "tongs_console", "tongs_raw",
}  # fmt: skip
READ_ONLY_TOOLS = {
    "tongs_state", "tongs_window", "tongs_threads", "tongs_frames",
    "tongs_ui_find", "tongs_ui_tree", "tongs_event_list",
}  # fmt: skip


async def test_annotations_declare_the_effects():
    async with Client(build_server(FakePipe().connect)) as client:
        tools = {t.name: t.annotations for t in (await client.list_tools()).tools}
    for name, annotations in tools.items():
        assert annotations.read_only_hint is (name in READ_ONLY_TOOLS), name
        assert annotations.destructive_hint is (name in DESTRUCTIVE_TOOLS), name
    # The screenshot writes a file and prunes old ones; a wait's cancel ends every client's waits.
    assert tools["tongs_screenshot"].idempotent_hint is False
    assert tools["tongs_wait_until"].read_only_hint is False


@pytest.mark.parametrize(("tool", "args", "line"), LINES)
async def test_request_line(tool, args, line):
    pipe = FakePipe()
    result = await call(pipe, tool, args)
    assert pipe.sent == [line]
    assert not result.is_error
    assert pipe.closed == 1


def test_every_tool_has_a_request_line_case():
    assert {tool for tool, _, _ in LINES} == ALL_TOOLS


async def test_answer_passes_through_unchanged():
    line = '{"ok":true,"in_game":true,"date":"1719.3.17","speed":2,"paused":true,"text":"zażółć"}'
    result = await call(FakePipe(lambda _: line), "tongs_state")
    assert not result.is_error
    assert result.content[0].text == line
    assert result.structured_content == json.loads(line)


async def test_failed_answer_is_an_error_with_the_plugin_json():
    line = '{"ok":false,"command":"ui click","error":"covered","covered_by":{"path":"x"}}'
    result = await call(FakePipe(lambda _: line), "tongs_ui_click", {"selector": "a/b"})
    assert result.is_error
    assert result.content[0].text == line
    assert result.structured_content["error"] == "covered"


async def test_answer_that_is_not_json_is_an_error():
    result = await call(FakePipe(lambda _: "garbage"), "tongs_state")
    assert result.is_error
    assert "not JSON" in result.content[0].text


@pytest.mark.parametrize(
    ("tool", "args"),
    [
        ("tongs_text", {"text": "a\nstate"}),
        ("tongs_console", {"line": "echo x\rpause"}),
        ("tongs_raw", {"line": "state\nresume"}),
        ("tongs_ui_find", {"selector": "a\nb"}),
        ("tongs_ui_tree", {"selector": "a\x00"}),
        ("tongs_event_choose", {"option_text": "a\nb"}),
        ("tongs_event_choose", {"option_text": 'say "hi"'}),
        ("tongs_event_choose", {}),
        ("tongs_event_choose", {"option": 1, "option_text": "a"}),
        ("tongs_event_choose", {"option": 1, "event": 1, "event_title": "t"}),
        ("tongs_scroll", {"steps": 1, "x": 5}),
        ("tongs_raw", {"line": "subscribe all"}),
        ("tongs_raw", {"line": "unsubscribe"}),
        ("tongs_speed", {"speed": 6}),
        ("tongs_key", {"name": "a b"}),
        ("tongs_wait_until", {"date": "next year"}),
        ("tongs_screenshot", {"name": "../x"}),
    ],
)
async def test_refused_without_sending(tool, args):
    pipe = FakePipe()
    result = await call(pipe, tool, args)
    assert result.is_error
    assert pipe.sent == []


async def test_missing_pipe_is_reported():
    def connect():
        raise FileNotFoundError(2, "not found")

    async with Client(build_server(connect)) as client:
        result = await client.call_tool("tongs_state", {})
    assert result.is_error
    assert "enabled = true" in result.content[0].text


async def test_pipe_closing_without_answer_is_reported():
    def handler(_):
        raise ConnectionError("the pipe closed without an answer")

    result = await call(FakePipe(handler), "tongs_state")
    assert result.is_error
    assert "did the game exit" in result.content[0].text
    assert "may or may not have been carried out" in result.content[0].text


def texts(result) -> list[str]:
    return [c.text for c in result.content if c.type == "text"]


async def test_a_cancelled_call_is_told_unknown_then_answered():
    """The plugin still has a cancelled click: later answers say so, then quote its answer, once each."""
    pipe = FakePipe()
    clicked = threading.Event()

    def handler(line):
        if line.startswith("ui click"):
            clicked.wait(5)
            return {"ok": True, "command": "ui click", "clicked": "speed_controls/button_speedup"}
        return {"ok": True, "in_game": False}

    pipe.handler = handler
    async with Client(build_server(pipe.connect)) as client:
        with anyio.move_on_after(0.3):
            await client.call_tool("tongs_ui_click", {"selector": "speed_controls/button_speedup"})
        pending = await client.call_tool("tongs_state", {})
        assert not pending.is_error
        assert len(texts(pending)) == 2
        assert "`ui click speed_controls/button_speedup` was cancelled before the plugin answered" in texts(pending)[1]
        assert "effect is unknown" in texts(pending)[1]
        assert len(texts(await client.call_tool("tongs_state", {}))) == 1  # told once while unanswered

        clicked.set()
        with anyio.fail_after(5):
            while True:
                answered = await client.call_tool("tongs_state", {})
                if len(texts(answered)) > 1:
                    break
                await anyio.sleep(0.02)
        assert "the plugin answered it afterwards" in texts(answered)[1]
        assert '"clicked": "speed_controls/button_speedup"' in texts(answered)[1]
        assert len(texts(await client.call_tool("tongs_state", {}))) == 1  # and once when settled
    assert pipe.closed == pipe.sent.count("state") + 1


async def test_a_cancelled_call_without_answer_is_told_on_the_next_error():
    pipe = FakePipe()
    release = threading.Event()

    def handler(line):
        if line == "key return":
            release.wait(5)
            raise ConnectionError("the pipe closed without an answer")
        if line == "frames":
            raise ConnectionError("the pipe closed without an answer")
        return {"ok": True, "in_game": False}

    pipe.handler = handler
    async with Client(build_server(pipe.connect)) as client:
        with anyio.move_on_after(0.3):
            await client.call_tool("tongs_key", {"name": "return"})
        release.set()
        with anyio.fail_after(5):
            while pipe.closed < 1:
                await anyio.sleep(0.02)
        failed = await client.call_tool("tongs_frames", {})
    assert failed.is_error
    assert "`key return` was cancelled and ended without an answer" in failed.content[0].text
    assert "its effect is unknown" in failed.content[0].text


async def test_screenshot_attaches_small_images(tmp_path: Path):
    png = tmp_path / "shot.png"
    png.write_bytes(b"\x89PNG\r\n\x1a\nfake")
    pipe = FakePipe(lambda _: {"ok": True, "path": str(png), "width": 1, "height": 1})
    result = await call(pipe, "tongs_screenshot", {"image": True})
    assert not result.is_error
    assert [c.type for c in result.content] == ["text", "image"]
    assert result.content[1].mime_type == "image/png"

    plain = await call(pipe, "tongs_screenshot", {})
    assert [c.type for c in plain.content] == ["text"]


async def test_screenshot_skips_large_images(tmp_path: Path, monkeypatch):
    import tongs_mcp.server as server

    monkeypatch.setattr(server, "MAX_IMAGE_BYTES", 3)
    png = tmp_path / "big.png"
    png.write_bytes(b"0123456789")
    pipe = FakePipe(lambda _: {"ok": True, "path": str(png)})
    result = await call(pipe, "tongs_screenshot", {"image": True})
    assert [c.type for c in result.content] == ["text", "text"]
    assert "not attached" in result.content[1].text


async def test_run_until_reports_game_dates_as_progress():
    pipe = FakePipe()
    released = threading.Event()

    def handler(line):
        if line == "subscribe date date=day":
            with pipe.lock:
                pipe.notes += [
                    {"notification": "date", "seq": 1, "date": "1719.3.18", "unit": "day"},
                    {"notification": "date", "seq": 2, "date": "1719.4.1", "unit": "day"},
                ]
            return {"ok": True, "command": "subscribe", "kinds": ["date"], "date": "1719.3.17"}
        released.wait(5)
        return {"ok": True, "command": "run_until", "reached": True, "reached_date": "1719.6.1"}

    pipe.handler = handler
    seen = []

    async def on_progress(progress, total, message):
        seen.append((progress, total, message))
        if message == "game date 1719.4.1":
            released.set()

    # In-process, the client hands progress to the callback without putting a token in the
    # request meta, which the server looks for (stdio requests carry one); add it.
    result = await call(
        pipe,
        "tongs_run_until",
        {"date": "1719.6.1", "speed": 5},
        progress_callback=on_progress,
        meta={"progress_token": "t"},
    )
    assert not result.is_error
    assert seen[-1] == (game_days("1719.4.1") - game_days("1719.3.17"), 76, "game date 1719.4.1")
    assert sorted(pipe.sent) == ["run_until 1719.6.1 speed=5", "subscribe date date=day"]
    assert pipe.closed == 2


def paused_game(pipe: FakePipe, release, ping=lambda: {"ok": True, "command": "ping"}):
    """A subscription answered at 1719.3.17 and a wait that blocks until `release` is set."""

    def handler(line):
        if line == "subscribe date date=day":
            return {"ok": True, "command": "subscribe", "kinds": ["date"], "date": "1719.3.17"}
        if line == "ping":
            return ping()
        release.wait(5)
        return {"ok": True, "command": "wait_until", "reached": True}

    pipe.handler = handler


async def sent(pipe: FakePipe, line: str) -> None:
    """Waits until `line` has been sent."""
    with anyio.fail_after(5):
        while line not in pipe.sent:
            await anyio.sleep(0.01)


async def wait_with_progress(pipe: FakePipe, on_progress):
    return await call(
        pipe, "tongs_wait_until", {"date": "1719.4.16"}, progress_callback=on_progress, meta={"progress_token": "t"}
    )


async def test_paused_wait_sends_heartbeats_with_increasing_progress(monkeypatch):
    monkeypatch.setattr(server, "HEARTBEAT_S", 0.2)
    pipe = FakePipe()
    released = threading.Event()
    paused_game(pipe, released)
    seen = []

    async def on_progress(progress, total, message):
        seen.append((progress, total, message))
        if len(seen) == 3:
            released.set()

    result = await wait_with_progress(pipe, on_progress)
    assert not result.is_error
    values = [s[0] for s in seen]
    assert values[0] == 0.0
    assert all(b > a for a, b in itertools.pairwise(values))
    assert all(s[1] == 30 and s[2].startswith("game date 1719.3.17, no new day for ") for s in seen)
    assert pipe.sent.count("ping") >= 3


async def test_a_new_day_after_heartbeats_reports_the_game_days(monkeypatch):
    monkeypatch.setattr(server, "HEARTBEAT_S", 0.2)
    pipe = FakePipe()
    released = threading.Event()
    paused_game(pipe, released)
    seen = []

    async def on_progress(progress, total, message):
        seen.append((progress, message))
        if len(seen) == 2:  # the game resumes after two heartbeats
            with pipe.lock:
                pipe.notes.append({"notification": "date", "seq": 1, "date": "1719.3.18", "unit": "day"})
        if message == "game date 1719.3.18":
            released.set()

    await wait_with_progress(pipe, on_progress)
    values = [s[0] for s in seen]
    assert all(b > a for a, b in itertools.pairwise(values))
    assert seen[2] == (1.0, "game date 1719.3.18")


async def test_heartbeat_stops_when_the_plugin_does_not_answer_ping(monkeypatch):
    monkeypatch.setattr(server, "HEARTBEAT_S", 0.1)
    monkeypatch.setattr(server, "PROGRESS_POLL_S", 0.05)
    pipe = FakePipe()
    released = threading.Event()
    paused_game(pipe, released, ping=lambda: {"ok": False, "command": "ping", "error": "subscribed"})
    seen = []

    async def on_progress(*args):
        seen.append(args)

    async with anyio.create_task_group() as tg:
        tg.start_soon(wait_with_progress, pipe, on_progress)
        await sent(pipe, "ping")
        await anyio.sleep(0.5)  # a few more heartbeat periods
        released.set()
    assert seen == []
    assert pipe.sent.count("ping") == 1


async def test_no_heartbeat_when_turned_off(monkeypatch):
    monkeypatch.setattr(server, "HEARTBEAT_S", 0)
    monkeypatch.setattr(server, "PROGRESS_POLL_S", 0.05)
    pipe = FakePipe()
    released = threading.Event()
    paused_game(pipe, released)
    seen = []

    async def on_progress(*args):
        seen.append(args)

    async with anyio.create_task_group() as tg:
        tg.start_soon(wait_with_progress, pipe, on_progress)
        await sent(pipe, "subscribe date date=day")
        await anyio.sleep(0.5)  # ten polls of the progress stream
        released.set()
    assert seen == []
    assert "ping" not in pipe.sent


async def test_no_progress_stream_without_a_progress_request():
    pipe = FakePipe()
    await call(pipe, "tongs_wait_until", {"date": "1720.1.1"})
    assert pipe.sent == ["wait_until 1720.1.1"]


async def test_no_progress_stream_when_turned_off():
    pipe = FakePipe()

    async def on_progress(*_):
        pass

    args = {"date": "1720.1.1", "progress": False}
    await call(pipe, "tongs_wait_until", args, progress_callback=on_progress, meta={"progress_token": "t"})
    assert pipe.sent == ["wait_until 1720.1.1"]


async def test_cancelling_a_wait_sends_cancel():
    pipe = FakePipe()
    cancelled = threading.Event()

    def handler(line):
        if line == "cancel":
            cancelled.set()
            return {"ok": True, "command": "cancel"}
        cancelled.wait(5)
        return {"ok": False, "command": "run_until", "error": "cancelled"}

    pipe.handler = handler
    async with Client(build_server(pipe.connect)) as client:
        with anyio.move_on_after(0.5):
            await client.call_tool("tongs_run_until", {"date": "1720.1.1", "progress": False})
        with anyio.fail_after(5):
            while not cancelled.is_set():
                await anyio.sleep(0.02)
    assert pipe.sent[0] == "run_until 1720.1.1"
    assert "cancel" in pipe.sent


async def test_pipe_error_during_a_wait_with_progress_keeps_its_message():
    def connect():
        raise FileNotFoundError(2, "not found")

    async def on_progress(*_):
        pass

    async with Client(build_server(connect)) as client:
        result = await client.call_tool(
            "tongs_run_until", {"date": "1720.1.1"}, progress_callback=on_progress, meta={"progress_token": "t"}
        )
    assert result.is_error
    assert "enabled = true" in result.content[0].text


async def test_cancelling_a_wait_with_progress_sends_cancel():
    pipe = FakePipe()
    cancelled = threading.Event()

    def handler(line):
        if line == "subscribe date date=day":
            return {"ok": True, "command": "subscribe", "date": "1719.3.17"}
        if line == "cancel":
            cancelled.set()
            return {"ok": True, "command": "cancel"}
        cancelled.wait(5)
        return {"ok": False, "command": "wait_until", "error": "cancelled"}

    pipe.handler = handler

    async def on_progress(*_):
        pass

    async with Client(build_server(pipe.connect)) as client:
        with anyio.move_on_after(0.5):
            await client.call_tool(
                "tongs_wait_until", {"date": "1720.1.1"}, progress_callback=on_progress, meta={"progress_token": "t"}
            )
        with anyio.fail_after(5):
            while not cancelled.is_set():
                await anyio.sleep(0.02)
    assert "cancel" in pipe.sent


def test_game_days():
    assert game_days("1719.3.17") == 1719 * 365 + 59 + 16
    assert game_days("1720.1.1.12") == 1720 * 365 + 0.5
    assert game_days("1719.6.1") - game_days("1719.3.17") == 76
    assert game_days(None) is None
    assert game_days("1719.13.1") is None


async def test_runs_over_stdio():
    """The installed entry point speaks MCP on stdin/stdout; without a game every call is an error."""
    import sys

    from mcp import StdioServerParameters

    params = StdioServerParameters(command=sys.executable, args=["-m", "tongs_mcp"])
    async with Client(params) as client:
        tools = {t.name for t in (await client.list_tools()).tools}
        assert tools == ALL_TOOLS
        result = await client.call_tool("tongs_raw", {"line": "subscribe all"})
        assert result.is_error
