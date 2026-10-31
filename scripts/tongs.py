"""Client for the Tongs pipe (\\\\.\\pipe\\tongs). Standard library only; the pipe needs Windows.

Command line: one answer line per command, in order, over one connection.
A `subscribe` command, which must come last, then prints notification lines
until --seconds or --count is reached (or Ctrl+C), and unsubscribes.

    python tongs.py state
    python tongs.py pause "speed 5" "run_until 1720.1.1"
    python tongs.py "subscribe state,date,event date=day" --seconds 60
    python tongs.py --ready --session 1 --speed 5

As a module:

    import tongs
    tongs.ready(session=1, expect_pid=pid, expect_start_ms=start_ms)  # waits for the pipe too
    with tongs.Tongs(expect_pid=pid, expect_start_ms=start_ms) as game:
        print(game.call("state")["date"], game.province(295)["owner"])
        result = game.run_until("1720.1.1", speed=5, on_progress=lambda p: print(p.date))

    with tongs.Tongs() as watch:  # a connection of its own
        watch.subscribe("state,event,activity")
        for note in watch.notifications(seconds=30):
            print(note["notification"], note.get("change"))
        watch.unsubscribe()

Every call has a read timeout (READ_TIMEOUT_S, or a command's own `timeout=` and
a margin). A wait that times out or is interrupted (Ctrl+C) is ended with
`cancel` on a second connection; the client then waits LATE_ANSWER_S for the
answer and raises CallTimeout / CallInterrupted, which say whether it came
("answered_late") or not ("unknown": the plugin may still carry the call out).
With expect_pid / expect_start_ms, every answer that names the game process is
checked against them, and another process raises ProcessMismatch.
"""
import argparse
import collections
import json
import math
import re
import sys
import threading
import time
from dataclasses import dataclass

PIPE = r"\\.\pipe\tongs"
READ_TIMEOUT_S = 30.0  # a call without a timeout of its own
TIMEOUT_MARGIN_S = 30.0  # added to a command's own timeout=
WAIT_TIMEOUT_S = 3600  # the plugin's default timeout of wait_until / run_until
LATE_ANSWER_S = 5.0  # how long a call that timed out or was interrupted waits for its answer
WAITS = ("wait_until", "run_until")

if sys.platform == "win32":
    import ctypes
    import msvcrt
    from ctypes import wintypes

    _kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
    _kernel32.PeekNamedPipe.argtypes = [
        wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
        ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
    ]
    _kernel32.PeekNamedPipe.restype = wintypes.BOOL


class TongsError(RuntimeError):
    """An answer with "ok": false, raised by call(..., check=True) and the typed steps.
    `answer` is the plugin's answer, or one the client made with the same shape."""

    def __init__(self, command, answer):
        super().__init__(f"{command}: {answer.get('error', 'failed')}" + (f" ({answer['detail']})" if answer.get("detail") else ""))
        self.command = command
        self.answer = answer

    @property
    def error(self):
        return self.answer.get("error")


class ProcessMismatch(TongsError):
    """An answer came from another game process than the one pinned; the connection is closed."""


class CallTimeout(TongsError):
    """No answer within the read timeout. `late` is the answer if it came within LATE_ANSWER_S
    afterwards (`effect` "answered_late"), else None (`effect` "unknown")."""

    def __init__(self, command, late):
        self.late = late
        self.effect = "unknown" if late is None else "answered_late"
        super().__init__(command, {"ok": False, "error": "timeout", "effect": self.effect, "late": late})


class CallInterrupted(KeyboardInterrupt):
    """Ctrl+C while waiting for an answer; `late` and `effect` as in CallTimeout."""

    def __init__(self, command, late):
        super().__init__(command)
        self.command = command
        self.late = late
        self.effect = "unknown" if late is None else "answered_late"


class ReadyError(TongsError):
    """ready() failed: `error` is "timeout" (with `unmet`, the first condition not met),
    "unexpected_session", "left_game", "cancelled" or the speed command's error."""


