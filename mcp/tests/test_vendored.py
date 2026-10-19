"""src/tongs_mcp/tongs.py is a verbatim copy of scripts/tongs.py, so the
package works when installed on its own; keep the two the same."""

from pathlib import Path

import pytest

ROOT = Path(__file__).resolve().parents[2]


def test_vendored_client_matches_scripts():
    original = ROOT / "scripts" / "tongs.py"
    if not original.exists():
        pytest.skip("not in the Tongs repository")
    vendored = ROOT / "mcp" / "src" / "tongs_mcp" / "tongs.py"
    assert vendored.read_bytes() == original.read_bytes(), "copy scripts/tongs.py to mcp/src/tongs_mcp/tongs.py"
