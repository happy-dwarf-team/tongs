"""PROTOTYPE, throwaway (tongs#104). A fake game behind the pipe client's `call(command)`.

Answers the commands the two scenarios send, including the ones Tongs does not have yet
(`province`, `country`, `player`, `selection`, `province open`, the `client_calls` counts).
Nothing here talks to a real game. `fault` injects the cases a runner has to reject.
"""

PROVINCES = {
    295: {"id": 295, "name": "Moskva", "owner": "RUS", "controller": "RUS", "cores": ["RUS"],
          "development": 31.0, "land": True, "wasteland": False},
    1: {"id": 1, "name": "Stockholm", "owner": "SWE", "controller": "SWE", "cores": ["SWE"],
        "development": 26.0, "land": True, "wasteland": False},
}


class FakeGame:
    def __init__(self, fault=None):
        self.fault = fault
        self.date = "1719.3.17"
        self.speed = 2
        self.paused = True
        self.session = 1
        self.client_calls_total = 0
        self.game_calls_total = 0
        self.selected = None
        self.view_visible = False
        self.reads_until_view = 2  # the view shows only after a couple of frames

    def _state(self):
        return {"pid": 4242, "process_start_ms": 1761600000000, "session": self.session,
                "in_game": True, "date": self.date, "speed": self.speed, "paused": self.paused,
                "client_calls_total": self.client_calls_total, "game_calls_total": self.game_calls_total}

    def call(self, command):
        words = command.split()
        head = words[0]
        if head in ("ping", "state"):
            return {"ok": True, "version": "0.19.0-proto", **self._state()}
        if head == "threads":
            mask = "0x100" if self.fault == "main-thread-moved" else "0x10"
            return {"ok": True, "pools": [{"workers": 8}, {"workers": 8}],
                    "process_mask": "0xFFFF", "main_mask": mask, **self._state()}
        if head == "speed":
            self.speed = int(words[1])
            return {"ok": True, "speed": self.speed, "speed_internal": self.speed - 1, **self._state()}
        if head == "province" and words[1] == "open":
            pid = int(words[2])
            self.client_calls_total += 1
            self.selected = pid
            self.view_visible = True
            return {"ok": True, "province": pid, "view": {"province": pid, "visible": True},
                    "camera": "settled", "covered_by": None, **self._state()}
        if head == "province":
            pid = int(words[1])
            if pid not in PROVINCES:
                return {"ok": False, "error": "unknown_province", **self._state()}
            return {"ok": True, **PROVINCES[pid], **self._state()}
        if head == "country":
            tag = words[1]
            owned = [p for p, v in PROVINCES.items() if v["owner"] == tag]
            return {"ok": True, "tag": tag, "exists": bool(owned), "provinces": owned,
                    "capital": owned[0] if owned else None, **self._state()}
        if head == "player":
            return {"ok": True, "tag": "RUS", "player_tag": "RUS", "observed_tag": None,
                    "observer": False, **self._state()}
        if head == "selection":
            if self.reads_until_view > 0 and self.fault == "slow-view":
                self.reads_until_view -= 1
                return {"ok": True, "last_selected": self.selected,
                        "view": {"province": None, "visible": False}, **self._state()}
            return {"ok": True, "last_selected": self.selected,
                    "view": {"province": self.selected if self.view_visible else None,
                             "visible": self.view_visible}, **self._state()}
        if head == "run_until":
            target = words[1]
            start = self.date
            answer = {"ok": True, "reached": True, "start_date": start, "date": target,
                      "wall_ms": 95_000, "paused_ms": 0, "game_days": 290, "days_per_second": 3.05,
                      "inputs": 0, "client_calls": 0, "game_calls": 0}
            if "events=first" in words:
                answer.update(inputs=7, events_chosen=7)
            if self.fault == "event-pause" and target.startswith("1721"):
                answer.update(reached=False, ok=False, error="max_pause: paused 5 s",
                              date="1720.6.2", paused_ms=5000)
            if self.fault == "input-in-stretch" and target.startswith("1721"):
                answer["inputs"] = 1
            if self.fault == "old-tongs":
                del answer["client_calls"]
            self.date = answer["date"]
            self.paused = True
            return {**self._state(), **answer}
        if head == "console":
            self.game_calls_total += 1
            return {"ok": True, "output": "", **self._state()}
        return {"ok": False, "error": f"fake game does not know {head!r}"}