# --- game dates (EU4: 365 days, no leap years) -------------------------------

_MONTH_START = (0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334)


def game_days(date):
    """`1720.1.1` or `1720.1.1.12` as days (and hours) since year 0; None if not a date."""
    try:
        parts = [int(p) for p in str(date).split(".")]
        if len(parts) not in (3, 4) or not 1 <= parts[1] <= 12:
            return None
        hours = parts[3] if len(parts) == 4 else 0
        return parts[0] * 365 + _MONTH_START[parts[1] - 1] + parts[2] - 1 + hours / 24
    except (TypeError, ValueError):
        return None


# --- request lines -----------------------------------------------------------

_TIMEOUT_OPTION = re.compile(r"(?:^|\s)timeout=(\d+)(?:\s|$)")


def read_timeout(command):
    """How long the client waits for the answer to `command`: its own timeout= and a margin,
    the plugin's default wait for wait_until / run_until, else READ_TIMEOUT_S."""
    own = _TIMEOUT_OPTION.search(command)
    if own:
        return int(own.group(1)) + TIMEOUT_MARGIN_S
    if command.split()[:1] and command.split()[0] in WAITS:
        return WAIT_TIMEOUT_S + TIMEOUT_MARGIN_S
    return READ_TIMEOUT_S


def line(*words, **options):
    """Words in order, then `name=value` for the options that are set."""
    parts = [str(w) for w in words if w is not None and w != ""]
    parts += [f"{k}={v}" for k, v in options.items() if v is not None]
    return " ".join(parts)


def check_process(command, answer, expect_pid=None, expect_start_ms=None):
    """Raises ProcessMismatch when `answer` names another process than the one expected."""
    if not isinstance(answer, dict):
        return
    wrong = (expect_pid is not None and "pid" in answer and answer["pid"] != expect_pid) or (
        expect_start_ms is not None and "process_start_ms" in answer and answer["process_start_ms"] != expect_start_ms
    )
    if wrong:
        raise ProcessMismatch(command, {
            "ok": False, "error": "process_mismatch",
            "expected": {"pid": expect_pid, "process_start_ms": expect_start_ms},
            "seen": {"pid": answer.get("pid"), "process_start_ms": answer.get("process_start_ms")},
        })


def send_cancel(timeout_s=LATE_ANSWER_S):
    """Sends `cancel` on a connection of its own (it ends the waits of every client); returns
    its answer, or None when the pipe did not take it."""
    try:
        with Tongs(timeout_s=2.0, read_timeout_s=timeout_s) as other:
            return other.call("cancel")
    except (OSError, TongsError):
        return None


# --- the connection ----------------------------------------------------------


