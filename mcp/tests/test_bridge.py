"""The Linux/Proton transport: under Wine and Proton, Tongs serves the pipe's commands on a socket on 127.0.0.1 (its
`bridge` setting), and the client reaches it from native Linux. These tests stand in for Tongs with a socket server in
this process, so they need neither Wine nor the game; the real server is tested in tests/test_bridge.cpp."""
import json
import socket
import sys
import threading

import pytest

from tongs_mcp import tongs
from tongs_mcp.tongs import Tongs

posix = pytest.mark.skipif(sys.platform == "win32", reason="Windows opens the pipe itself")


class FakeRelay:
    """Speaks the socket's protocol: `RELAY <token> <pipe>` answered `OK`, then JSON lines (ping, state, echo)."""

    def __init__(self, token="secret", serve_pipe="tongs"):
        self.token = token
        self.serve_pipe = serve_pipe
        self.requests: list[str] = []
        self.listener = socket.socket()
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(4)
        self.port = self.listener.getsockname()[1]
        threading.Thread(target=self._accept, daemon=True).start()

    def _accept(self):
        while True:
            try:
                client, _ = self.listener.accept()
            except OSError:
                return
            threading.Thread(target=self._serve, args=(client,), daemon=True).start()

    def _serve(self, client):
        first = b""
        while not first.endswith(b"\n"):
            byte = client.recv(1)
            if not byte:
                return
            first += byte
        self.requests.append(first.decode().strip())
        parts = first.decode().split()
        if len(parts) != 3 or parts[1] != self.token:
            client.sendall(b"ERR bad token\n")
        elif parts[2] != self.serve_pipe:
            client.sendall(b"ERR pipe not served\n")
        else:
            client.sendall(b"OK\n")
            buffer = b""
            while True:
                chunk = client.recv(4096)
                if not chunk:
                    return
                buffer += chunk
                while b"\n" in buffer:
                    line, _, buffer = buffer.partition(b"\n")
                    answer = {"ok": True, "command": "ping", "pid": 42, "process_start_ms": 7} if line == b"ping" else {"ok": True, "echo": line.decode()}
                    client.sendall(json.dumps(answer).encode() + b"\n")
        client.close()

    def state_file(self, directory, token=None):
        path = directory / "bridge.json"
        path.write_text(json.dumps({"port": self.port, "token": token or self.token, "pid": 1}))
        return path

    def close(self):
        self.listener.close()


@pytest.fixture
def relay():
    fake = FakeRelay()
    yield fake
    fake.close()


@posix
def test_the_client_talks_to_tongs_through_the_socket(relay, tmp_path, monkeypatch):
    monkeypatch.setenv("TONGS_BRIDGE", str(relay.state_file(tmp_path)))
    with Tongs(timeout_s=2, expect_pid=42, expect_start_ms=7) as t:
        assert t.call("ping")["pid"] == 42
        assert t.raw("hello") == '{"ok": true, "echo": "hello"}'
    assert relay.requests[0] == "RELAY secret tongs"


@posix
def test_a_wrong_secret_is_refused(relay, tmp_path, monkeypatch):
    monkeypatch.setenv("TONGS_BRIDGE", str(relay.state_file(tmp_path, token="wrong")))
    with pytest.raises(OSError, match="refused tongs: ERR bad token"):
        tongs._open_bridge("tongs", 0.3)


@posix
def test_a_pipe_tongs_does_not_serve_is_refused(relay, tmp_path, monkeypatch):
    monkeypatch.setenv("TONGS_BRIDGE", str(relay.state_file(tmp_path)))
    with pytest.raises(OSError, match="pipe not served"):
        tongs._open_bridge("something-else", 0.3)


@posix
def test_no_state_file_means_no_socket_yet(tmp_path, monkeypatch):
    monkeypatch.setenv("TONGS_BRIDGE", str(tmp_path / "missing.json"))
    with pytest.raises(OSError, match="cannot reach Tongs through its socket"):
        Tongs(timeout_s=0.2)


@posix
def test_the_client_waits_for_the_socket_to_start(relay, tmp_path, monkeypatch):
    path = tmp_path / "bridge.json"
    monkeypatch.setenv("TONGS_BRIDGE", str(path))
    threading.Timer(0.3, relay.state_file, args=(tmp_path,)).start()
    with Tongs(timeout_s=3) as t:
        assert t.call("ping")["ok"]


def test_the_state_file_defaults_to_the_state_directory(monkeypatch):
    monkeypatch.delenv("TONGS_BRIDGE", raising=False)
    monkeypatch.setenv("XDG_STATE_HOME", "/data/state")
    assert tongs._bridge_state_path().replace("\\", "/") == "/data/state/tongs/bridge.json"
    monkeypatch.delenv("XDG_STATE_HOME")
    monkeypatch.setenv("HOME", "/home/me")
    assert tongs._bridge_state_path().replace("\\", "/").endswith("/.local/state/tongs/bridge.json")
