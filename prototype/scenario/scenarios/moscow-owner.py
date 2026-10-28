"""PROTOTYPE (tongs#104). Moscow belongs to Russia on 17 March 1719, and `province open` shows it."""

# Read with ast by `runner.py needs` before the start: literals only, no code runs.
LAUNCH = {
    "start": {"save": "russia-1719"},
    "cpu": "default",
    "plugins": {"tongs": {"input": True}},
}
MARKED = None


def run(s):
    s.ready()
    s.open_province(295)
    s.read("selection", expect={"view": {"visible": True, "province": 295}}, within_s=10)

    moscow = s.read("province 295")
    s.check(moscow["owner"] == "RUS" and moscow["controller"] == "RUS", "Moscow owned and controlled by RUS")
    s.check("RUS" in moscow["cores"], "RUS has a core on Moscow")

    russia = s.read("country RUS")
    s.check(russia["exists"] and 295 in russia["provinces"], "Moscow is in Russia's province list")