class Tongs:
    def __init__(self, timeout_s=2.0, expect_pid=None, expect_start_ms=None, read_timeout_s=None):
        """Opens the pipe, retrying for `timeout_s` (the pipe has 8 instances, and it is not there
        until the plugin starts). With expect_pid / expect_start_ms it checks `ping` first."""
        if sys.platform != "win32":
            raise OSError("the Tongs pipe is Windows only")
        deadline = time.monotonic() + timeout_s
        while True:
            try:
                self._pipe = open(PIPE, "r+b", buffering=0)
                break
            except OSError:
                if time.monotonic() >= deadline:
                    raise
                time.sleep(0.05)
        self._buffer = b""
        self._notes = collections.deque()  # notifications read while waiting for an answer
        self.expect_pid = expect_pid
        self.expect_start_ms = expect_start_ms
        self.read_timeout_s = read_timeout_s  # None: read_timeout(command) for each call
        self.closed = False
        if expect_pid is not None or expect_start_ms is not None:
            self.call("ping", check=True)

    def close(self):
        if not self.closed:
            self.closed = True
            self._pipe.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def _available(self):
        """Bytes waiting in the pipe, without blocking."""
        handle = msvcrt.get_osfhandle(self._pipe.fileno())
        available = wintypes.DWORD(0)
        if not _kernel32.PeekNamedPipe(handle, None, 0, None, ctypes.byref(available), None):
            raise ConnectionError(f"the pipe closed (error {ctypes.get_last_error()})")
        return available.value

    def _line(self, timeout_s=None):
        """The next line as text; None when timeout_s passes first (None waits for ever). Polls,
        so Ctrl+C is seen while waiting."""
        deadline = None if timeout_s is None else time.monotonic() + timeout_s
        idle = 0.002
        while b"\n" not in self._buffer:
            waiting = self._available()
            if waiting == 0:
                if deadline is not None and time.monotonic() >= deadline:
                    return None
                time.sleep(idle)
                idle = min(idle * 2, 0.05)
                continue
            idle = 0.002
            chunk = self._pipe.read(min(waiting, 65536))
            if not chunk:
                raise ConnectionError("the pipe closed without an answer")
            self._buffer += chunk
        line_, _, self._buffer = self._buffer.partition(b"\n")
        return line_.decode("utf-8")

    def _answer_line(self, timeout_s):
        """The next answer line; notifications that come first are kept for notifications()."""
        deadline = None if timeout_s is None else time.monotonic() + timeout_s
        while True:
            left = None if deadline is None else max(0.0, deadline - time.monotonic())
            text = self._line(left)
            if text is None:
                return None
            if text.startswith('{"notification":'):
                self._notes.append(json.loads(text))
                continue
            return text

    def _late(self, command):
        """After a timeout or an interrupt: ends a wait with `cancel` on a second connection and
        waits LATE_ANSWER_S for the answer. Without one, the connection is closed, since the
        answer may still come."""
        if command.split()[:1] and command.split()[0] in WAITS:
            send_cancel()
        try:
            text = self._answer_line(LATE_ANSWER_S)
        except (OSError, KeyboardInterrupt):
            text = None
        if text is None:
            self.close()
            return None
        return json.loads(text)

    def raw(self, command, timeout_s=None):
        """Sends one command and returns its answer line as text. `timeout_s` overrides the read
        timeout. Raises CallTimeout, CallInterrupted and ProcessMismatch (see the module)."""
        if self.closed:
            raise ConnectionError("the connection is closed")
        if timeout_s is None:
            timeout_s = self.read_timeout_s if self.read_timeout_s is not None else read_timeout(command)
        self._pipe.write(command.encode("utf-8") + b"\n")
        try:
            text = self._answer_line(timeout_s)
        except KeyboardInterrupt:
            raise CallInterrupted(command, self._late(command)) from None
        if text is None:
            raise CallTimeout(command, self._late(command))
        if self.expect_pid is not None or self.expect_start_ms is not None:
            try:
                check_process(command, json.loads(text), self.expect_pid, self.expect_start_ms)
            except ProcessMismatch:
                self.close()
                raise
        return text

    def call(self, command, check=False, timeout_s=None):
        """Sends one command and returns the parsed answer; with `check`, raises TongsError on
        "ok": false."""
        answer = json.loads(self.raw(command, timeout_s))
        if check and not answer.get("ok"):
            raise TongsError(command, answer)
        return answer

    def subscribe(self, kinds, date=None, check=True):
        """Turns this connection into a notification stream: `kinds` such as
        "state,date,event" or "all", `date` "day", "month" or "year"."""
        command = f"subscribe {kinds}" + (f" date={date}" if date else "")
        return self.call(command, check=check)

    def unsubscribe(self):
        """Ends the stream; the connection takes ordinary requests again."""
        return self.call("unsubscribe", check=True)

    def notifications(self, seconds=None, count=None):
        """Yields parsed notifications until `seconds` pass or `count` arrived
        (both None: for ever)."""
        deadline = None if seconds is None else time.monotonic() + seconds
        got = 0
        while count is None or got < count:
            if self._notes:
                note = self._notes.popleft()
            else:
                left = None if deadline is None else deadline - time.monotonic()
                if left is not None and left <= 0:
                    return
                text = self._line(left)
                if text is None:
                    return
                note = json.loads(text)
            got += 1
            yield note

    # --- typed steps: the plugin's answer unchanged; TongsError on "ok": false ---

    def state(self):
        return self.call("state", check=True)

    def wait_until(self, date, timeout=None, max_pause=None, on_progress=None):
        """Waits until the game date reaches `date`. `on_progress(DateProgress)` is called from a
        second connection (see follow_dates)."""
        return self._wait(line("wait_until", date, timeout=timeout, max_pause=max_pause), on_progress)

    def run_until(self, date, speed=None, timeout=None, max_pause=None, events=None, on_progress=None):
        """Runs the game until `date` and pauses (input)."""
        command = line("run_until", date, speed=speed, timeout=timeout, max_pause=max_pause, events=events)
        return self._wait(command, on_progress)

    def _wait(self, command, on_progress):
        if on_progress is None:
            return self.call(command, check=True)
        stop = threading.Event()
        feed = threading.Thread(target=_report_progress, args=(on_progress, stop), daemon=True)
        feed.start()
        try:
            return self.call(command, check=True)
        finally:
            stop.set()
            feed.join(2.0)

    # World reads (docs/commands.md, "World reads"): on the main thread, not counted.

    def province(self, id, timeout=None):
        """One province by ID: name, owner, controller, cores, development, land, wasteland."""
        return self.call(line("province", int(id), timeout=timeout), check=True)

    def country(self, tag, timeout=None):
        """One country by tag: exists, province_count, provinces, capital, observer."""
        return self.call(line("country", tag, timeout=timeout), check=True)

    def player(self, timeout=None):
        """The player's country: tag (observe mode included), player_tag, observed_tag, observer."""
        return self.call(line("player", timeout=timeout), check=True)

    def selection(self, timeout=None):
        """The last selected province and the province view: only view.visible says it is open."""
        return self.call(line("selection", timeout=timeout), check=True)

    # Navigation (docs/commands.md, "Navigation"): client calls, behind input.

    def open_province(self, id, timeout=None):
        """Opens the province's view (and moves the camera to it), as the game's links do."""
        return self.call(line("province open", int(id), timeout=timeout), check=True)

    def goto_province(self, id, timeout=None):
        """Moves the camera to the province, nothing else."""
        return self.call(line("province goto", int(id), timeout=timeout), check=True)

    def ready(self, session=None, in_game=True, speed=None, timeout=None):
        """ready() on this connection; see the module function."""
        return wait_ready(
            self.call, session=session, in_game=in_game, speed=speed, timeout=timeout,
            expect_pid=self.expect_pid, expect_start_ms=self.expect_start_ms,
        )


