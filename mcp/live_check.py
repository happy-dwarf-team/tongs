"""Live check of the MCP server against a running game. NOT a unit test: it
presses keys and clicks in the game, runs it for a month and makes one game
call (`console echo x`), so the session becomes a marked run.

Needs a loaded game with Tongs `enabled`, `input` and `game_calls` on.

    uv run --frozen python live_check.py [OUT.jsonl]

Starts `python -m tongs_mcp` as a stdio subprocess, as an MCP client would,
calls the tools in order and prints (and writes) one JSON line per step.
"""

import json
import sys
import time

import anyio
from mcp import Client, StdioServerParameters

from tongs_mcp.server import game_days
from tongs_mcp.tongs import Tongs

OUT = sys.argv[1] if len(sys.argv) > 1 else None
records: list[dict] = []


def log(step: str, **fields) -> None:
    record = {"step": step, **fields}
    records.append(record)
    print(json.dumps(record, ensure_ascii=False), flush=True)


def summary(result) -> dict:
    return {
        "is_error": result.is_error,
        "content": [c.type for c in result.content],
        "answer": result.structured_content,
    }


def add_days(date: str, days: int) -> str:
    """A date `days` later (EU4 calendar), as y.m.d."""
    total = int(game_days(date)) + days
    year, rest = divmod(total, 365)
    starts = (0, 31, 59, 90, 120, 151, 181, 212, 243, 273, 304, 334, 365)
    month = next(m for m in range(12) if starts[m + 1] > rest)
    return f"{year}.{month + 1}.{rest - starts[month] + 1}"


async def main() -> None:
    params = StdioServerParameters(command=sys.executable, args=["-m", "tongs_mcp"])
    async with Client(params, read_timeout_seconds=3600) as client:
        tools = (await client.list_tools()).tools
        log("list_tools", count=len(tools), names=sorted(t.name for t in tools))

        async def call(tool: str, args: dict | None = None, **kwargs):
            started = time.monotonic()
            result = await client.call_tool(tool, args or {}, **kwargs)
            log(tool, args=args or {}, ms=round((time.monotonic() - started) * 1000), **summary(result))
            return result

        state = (await call("tongs_state")).structured_content
        if not state.get("in_game"):
            raise SystemExit("no game loaded")
        for tool in ("tongs_window", "tongs_threads", "tongs_frames"):
            await call(tool)
        await call("tongs_raw", {"line": "ping"})
        await call("tongs_screenshot", {"name": "mcp-live", "image": True})

        # A speed button by name: find it, click it, check the speed, click back.
        await call("tongs_pause")
        await call("tongs_speed", {"speed": 2})
        await call("tongs_ui_find", {"selector": "speed_controls/button_speedup"})
        click = await call("tongs_ui_click", {"selector": "speed_controls/button_speedup"})
        log("check_speed_up", speed=click.structured_content.get("speed"), expected=3)
        await call("tongs_ui_click", {"selector": "speed_controls/button_speeddown"})

        await call("tongs_event_list")

        # A month at speed 5, with progress notifications.
        progress: list[tuple] = []

        async def on_progress(value, total, message):
            progress.append((value, total, message))

        start = (await call("tongs_state")).structured_content["date"]
        target = add_days(start, 30)
        await call(
            "tongs_run_until",
            {"date": target, "speed": 5, "timeout": 600, "max_pause": 30, "events": "first"},
            progress_callback=on_progress,
        )
        log("progress", count=len(progress), first=progress[:2], last=progress[-1:])

        # tongs_cancel ends a wait in progress.
        far = add_days(target, 3650)
        async with anyio.create_task_group() as tg:
            waited: dict = {}

            async def wait_far():
                waited["result"] = await call("tongs_wait_until", {"date": far, "timeout": 120, "progress": False})

            tg.start_soon(wait_far)
            await anyio.sleep(1.5)
            await call("tongs_cancel")
        log("check_cancel", error=(waited["result"].structured_content or {}).get("error"))

        # Cancelling the MCP call sends `cancel`: a wait of another pipe client ends too.
        def other_wait() -> dict:
            with Tongs() as game:
                return game.call(f"wait_until {far} timeout=60")

        async with anyio.create_task_group() as tg:
            other: dict = {}

            async def run_other():
                started = time.monotonic()
                other["answer"] = await anyio.to_thread.run_sync(other_wait)
                other["ms"] = round((time.monotonic() - started) * 1000)

            tg.start_soon(run_other)
            await anyio.sleep(0.5)
            with anyio.move_on_after(1.5):
                await client.call_tool("tongs_wait_until", {"date": far, "timeout": 60, "progress": False})
        log("check_abort", other_ms=other["ms"], other_error=other["answer"].get("error"))

        before = (await call("tongs_state")).structured_content.get("game_calls_total")
        echo = await call("tongs_console", {"line": "echo x"})
        log("check_console", text=echo.structured_content.get("text"), game_calls_total_before=before)
        await call("tongs_text", {"text": "a\nstate"})  # refused by the adapter, nothing sent
        await call("tongs_state")

    if OUT:
        with open(OUT, "w", encoding="utf-8") as f:
            for record in records:
                f.write(json.dumps(record, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    anyio.run(main)
