"""Client for the Tongs pipe (\\\\.\\pipe\\tongs). Standard library only, Windows.

Command line: one answer line per command, in order, over one connection.
A `subscribe` command, which must come last, then prints notification lines
until --seconds or --count is reached (or Ctrl+C), and unsubscribes.

    python tongs.py state
    python tongs.py pause "speed 5" "run_until 1720.1.1"
    python tongs.py "subscribe state,date,event date=day" --seconds 60

As a module:

    from tongs import Tongs
    with Tongs() as game:
        print(game.call("state")["date"])
        result = game.call("run_until 1720.1.1 speed=5", check=True)

    with Tongs() as watch:  # a connection of its own
        watch.subscribe("state,event,activity")
        for note in watch.notifications(seconds=30):
            print(note["notification"], note.get("change"))
        watch.unsubscribe()
"""
import argparse
import collections
import ctypes
import json
import msvcrt
import sys
import time
from ctypes import wintypes

PIPE = r"\\.\pipe\tongs"

_kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
_kernel32.PeekNamedPipe.argtypes = [
    wintypes.HANDLE, wintypes.LPVOID, wintypes.DWORD,
    ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD), ctypes.POINTER(wintypes.DWORD),
]
_kernel32.PeekNamedPipe.restype = wintypes.BOOL


class TongsError(RuntimeError):
    """An answer with "ok": false, raised by call(..., check=True)."""

    def __init__(self, command, answer):
        super().__init__(f"{command}: {answer.get('error', 'failed')}")
        self.command = command
        self.answer = answer


class Tongs:
    def __init__(self, timeout_s=2.0):
        # The pipe has 8 instances; when all are busy, opening fails until one frees up.
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

    def close(self):
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
        """The next line as text; None when timeout_s passes first (None waits for ever)."""
        deadline = None if timeout_s is None else time.monotonic() + timeout_s
        while b"\n" not in self._buffer:
            if deadline is not None:
                waiting = self._available()
                if waiting == 0:
                    if time.monotonic() >= deadline:
                        return None
                    time.sleep(0.01)
                    continue
                chunk = self._pipe.read(min(waiting, 65536))
            else:
                chunk = self._pipe.read(4096)
            if not chunk:
                raise ConnectionError("the pipe closed without an answer")
            self._buffer += chunk
        line, _, self._buffer = self._buffer.partition(b"\n")
        return line.decode("utf-8")

    def raw(self, command):
        """Sends one command and returns its answer line as text. Notifications
        that arrive first are kept for notifications()."""
        self._pipe.write(command.encode("utf-8") + b"\n")
        while True:
            line = self._line()
            if line.startswith('{"notification":'):
                self._notes.append(json.loads(line))
                continue
            return line

    def call(self, command, check=False):
        """Sends one command and returns the parsed answer."""
        answer = json.loads(self.raw(command))
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
                line = self._line(left)
                if line is None:
                    return
                note = json.loads(line)
            got += 1
            yield note


def main(argv):
    parser = argparse.ArgumentParser(description="Send commands to the Tongs pipe.")
    parser.add_argument("commands", nargs="*", default=["state"])
    parser.add_argument("--seconds", type=float, help="after subscribe: stop after this many seconds")
    parser.add_argument("--count", type=int, help="after subscribe: stop after this many notifications")
    args = parser.parse_args(argv)
    with Tongs() as game:
        ok = True
        for i, command in enumerate(args.commands):
            line = game.raw(command)
            print(line, flush=True)
            answer = json.loads(line)
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