# --- progress ----------------------------------------------------------------


@dataclass
class DateProgress:
    """One report of follow_dates: `kind` "start" (the date when it subscribed), "day" (a new
    game day) or "heartbeat" (no new day for heartbeat_s; the plugin answered `ping`)."""

    kind: str
    date: str
    days: float
    idle_s: float  # seconds since the last new day (or the start)


def follow_dates(conn, heartbeat_s=60.0, poll_s=0.5):
    """On a connection of its own: subscribes to the date by day and yields a DateProgress for
    the start, each new day and each heartbeat, and None after every poll_s without one (so the
    caller can stop). Returns when the subscription or a heartbeat `ping` is refused."""
    first = json.loads(conn.raw("subscribe date date=day"))
    last = first.get("date")
    if not first.get("ok") or game_days(last) is None:
        return
    new_day_at = reported_at = time.monotonic()
    yield DateProgress("start", last, game_days(last), 0.0)
    while True:
        notes = list(conn.notifications(seconds=poll_s))
        dates = [n["date"] for n in notes if n.get("notification") == "date" and n.get("date")]
        now = time.monotonic()
        if dates and dates[-1] != last and game_days(dates[-1]) is not None:
            last, new_day_at, reported_at = dates[-1], now, now
            yield DateProgress("day", last, game_days(last), 0.0)
        elif heartbeat_s > 0 and now - reported_at >= heartbeat_s:
            # `ping` is one of the two requests a subscribed connection answers.
            if not json.loads(conn.raw("ping")).get("ok"):
                return
            reported_at = now
            yield DateProgress("heartbeat", last, game_days(last), now - new_day_at)
        else:
            yield None


