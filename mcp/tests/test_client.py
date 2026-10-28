"""The pipe client (scripts/tongs.py) against a named pipe of its own: read timeouts, late
answers, cancel on a second connection, process pinning, date progress and the ready wait."""

import itertools
import json
import os
import sys
import threading
import time
import uuid

import pytest

from tongs_mcp import tongs
from tongs_mcp.tongs import (
    CallTimeout,
    DateProgress,
    ProcessMismatch,
    ReadyError,
    Tongs,
    follow_dates,
    read_timeout,
    wait_ready,
)

windows = pytest.mark.skipif(sys.platform != "win32", reason="named pipes are Windows only")


class PipeServer:
    """Serves a named pipe of its own: `handler(line)` returns the answer (a dict, a str of one
    or more lines, or None for no answer); it runs on the connection's own thread."""

    def __init__(self, handler):
        import ctypes

        self.name = rf"\\.\pipe\tongs-test-{uuid.uuid4().hex}"
        self.handler = handler
        self.lines: list[str] = []
        self._k32 = ctypes.WinDLL("kernel32", use_last_error=True)
        self._k32.CreateNamedPipeW.restype = ctypes.c_void_p
        self._k32.CreateNamedPipeW.argtypes = [ctypes.c_wchar_p] + [ctypes.c_uint32] * 6 + [ctypes.c_void_p]
        self._k32.ConnectNamedPipe.argtypes = [ctypes.c_void_p, ctypes.c_void_p]
        self._ready = threading.Event()
        threading.Thread(target=self._accept, daemon=True).start()
        self._ready.wait(5)

    def _accept(self):
        import msvcrt

        while True:
            # PIPE_ACCESS_DUPLEX, PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT, unlimited instances
            handle = self._k32.CreateNamedPipeW(self.name, 3, 0, 255, 65536, 65536, 0, None)
            self._ready.set()
            self._k32.ConnectNamedPipe(handle, None)  # ERROR_PIPE_CONNECTED is fine too
            pipe = os.fdopen(msvcrt.open_osfhandle(handle, 0), "r+b", buffering=0)
            threading.Thread(target=self._serve, args=(pipe,), daemon=True).start()

    def _serve(self, pipe):
        buffer = b""
        while True:
            try:
                chunk = pipe.read(4096)
            except OSError:
                return
            if not chunk:
                return
            buffer += chunk
            while b"\n" in buffer:
                raw, _, buffer = buffer.partition(b"\n")
                line = raw.decode()
                self.lines.append(line)
                answer = self.handler(line)
                if answer is None:
                    continue
                text = answer if isinstance(answer, str) else json.dumps(answer)
                try:
                    pipe.write(text.encode() + b"\n")
                except OSError:
                    return


@pytest.fixture
def serve(monkeypatch):
    def start(handler):
        server = PipeServer(handler)
        monkeypatch.setattr(tongs, "PIPE", server.name)
        return server

    return start


PING = {"ok": True, "command": "ping", "pong": True, "version": "0.19.0", "pid": 42, "process_start_ms": 7}
STATE = {"ok": True, "in_game": True, "date": "1719.3.17", "speed": 2, "paused": True, "session": 1, "pid": 42,
         "process_start_ms": 7}  # fmt: skip


def game(line):
    return PING if line == "ping" else STATE if line == "state" else {"ok": False, "error": "unknown command"}


# --- pinning ---


@windows
def test_pinned_connection_checks_ping_and_answers(serve):
    server = serve(game)
    with Tongs(expect_pid=42, expect_start_ms=7) as conn:
        assert conn.call("state")["session"] == 1
    assert server.lines == ["ping", "state"]
    with pytest.raises(ProcessMismatch) as e:
        Tongs(expect_pid=41)
    assert e.value.error == "process_mismatch" and e.value.answer["seen"]["pid"] == 42


