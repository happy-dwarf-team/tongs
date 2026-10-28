"""PROTOTYPE (tongs#104). A marked scenario: it drives the game inside its stretch on purpose.

`events=first` answers every event on the way, so the stretch takes input. MARKED says so up
front; the record is marked and never counts as a measurement, but the run still passes or fails.
"""

LAUNCH = {
    "start": {"save": "russia-1719"},
    "cpu": "simulation",
    "workers": 8,
    "plugins": {"tongs": {"input": True}},
}
MARKED = "answers events with events=first inside the stretch"


def run(s):
    s.ready()
    s.call("speed 5")
    s.run_until("1720.1.1", speed=5, timeout=300)
    s.run_until("1730.1.1", speed=5, timeout=3600, events="first", measure="decade-events")
    s.read("player", expect={"tag": "RUS"})
    s.read("province 295", expect={"owner": "RUS"})