def _report_progress(on_progress, stop):
    try:
        with Tongs() as conn:
            for p in follow_dates(conn):
                if stop.is_set():
                    return
                if p is not None:
                    on_progress(p)
    except (OSError, ValueError, TongsError):
        return  # progress is optional; the wait itself reports pipe trouble


# --- ready -------------------------------------------------------------------

READY_TIMEOUT_START_S = 180  # a start with continue= (session 1)
READY_TIMEOUT_LOAD_S = 30  # after save load (a later session)
READY_TIMEOUT_MENU_S = 60  # a start to the main menu
READY_POLL_S = 0.1
READY_FRAMES_S = 10  # timeout= of each `frames` (at most)


def ready_timeout(session=None, in_game=True):
    """The default timeout of ready()."""
    if not in_game:
        return READY_TIMEOUT_MENU_S
    return READY_TIMEOUT_START_S if session in (None, 1) else READY_TIMEOUT_LOAD_S


def wait_ready(call, session=None, in_game=True, speed=None, timeout=None, expect_pid=None, expect_start_ms=None,
               cancelled=None, clock=time.monotonic, sleep=time.sleep):
    """Waits, with no fixed sleep, until at once: the pipe answers from the pinned process;
    `state` is in game with `session` (any when None); a `frames` answer showed an outer frame
    of that session; and the game is paused. With in_game=False: the main menu instead, with no
    session and no pause. `call(line)` returns a parsed answer (OSError and CallTimeout count as
    the pipe not answering). `speed=N` then sets the speed, which proves input is handled
    (`input_proven`) unless it already was N. Returns {"ok": true, "command": "ready", "session",
    "state", "frame", "waited_ms", "speed", "input_proven"}; raises ReadyError or ProcessMismatch."""
    if timeout is None:
        timeout = ready_timeout(session, in_game)
    start = clock()
    deadline = start + timeout
    unmet, state, seen = "pipe", None, False

    def fail(error, detail):
        return ReadyError("ready", {"ok": False, "command": "ready", "error": error, "detail": detail,
                                    "unmet": unmet, "state": state})

    def wrong(answer):
        """The first condition `answer` (with the state) does not meet, or None."""
        nonlocal seen
        if not in_game:
            return "in_game" if answer.get("in_game") else None
        if not answer.get("in_game"):
            if seen:
                raise fail("left_game", "the game left the session while waiting")
            return "in_game"
        if session is not None:
            current = answer.get("session")
            if current is None:
                raise fail("unexpected_session", "the answer has no session (Tongs before 0.19)")
            if current > session:
                raise fail("unexpected_session", f"session {current} is past the expected {session}")
            if current < session:
                return "session"
        seen = True
        return None

    def ask(command):
        try:
            answer = call(command)
        except (OSError, CallTimeout):
            return None
        check_process(command, answer, expect_pid, expect_start_ms)
        return answer

    while True:
        if cancelled is not None and cancelled():
            raise fail("cancelled", "ready was cancelled")
        left = deadline - clock()
        if left <= 0:
            raise fail("timeout", f"not ready within {timeout} s: {unmet}")
        answer = ask("state")
        if answer is None:
            unmet = "pipe"
        else:
            state = answer
            unmet = wrong(state)
            if unmet is None:
                frames = ask(f"frames timeout={max(1, min(READY_FRAMES_S, math.ceil(left)))}")
                if frames is None or not frames.get("ok") or "in_game" not in frames:
                    unmet = "frames"
                else:
                    state = frames  # the state on the frame itself
                    unmet = wrong(frames)
                    if unmet is None and in_game and not frames.get("paused"):
                        unmet = "paused"
                    if unmet is None:
                        break
        sleep(READY_POLL_S)

    result = {"ok": True, "command": "ready", "session": state.get("session"), "state": state,
              "frame": frames.get("frame"), "waited_ms": 0, "speed": None, "input_proven": False}
    if speed is not None and in_game:
        if state.get("speed") != speed:
            unmet = "speed"
            answer = call(f"speed {speed}")
            check_process("speed", answer, expect_pid, expect_start_ms)
            result["speed"] = answer
            if not answer.get("ok"):
                raise fail(answer.get("error", "speed"), "the speed command failed")
            result["input_proven"] = answer.get("presses", 0) > 0
            result["state"] = state = answer
    result["waited_ms"] = round((clock() - start) * 1000)
    return result


