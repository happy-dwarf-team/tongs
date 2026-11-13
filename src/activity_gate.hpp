#pragma once

#include "protocol.hpp"

#include <array>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace tongs {

// Input that goes through the game's event queue is refused with this error while the
// window is minimized: the game handles that queue only once the window is restored.
inline constexpr char kWindowMinimized[] =
    "the game window is minimized: the game handles input only after it is restored, so nothing was sent";

// Levels of Handlers::log (AnvilLogLevel).
inline constexpr int kLogInfo = 1;
inline constexpr int kLogWarning = 2;
inline constexpr int kLogError = 3;

// What a breaker turns off, until the game exits, after a call of it faulted in the game's code.
enum class Breaker { GameCalls, SpeedButtons, Navigation, MapMode };

// Why an activity did not happen: the name answers carry in "error", and the detail.
struct Refusal {
    std::string error;
    std::string detail;
    bool refused() const { return !error.empty(); }
};

// `mode` in a game call's answer (ADR 0001): with game_calls_ironman or game_calls_multiplayer
// on, once the main thread has read the mode; a refusal before that has none.
std::string ModeMembers(const GameCallPolicy& policy, const ConsoleRaw& raw);

// Runs the activities of the pipe's commands: the inputs, client calls and game calls
// Tongs carries out for a client (GLOSSARY.md). A command declares its command type in
// the command table and gives the gate its own checks and its action; the gate applies
// the rules of that type as the ADRs define them: the switch (`input`, or `game_calls`
// for game calls), Anvil's suspension, the breaker, the turn (one activity at a time
// across all clients), the count of every wait in progress, the total since the save was
// loaded, the `activity` notification and the Anvil log. Its counters, totals and
// breakers belong to one plugin instance (Handlers::gate); tongs.navigation shares them.
class ActivityGate {
public:
    class Activity;
    class Part;

    // What a command gives Run, in the order the gate uses it.
    struct Plan {
        // After the command in the Anvil log lines: the console line, the save, the province.
        std::string subject;
        // A client call's own breaker (game calls always have Breaker::GameCalls; inputs none).
        std::optional<Breaker> breaker;
        // Before the switch: the command's checks of its arguments, when they are to win over
        // the switch (and, for a game call, be logged). Optional.
        std::function<Refusal()> first;
        // After the switch and Anvil's suspension, before the breaker: whether the command can
        // run here (a reader that is not set up, a console command refused by name). Optional.
        std::function<Refusal()> check;
        // The answer of a refusal before `act`: the gate's, or of `first` or `check`.
        std::function<std::string(const Refusal&)> refuse;
        // With the turn held: the activity itself; returns the answer.
        std::function<std::string(Activity&)> act;
    };

    // Runs the activity of a request's command, whose type the command table gives (never
    // read): first, the switch, the suspension, check, the breaker, the turn, then act. An
    // input counts when the activity ends, once, if anything was sent, pressed, posted or
    // chosen through Activity::handlers().
    std::string Run(const Request& request, const Plan& plan);

    // The switch of a command type: input and client calls need `input = true` (and the
    // input functions), game calls `game_calls = true`; a read needs none.
    static Refusal Switch(const Handlers& handlers, CommandType type);
    // While Anvil suspends Tongs (an ironman or multiplayer game it was not allowed in).
    static Refusal Suspension(const Handlers& handlers);
    static Refusal TrippedRefusal(Breaker breaker);

    bool Tripped(Breaker breaker) const;
    void Trip(Breaker breaker);

    // Activities of all clients so far, for `inputs`, `client_calls` and `game_calls` of a wait.
    struct Counts {
        std::uint64_t inputs = 0;
        std::uint64_t client_calls = 0;
        std::uint64_t game_calls = 0;
    };
    Counts counts() const;

    // Client calls or game calls since the save `state` was read in was loaded, for `state`.
    std::uint64_t Total(CommandType type, const StateSnapshot& state) const;

