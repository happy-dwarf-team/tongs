"""PROTOTYPE, throwaway (tongs#104): what a lab scenario looks like to its author.

Runs a scenario written as TOML or as Python against a fake game and prints what the
real runner would record. Nothing here starts, touches or reads a real game.

  python runner.py needs scenarios/moscow-owner.toml     # what run-scenario.ps1 reads before the start
  python runner.py run scenarios/moscow-owner.py         # dry run against the fake game
  python runner.py run scenarios/simulation-year.toml --fault event-pause

Faults: event-pause, input-in-stretch, old-tongs, main-thread-moved, slow-view.
"""
import ast
import hashlib
import json
import sys
import time
import tomllib
from pathlib import Path

from fakegame import FakeGame


# ---------------------------------------------------------------- launch needs (before the start)

def launch_needs(path):
    """The part run-scenario.ps1 needs before it starts the game, as JSON-ready data.

    TOML: the [launch] table and `marked`. Python: the module-level LAUNCH and MARKED literals,
    read with ast so that PowerShell gets them without running the scenario's code.
    """
    if path.suffix == ".toml":
        doc = tomllib.loads(path.read_text(encoding="utf-8"))
        return {"launch": doc["launch"], "marked": doc.get("marked")}
    found = {"LAUNCH": None, "MARKED": None}
    for node in ast.parse(path.read_text(encoding="utf-8")).body:
        if isinstance(node, ast.Assign) and len(node.targets) == 1 and getattr(node.targets[0], "id", None) in found:
            found[node.targets[0].id] = ast.literal_eval(node.value)
    return {"launch": found["LAUNCH"], "marked": found["MARKED"]}


# ---------------------------------------------------------------- the session a scenario drives

class AssertionFailed(Exception):
    pass


class Rejected(Exception):
    pass


def subset(expected, actual):
    """True when every key in `expected` has that value in `actual` (nested dicts by subset)."""
    if isinstance(expected, dict):
        return isinstance(actual, dict) and all(k in actual and subset(v, actual[k]) for k, v in expected.items())
    return expected == actual


class Session:
    """What a Python scenario's run(s) gets. The TOML steps map onto the same calls."""

    def __init__(self, game, marked):
        self.game = game
        self.marked = marked
        self.steps = []
        self.stretches = []
        self.lab = LabChecks(self)

    # one pipe exchange = one line of steps.jsonl
    def _exchange(self, command, kind):
        t0 = time.perf_counter()
        answer = self.game.call(command)
        self.steps.append({"command": command, "kind": kind, "ok": answer.get("ok"),
                           "wall_ms": round((time.perf_counter() - t0) * 1000, 2), "answer": answer})
        print(f"  [{kind:8}] {command:42} -> {'ok' if answer.get('ok') else answer.get('error')}")
        return answer

    def _expect(self, answer, expect, what):
        if expect is not None and not subset(expect, answer):
            got = {k: answer.get(k) for k in expect}
            raise AssertionFailed(f"{what}: expected {expect}, got {got}")

    # ---- the calls
    def ready(self):
        """STUB: the predicate is tongs#109's. Here: the pipe answers and the game is in game and paused."""
        a = self._exchange("state", "ready")
        if not (a.get("in_game") and a.get("paused")):
            raise Rejected("not ready")
        return a

    def call(self, command, expect=None):
        """Any pipe command as a setup step (input, client or game call: recorded, allowed)."""
        a = self._exchange(command, "setup")
        if not a.get("ok"):
            raise RuntimeError(f"{command}: {a.get('error')}")
        self._expect(a, expect, command)
        return a

    def open_province(self, province_id):
        """STUB of `province open ID` (tongs#101), a client call."""
        return self.call(f"province open {province_id}")

    def read(self, command, expect=None, within_s=None):
        """A world read (tongs#100). With within_s: read again until `expect` holds or time is up."""
        deadline = time.monotonic() + (within_s or 0)
        while True:
            a = self._exchange(command, "read")
            if not a.get("ok"):
                raise RuntimeError(f"{command}: {a.get('error')}")
            if expect is None or subset(expect, a) or time.monotonic() >= deadline:
                break
            time.sleep(0.05)
        self._expect(a, expect, command)
        return a

    def check(self, condition, what):
        print(f"  [check   ] {what:42} -> {'holds' if condition else 'FALSE'}")
        self.steps.append({"check": what, "holds": bool(condition)})
        if not condition:
            raise AssertionFailed(what)

    def run_until(self, date, measure=None, expect=None, **options):
        """A wait. Without `measure` it is setup; with it, a measured stretch judged by its marks."""
        opts = "".join(f" {k}={v}" for k, v in options.items())
        if measure is None:
            a = self._exchange(f"run_until {date}{opts}", "setup")
            if not a.get("reached"):
                raise Rejected(f"setup wait to {date}: {a.get('error')}")
            self._expect(a, expect, f"run_until {date}")
            return a
        before = self.game.call("state")
        a = self._exchange(f"run_until {date}{opts}", "measure")
        after = self.game.call("state")
        reasons = []
        if not a.get("reached"):
            reasons.append(f"not reached: {a.get('error')}")
        if a.get("paused_ms", 0) != 0:
            reasons.append(f"paused {a['paused_ms']} ms")
        for field in ("inputs", "client_calls", "game_calls"):
            if field not in a:
                reasons.append(f"{field} missing (Tongs too old)")
            elif a[field] != 0 and not self.marked:
                reasons.append(f"{field} = {a[field]}")
        for total in ("client_calls_total", "game_calls_total"):
            if before.get(total) != after.get(total) and not self.marked:
                reasons.append(f"{total} {before.get(total)} -> {after.get(total)}")
        stretch = {"name": measure, "start_date": a.get("start_date"), "end_date": a.get("date"),
                   "wall_ms": a.get("wall_ms"), "game_days": a.get("game_days"),
                   "counts": {k: a.get(k) for k in ("inputs", "client_calls", "game_calls")},
                   "verdict": "rejected" if reasons else ("marked" if self.marked else "valid"), "reasons": reasons}
        self.stretches.append(stretch)
        if reasons:
            raise Rejected(f"stretch {measure}: " + "; ".join(reasons))
        self._expect(a, expect, f"run_until {date}")
        return a