def ready(session=None, in_game=True, speed=None, timeout=None, expect_pid=None, expect_start_ms=None):
    """ready() from outside the game's start: waits for the pipe as well, one connection per
    request, and returns as wait_ready. Then open a Tongs with the same expect_pid /
    expect_start_ms for the next steps."""

    def call(command):
        with Tongs(timeout_s=0, read_timeout_s=read_timeout(command)) as conn:
            return conn.call(command)

    return wait_ready(call, session=session, in_game=in_game, speed=speed, timeout=timeout,
                      expect_pid=expect_pid, expect_start_ms=expect_start_ms)


# --- command line ------------------------------------------------------------


def main(argv):
    parser = argparse.ArgumentParser(description="Send commands to the Tongs pipe.")
    parser.add_argument("commands", nargs="*", default=["state"])
    parser.add_argument("--seconds", type=float, help="after subscribe: stop after this many seconds")
    parser.add_argument("--count", type=int, help="after subscribe: stop after this many notifications")
    parser.add_argument("--ready", action="store_true", help="wait until the game is ready first (see ready())")
    parser.add_argument("--session", type=int, help="with --ready: the session to wait for")
    parser.add_argument("--menu", action="store_true", help="with --ready: wait for the main menu instead")
    parser.add_argument("--speed", type=int, choices=range(1, 6), help="with --ready: then set this speed")
    parser.add_argument("--timeout", type=float, help="with --ready: seconds (default by case)")
    parser.add_argument("--pid", type=int, help="expect this game process id")
    parser.add_argument("--start-ms", type=int, help="expect this process creation time (Unix ms)")
    args = parser.parse_args(argv)
    # Answers are UTF-8 JSON (e.g. "£yes" in console texts); a pipe would otherwise get the
    # ANSI code page and fail on such characters.
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8")
    if args.ready:
        try:
            result = ready(args.session, not args.menu, args.speed, args.timeout, args.pid, args.start_ms)
        except TongsError as e:
            print(json.dumps(e.answer, ensure_ascii=False, separators=(",", ":")), flush=True)
            return 1
        print(json.dumps(result, ensure_ascii=False, separators=(",", ":")), flush=True)
        if argv and args.commands == ["state"] and "state" not in argv:
            return 0
    with Tongs(expect_pid=args.pid, expect_start_ms=args.start_ms) as game:
        ok = True
        for i, command in enumerate(args.commands):
            text = game.raw(command)
            print(text, flush=True)
            answer = json.loads(text)
            ok = ok and answer.get("ok", False)
            if command.split()[:1] == ["subscribe"] and answer.get("ok"):
                if i != len(args.commands) - 1:
                    print("subscribe must be the last command", file=sys.stderr)
                    return 2
                try:
                    for note in game.notifications(args.seconds, args.count):
                        print(json.dumps(note, ensure_ascii=False, separators=(",", ":")), flush=True)
                except KeyboardInterrupt:
                    pass
                print(game.raw("unsubscribe"), flush=True)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