@windows
def test_an_answer_from_another_process_closes_the_connection(serve):
    serve(lambda line: PING if line == "ping" else {**STATE, "process_start_ms": 8})
    conn = Tongs(expect_start_ms=7)
    with pytest.raises(ProcessMismatch):
        conn.call("state")
    assert conn.closed


# --- read timeouts ---


@windows
def test_a_late_answer_is_returned_with_the_timeout(serve):
    def handler(line):
        if line == "frames":
            time.sleep(0.4)
            return {"ok": True, "command": "frames", "frame": 3}
        return game(line)

    serve(handler)
    with Tongs() as conn:
        with pytest.raises(CallTimeout) as e:
            conn.call("frames", timeout_s=0.1)
        assert e.value.effect == "answered_late" and e.value.late["frame"] == 3
        assert conn.call("state")["ok"]  # still in step: the late answer was read


@windows
def test_no_answer_closes_the_connection(serve, monkeypatch):
    monkeypatch.setattr(tongs, "LATE_ANSWER_S", 0.2)
    server = serve(lambda line: None if line == "key a" else game(line))
    conn = Tongs()
    with pytest.raises(CallTimeout) as e:
        conn.call("key a", timeout_s=0.1)
    assert e.value.effect == "unknown" and e.value.late is None and e.value.error == "timeout"
    assert conn.closed
    assert "cancel" not in server.lines  # only waits are cancelled


@windows
def test_a_wait_that_times_out_is_cancelled_on_a_second_connection(serve):
    cancelled = threading.Event()

    def handler(line):
        if line == "cancel":
            cancelled.set()
            return {"ok": True, "command": "cancel", "withdrawn": 0}
        if line.startswith("wait_until"):
            cancelled.wait(5)
            return {"ok": False, "command": "wait_until", "error": "cancelled"}
        return game(line)

    server = serve(handler)
    with Tongs() as conn:
        with pytest.raises(CallTimeout) as e:
            conn.call("wait_until 1720.1.1", timeout_s=0.2)
    assert e.value.effect == "answered_late" and e.value.late["error"] == "cancelled"
    assert server.lines == ["wait_until 1720.1.1", "cancel"]


def test_read_timeouts():
    assert read_timeout("state") == tongs.READ_TIMEOUT_S
    assert read_timeout("frames timeout=60") == 60 + tongs.TIMEOUT_MARGIN_S
    assert read_timeout("run_until 1720.1.1 speed=5") == tongs.WAIT_TIMEOUT_S + tongs.TIMEOUT_MARGIN_S
    assert read_timeout("wait_until 1720.1.1 timeout=7200 max_pause=5") == 7200 + tongs.TIMEOUT_MARGIN_S
    assert read_timeout("ui find //[text~=timeout=3]") == tongs.READ_TIMEOUT_S


# --- progress ---


@windows
def test_a_wait_reports_game_dates_from_a_second_connection(serve):
    seen: list[DateProgress] = []
    got = threading.Event()

    def handler(line):
        if line == "subscribe date date=day":
            notes = [{"notification": "date", "seq": i, "date": d, "unit": "day"}
                     for i, d in enumerate(["1719.3.18", "1719.3.19"], 1)]  # fmt: skip
            answer = {"ok": True, "command": "subscribe", "kinds": ["date"], "date": "1719.3.17"}
            return "\n".join(json.dumps(x) for x in [answer, *notes])
        if line.startswith("run_until"):
            got.wait(5)
            return {"ok": True, "command": "run_until", "reached": True}
        return game(line)

    def on_progress(p):
        seen.append(p)
        if p.date == "1719.3.19":
            got.set()

    server = serve(handler)
    with Tongs() as conn:
        assert conn.run_until("1719.3.19", speed=5, on_progress=on_progress)["reached"]
    # Days that come within one poll are reported as the last of them.
    assert [(p.kind, p.date) for p in seen] == [("start", "1719.3.17"), ("day", "1719.3.19")]
    assert "run_until 1719.3.19 speed=5" in server.lines