class LabChecks:
    """Named lab checks: things the lab's procedures demand that are not one world read."""

    def __init__(self, s):
        self.s = s

    def simulation_threads(self, workers=8):
        t = self.s._exchange("threads", "read")
        self.s.check([p["workers"] for p in t["pools"]] == [workers, workers], f"{workers} workers in both pools")
        self.s.check(t["process_mask"] == "0xFFFF" and t["main_mask"] == "0x10", "process 0-15, main thread CPU4")


# ---------------------------------------------------------------- TOML steps -> the same calls

def run_toml(s, steps):
    for step in steps:
        expect = step.get("expect")
        if step.get("ready"):
            s.ready()
        elif "open" in step:
            s.open_province(step["open"])
        elif "read" in step:
            s.read(step["read"], expect=expect, within_s=step.get("within_s"))
        elif "check" in step:
            getattr(s.lab, step["check"].replace("-", "_"))(**step.get("with", {}))
        elif "run_until" in step:
            options = {k: step[k] for k in ("speed", "timeout", "max_pause", "events") if k in step}
            s.run_until(step["run_until"], measure=step.get("measure"), expect=expect, **options)
        elif "do" in step:
            s.call(step["do"], expect=expect)
        else:
            raise ValueError(f"unknown step {step}")


def run(path, fault):
    needs = launch_needs(path)
    s = Session(FakeGame(fault), needs["marked"])
    print(f"scenario {path.name}  (fault: {fault or 'none'})")
    verdict, reason = "passed", None
    try:
        if path.suffix == ".toml":
            run_toml(s, tomllib.loads(path.read_text(encoding="utf-8"))["step"])
        else:
            namespace = {}
            exec(compile(path.read_text(encoding="utf-8"), str(path), "exec"), namespace)
            namespace["run"](s)
    except AssertionFailed as e:
        verdict, reason = "failed", str(e)
    except Rejected as e:
        verdict, reason = "rejected", str(e)
    except Exception as e:  # noqa: BLE001 - prototype
        verdict, reason = "error", f"{type(e).__name__}: {e}"
    record = {
        "schema": "eu4-lab-scenario-run.v1",
        "scenario": {"file": path.name, "sha256": hashlib.sha256(path.read_bytes()).hexdigest()[:16]},
        "launch_needs": needs["launch"],
        "launch": "<eu4-lab-game-launch.v1 verdict of Invoke-GameLaunch, embedded unchanged>",
        "tongs": {"version": s.steps[0]["answer"].get("version") if s.steps else None,
                  "commit": "<tongs tree commit>", "client_sha256": "<tongs.py hash>"},
        "marked": needs["marked"],
        "stretches": s.stretches,
        "steps": len(s.steps),
        "verdict": verdict,
        "reason": reason,
    }
    print("\nresult.json (abridged; steps.jsonl holds every exchange):")
    print(json.dumps(record, indent=2, ensure_ascii=False))
    return 0 if verdict == "passed" else 1


def main(argv):
    if len(argv) < 2 or argv[0] not in ("needs", "run"):
        print(__doc__)
        return 2
    path = Path(argv[1])
    if argv[0] == "needs":
        print(json.dumps(launch_needs(path), indent=2))
        return 0
    fault = argv[3] if len(argv) > 3 and argv[2] == "--fault" else None
    return run(path, fault)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