    // A client call or game call made outside an activity of a pipe request: by another
    // plugin through tongs.navigation (ADR 0004), or a save load whose request stopped
    // waiting for it. Counts it, adds it to the total of the save `save` was read in and
    // publishes the `activity` notification (`members` go after "command").
    void Counted(const Handlers& handlers, CommandType type, std::string_view command, int client,
                 const StateSnapshot& save, std::string_view members = {});

private:
    struct Total_ {
        std::uint64_t load = 0;
        std::uintptr_t session = 0;
        std::int32_t date_raw = 0;
        std::uint64_t count = 0;
    };

    std::uint64_t Count(CommandType type);
    std::uint64_t AddTotal(CommandType type, const StateSnapshot& state);
    void Publish(const Handlers& handlers, CommandType type, std::string_view command, int client,
                 std::string_view members, std::uint64_t count) const;

    std::mutex turn_;
    std::atomic<std::uint64_t> inputs_{0};
    std::atomic<std::uint64_t> client_calls_{0};
    std::atomic<std::uint64_t> game_calls_{0};
    std::array<std::atomic<bool>, 4> tripped_{};
    mutable std::mutex totals_mutex_;
    Total_ game_total_;
    Total_ client_total_;
};

// The handlers of an input, or of a part of one counted under a name of its own: they
// notice whether anything was sent, pressed, posted or chosen, and the part counts as one
// input when it ends if it was. Input through the event queue is refused while the game
// window is minimized.
class ActivityGate::Part {
public:
    Part(ActivityGate& gate, const Handlers& handlers, std::string command, int client);
    ~Part();
    Part(const Part&) = delete;
    Part& operator=(const Part&) = delete;

    const Handlers& handlers() const { return handlers_; }

private:
    ActivityGate& gate_;
    Handlers handlers_;
    std::string command_;
    int client_;
    bool sent_ = false;
};

// One activity while Run carries it out: what its action needs of the gate.
class ActivityGate::Activity {
public:
    Activity(const Activity&) = delete;
    Activity& operator=(const Activity&) = delete;

    // The handlers the action acts through: an input's count them (Part).
    const Handlers& handlers() const;

    // A part of an input counted under a name of its own (`run_until events=first: choice`),
    // within the activity's turn. run_until's own presses use the request's handlers and are
    // not counted (ADR 0005).
    Part CountAs(std::string command) const;

    // The turn, given up while run_until waits for the date and taken again to act.
    void Release();
    void Take();

    // The activity's breaker (a client call's own, or game calls'), for a check on the main
    // thread right before the call, and tripping it after a fault in the game's code.
    bool Tripped() const;
    void Trip();

    // ADR 0003: a client call that would change game state is a game call. On the main
    // thread before the call: refused with needs_game_calls (`detail`) without
    // `game_calls = true`, when game calls are tripped, and by ADR 0001's ironman and
    // multiplayer refusals; otherwise the activity counts and logs as a game call, `why`
    // in the log. mode() has the answer's `mode` members.
    Refusal Promote(std::string detail, std::string why);
    bool promoted() const { return promoted_; }
    const std::string& mode() const { return mode_; }

    // The call was made: counts once (a client call, or a game call when it is one or was
    // promoted), publishes the `activity` notification (`members` after "command") and logs
    // a promotion.
    void Called(std::string_view members = {});
    // Adds the call to the total of the save `save` was read in (once) and returns that total.
    std::uint64_t Total(const StateSnapshot& save);

    // Logs a refusal as the type's ADR says (game calls and client calls; not inputs).
    void Refused(const Refusal& refusal) const;
    // "<command> <subject> (pipe N, <date>): <what>" in the Anvil log.
    void Log(int level, const std::string& what) const;

private:
    friend class ActivityGate;
    Activity(ActivityGate& gate, const Request& request, const Plan& plan, std::optional<Breaker> breaker);

    ActivityGate& gate_;
    const Request& request_;
    const Plan& plan_;
    std::optional<Breaker> breaker_;
    std::unique_lock<std::mutex> turn_;
    std::optional<Part> input_;  // an input's handlers; destroyed (counted) before the turn ends
    bool promoted_ = false;
    std::string mode_;
    std::string why_;
    bool totalled_ = false;
    std::uint64_t total_ = 0;
    StateSnapshot saw_;  // the save the call belongs to, for the log's date
};

}  // namespace tongs
