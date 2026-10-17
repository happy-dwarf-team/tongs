"""Client for the Tongs pipe (\\\\.\\pipe\\tongs). Standard library only, Windows.

Command line: one answer line per command, in order, over one connection.

    python tongs.py state
    python tongs.py pause "speed 5" "run_until 1720.1.1"

As a module:

    from tongs import Tongs
    with Tongs() as game:
        print(game.call("state")["date"])
        result = game.call("run_until 1720.1.1 speed=5", check=True)
"""
import json
import sys
import time

PIPE = r"\\.\pipe\tongs"


class TongsError(RuntimeError):
    """An answer with "ok": false, raised by call(..., check=True)."""

    def __init__(self, command, answer):
        super().__init__(f"{command}: {answer.get('error', 'failed')}")
        self.command = command
        self.answer = answer


class Tongs:
    def __init__(self, timeout_s=2.0):
        # The pipe has a few instances; when all are busy, opening fails until one frees up.
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

    def close(self):
        self._pipe.close()

    def __enter__(self):
        return self

    def __exit__(self, *exc):
        self.close()

    def raw(self, command):
        """Sends one command and returns its answer line as text."""
        self._pipe.write(command.encode("utf-8") + b"\n")
        while b"\n" not in self._buffer:
            chunk = self._pipe.read(4096)
            if not chunk:
                raise ConnectionError("the pipe closed without an answer")
            self._buffer += chunk
        line, _, self._buffer = self._buffer.partition(b"\n")
        return line.decode("utf-8")

    def call(self, command, check=False):
        """Sends one command and returns the parsed answer."""
        answer = json.loads(self.raw(command))
        if check and not answer.get("ok"):
            raise TongsError(command, answer)
        return answer


def main(argv):
    commands = argv or ["state"]
    with Tongs() as game:
        ok = True
        for command in commands:
            line = game.raw(command)
            print(line, flush=True)
            ok = ok and json.loads(line).get("ok", False)
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