class FakeConnection:
    def __init__(self, notes, ping_ok=True):
        self.notes = list(notes)
        self.sent: list[str] = []
        self.ping_ok = ping_ok

    def raw(self, command):
        self.sent.append(command)
        if command == "ping":
            return json.dumps({"ok": self.ping_ok})
        return json.dumps({"ok": True, "date": "1719.3.17"})

    def notifications(self, seconds=None, count=None):
        time.sleep(seconds or 0)
        while self.notes:
            yield self.notes.pop(0)


def test_follow_dates_heartbeats_while_no_day_comes():
    feed = follow_dates(FakeConnection([]), heartbeat_s=0.05, poll_s=0.03)
    kinds = [p.kind if p else None for p in itertools.islice(feed, 6)]
    assert kinds[0] == "start" and "heartbeat" in kinds and None in kinds


def test_follow_dates_ends_when_ping_is_refused():
    conn = FakeConnection([], ping_ok=False)
    assert [p for p in follow_dates(conn, heartbeat_s=0.01, poll_s=0.02) if p][0].kind == "start"
    assert conn.sent.count("ping") == 1


# --- the ready wait ---


class Clock:
    def __init__(self):
        self.now = 0.0

    def __call__(self):
        return self.now

    def sleep(self, seconds):
        self.now += seconds


def scripted(*states, frames=None):
    """call() answering `state` with each of `states` in turn (the last one repeats) and
    `frames` with the state of the moment and frame 5, or with `frames`."""
    queue = list(states)
    current = {}

    def call(line):
        if line == "state":
            current.clear()
            current.update(queue.pop(0) if len(queue) > 1 else queue[0])
            return dict(current)
        if line.startswith("frames"):
            return frames or {"ok": True, "command": "frames", "frame": 5, **current}
        raise AssertionError(line)

    return call


def test_ready_waits_through_menu_loading_and_the_old_session():
    clock = Clock()
    call = scripted({"ok": True, "in_game": False}, {**STATE, "session": 1}, {**STATE, "session": 2})
    result = wait_ready(call, session=2, clock=clock, sleep=clock.sleep)
    assert result["session"] == 2 and result["frame"] == 5
    assert result["waited_ms"] == 200  # two polls


def test_ready_fails_when_the_game_leaves_the_session():
    clock = Clock()
    call = scripted({**STATE, "paused": False}, {"ok": True, "in_game": False, "reason": "x"})
    with pytest.raises(ReadyError) as e:
        wait_ready(call, session=1, clock=clock, sleep=clock.sleep)
    assert e.value.error == "left_game"


def test_ready_refuses_a_tongs_without_sessions():
    old = {k: v for k, v in STATE.items() if k != "session"}
    with pytest.raises(ReadyError) as e:
        wait_ready(scripted(old), session=1)
    assert e.value.error == "unexpected_session"
    assert wait_ready(scripted(old))["session"] is None  # no session asked: none checked


def test_ready_timeout_names_the_pipe_and_frames():
    clock = Clock()

    def no_pipe(_):
        raise FileNotFoundError(2, "not found")

    with pytest.raises(ReadyError) as e:
        wait_ready(no_pipe, timeout=1, clock=clock, sleep=clock.sleep)
    assert e.value.answer["unmet"] == "pipe" and e.value.answer["state"] is None
    busy = {"ok": False, "command": "frames", "error": "main thread busy"}
    with pytest.raises(ReadyError) as e:
        wait_ready(scripted(STATE, frames=busy), timeout=1, clock=clock, sleep=clock.sleep)
    assert e.value.answer["unmet"] == "frames" and e.value.answer["state"]["session"] == 1


def test_ready_default_timeouts():
    assert tongs.ready_timeout(1) == 180 and tongs.ready_timeout(None) == 180
    assert tongs.ready_timeout(2) == 30 and tongs.ready_timeout(in_game=False) == 60
