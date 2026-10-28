"""PROTOTYPE (tongs#104). The SIMULATION year 1720 as a scenario.

Warm-up to 1 January 1720 is setup; the year to 1 January 1721 is the measured stretch.
"""

LAUNCH = {
    "start": {"save": "russia-1719"},
    "cpu": "simulation",
    "workers": 8,
    "plugins": {"tongs": {"input": True}},
}
MARKED = None

WARMUP, END = "1720.1.1", "1721.1.1"


def run(s):
    s.ready()
    s.lab.simulation_threads(workers=8)
    s.call("speed 5", expect={"speed": 5, "speed_internal": 4})

    s.run_until(WARMUP, speed=5, timeout=300, max_pause=5)
    year = s.run_until(END, speed=5, timeout=300, max_pause=5, measure="year-1720")
    s.check(year["start_date"] in (WARMUP, "1720.1.2"), "stretch starts at the warm-up date or the day after")

    s.lab.simulation_threads(workers=8)
    s.read("player", expect={"tag": "RUS"})
