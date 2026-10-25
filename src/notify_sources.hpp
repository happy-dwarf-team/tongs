#pragma once

#include "events.hpp"
#include "notifications.hpp"
#include "protocol.hpp"

#include <cstdint>
#include <deque>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace tongs {

// Where notifications come from (docs/design.md, "Notifications"). Three
// Tongs threads, each idle while nobody subscribes to its kinds:
//
// - state and date: the state read every poll interval (10 ms), as in
//   wait_until, without the main thread, so they also work while a save loads;
// - event: the count of the session's event window list read every 10 ms;
//   when it changes (and once a second while events are open) the open events
//   are read as a main-thread read and compared; game.log, read from its end
//   every 100 ms (every poll while a closed event waits for its line), names
//   the choices Tongs did not make;
// - error_log: error.log read from its end every 100 ms.
//
// `activity` is published by the commands themselves (protocol.cpp). A
// source that starts takes what it sees first as the baseline, so only later
// changes are reported.

// The members of each `state` notification between two snapshots, in order:
// leaving or entering the game, pause or resume, speed.
std::vector<std::string> StateChanges(const StateSnapshot& before, const StateSnapshot& after);

struct KnownEvent {
    std::uintptr_t controller = 0;
    std::string id;     // UTF-8
    std::string title;
    std::vector<EventOption> options;  // as the event opened
};

// Between the events known so far and the events now open: the events that
// closed and the members of each `opened` notification. Updates `known`.
struct EventDiff {
    std::vector<KnownEvent> closed;   // in the order they were known: oldest first
    std::vector<std::string> opened;  // oldest first
};
EventDiff DiffEvents(std::vector<KnownEvent>& known, const std::vector<EventView>& now);

// The members of a `closed` notification; `chosen` is JSON, or "null".
std::string ClosedMembers(const KnownEvent& event, std::string_view chosen);

// A choice game.log names. The game writes "[eventwindow.cpp:486]: Selected
// event option N for event X" for every click on an option (the player's mouse
// as well as Tongs) and "[eventwindow.cpp:494]: Auto-selected event option N
// for event X" when it chooses itself at the deadline; N is the option_index,
// X the script id or the numeric id as digits.
struct LoggedChoice {
    bool automatic = false;
    int option_index = -1;
    std::string id;
    bool operator==(const LoggedChoice&) const = default;
};
std::optional<LoggedChoice> ParseChoiceLine(std::string_view line);

// `chosen` for a choice only game.log names: `by` "player" or "auto", with the
// option as the event opened with it (number, text and key null when the
// option was not on screen).
std::string LoggedChoiceJson(const KnownEvent& event, const LoggedChoice& choice);

// Pairs the events that closed with the choices game.log names. The game
// writes the line when the option is chosen, frames before the event leaves
// its lists, so the line is usually read by the time the event is seen closed;
// an event Tongs did not choose still waits up to kWaitMs for it. A choice
// Tongs noted is reported at once and takes its line when the line comes.
//
// Lines go to the closed events with the same id, oldest line to the event
// known longest. When events with one id close in the same read, the lines do
// not say which event got which option, so `chosen` names a logged choice for
// them only when their lines agree.
class ChoiceMatcher {
public:
    // The events one read found closed, each with the choice Tongs noted for it.
    void Closed(std::vector<std::pair<KnownEvent, std::optional<std::string>>> events, std::uint64_t now_ms);
    void Logged(LoggedChoice choice, std::uint64_t now_ms);
    // The members of each `closed` notification now due, in order. Without
    // `wait` (game.log is not followed), none waits for its line.
    std::vector<std::string> Due(std::uint64_t now_ms, bool wait);
    bool waiting() const { return !pending_.empty(); }
    void Reset();

    static constexpr int kWaitMs = 1000;
    static constexpr int kKeepLineMs = 3000;  // a line no closed event took: an event Tongs did not know

private:
    struct Pending {
        KnownEvent event;
        std::optional<std::string> tongs;
        std::uint64_t batch = 0;
        std::uint64_t deadline = 0;
        bool published = false;
    };
    struct Line {
        LoggedChoice choice;
        std::uint64_t at = 0;
    };
    std::deque<Pending> pending_;
    std::deque<Line> lines_;
    std::uint64_t batches_ = 0;
};

class NotificationSources {
public:
    NotificationSources(Handlers handlers, NotificationHub& hub);

    // Starts the three threads; they live as long as the process.
    void Start();

    // One poll of each source, as the threads run them (and the tests).
    void PollState();
    void PollEvents();
    // Returns the bytes read, so the thread reads again at once after a full chunk.
    std::size_t PollErrorLog();
    // Forget the baseline, as a source does when nobody subscribes any more.
    void ResetState() { state_seeded_ = false; }
    void ResetEvents();
    void ResetErrorLog();

    static constexpr int kEventCheckMs = 1000;
    static constexpr int kEventReadTimeoutMs = 2000;
    static constexpr int kErrorLogPollMs = 100;
    static constexpr int kLocateRetryMs = 1000;
    static constexpr std::size_t kErrorLogChunk = 1 << 20;

private:
    std::optional<std::int32_t> Date();
    void PollEventList();
    // Reads new lines of game.log every kErrorLogPollMs, or now with `force`;
    // returns whether game.log is followed.
    bool FollowGameLog(bool force);
    void PublishClosed(bool wait, bool in_session = true);

    Handlers h_;
    NotificationHub& hub_;

    bool state_seeded_ = false;
    StateSnapshot last_;

    bool events_seeded_ = false;
    int last_count_ = 0;
    std::uint64_t next_event_check_ = 0;
    std::vector<KnownEvent> known_;
    ChoiceMatcher matcher_;
    std::string game_log_path_;
    std::uint64_t next_game_log_ = 0;  // the next read, or the next try to locate it
    LineTail game_tail_;

    std::string log_path_;
    std::uint64_t next_locate_ = 0;
    LineTail tail_;
};

}  // namespace tongs
