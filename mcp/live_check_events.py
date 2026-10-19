"""Live check of event choice and of the heartbeat through the MCP server. NOT a
unit test: it runs the game and clicks event options, so use it only on a test
session.

Needs a loaded game with Tongs `enabled` and `input` on.

    uv run --frozen python live_check_events.py [OUT.jsonl]

Starts `python -m tongs_mcp` as a stdio subprocess (with a 2 s heartbeat), as
an MCP client would, and prints (and writes) one JSON line per step:

1. While the game is paused, a `tongs_wait_until` with progress gets heartbeats
   until `tongs_cancel` ends it.
2. The game runs in 30-day steps until an event is open; `tongs_event_list`
   shows it and `tongs_event_choose` picks option 1 by number. Then again until
   the next event, which is chosen by the text of its first enabled option.
"""

import json
import sys
import time

import anyio
from mcp import Client, StdioServerParameters

from live_check import add_days

OUT = sys.argv[1] if len(sys.argv) > 1 else None
HEARTBEAT_S = 2
MAX_STEPS = 24  # 30-day steps per event: two years
records: list[dict] = []


def log(step: str, **fields) -> None:
    record = {"step": step, **fields}
    records.append(record)
    print(json.dumps(record, ensure_ascii=False), flush=True)


async def main() -> None:
    params = StdioServerParameters(
        command=sys.executable, args=["-m", "tongs_mcp"], env={"TONGS_MCP_HEARTBEAT_S": str(HEARTBEAT_S)}
    )
    async with Client(params, read_timeout_seconds=3600) as client:

        async def call(tool: str, args: dict | None = None, **kwargs):
            started = time.monotonic()
            result = await client.call_tool(tool, args or {}, **kwargs)
            answer = result.structured_content
            log(
                tool,
                args=args or {},
                ms=round((time.monotonic() - started) * 1000),
                is_error=result.is_error,
                answer=answer,
            )
            return answer or {}

        state = await call("tongs_state")
        if not state.get("in_game"):
            raise SystemExit("no game loaded")
        await call("tongs_pause")

        # 1. Heartbeats while paused: progress keeps coming although no day passes.
        progress: list[tuple] = []

        async def on_progress(value, total, message):
            progress.append((round(time.monotonic(), 2), value, total, message))

        far = add_days(state["date"], 3650)
        async with anyio.create_task_group() as tg:

            async def wait_far():
                await call("tongs_wait_until", {"date": far, "timeout": 60}, progress_callback=on_progress)

            tg.start_soon(wait_far)
            await anyio.sleep(3 * HEARTBEAT_S + 1.5)
            await call("tongs_cancel")
        values = [p[1] for p in progress]
        log(
            "check_heartbeat",
            count=len(progress),
            increasing=all(b > a for a, b in zip(values, values[1:])),
            progress=progress,
        )

        # 2. Run until an event is open, then choose: first by number, then by text.
        for how in ("number", "text"):
            events = []
            for _ in range(MAX_STEPS):
                events = (await call("tongs_event_list")).get("events") or []
                if events:
                    break
                date = (await call("tongs_state"))["date"]
                await call(
                    "tongs_run_until",
                    {"date": add_days(date, 30), "speed": 5, "timeout": 300, "max_pause": 10, "progress": False},
                )
            if not events:
                log("check_event", how=how, error=f"no event in {MAX_STEPS * 30} days")
                continue
            event = events[0]
            if how == "number":
                args = {"option": 1}
            else:
                option = next((o for o in event["options"] if o.get("enabled")), event["options"][0])
                prefix = option["text"].split('"')[0][:40].strip()
                args = {"option_text": prefix, "option_match": "starts"}
            if len(events) > 1:
                args["event"] = event["number"]
            await anyio.sleep(0.6)  # options stay disabled for half a second after opening
            chosen = await call("tongs_event_choose", args)
            log(
                "check_event",
                how=how,
                id=event.get("id"),
                title=event.get("title"),
                options=[o.get("text") for o in event.get("options", [])],
                ok=chosen.get("ok"),
                closed=chosen.get("closed"),
                option=chosen.get("option"),
            )
            await call("tongs_event_list")
        await call("tongs_state")

    if OUT:
        with open(OUT, "w", encoding="utf-8") as f:
            for record in records:
                f.write(json.dumps(record, ensure_ascii=False) + "\n")


if __name__ == "__main__":
    anyio.run(main)
