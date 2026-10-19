#pragma once

#include "events.hpp"
#include "notifications.hpp"
#include "protocol.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace tongs {

// Where notifications come from (docs/design.md, "Notifications"). Three
// Tongs threads, each idle while nobody subscribes to its kinds:
//
// - state and date: the state read every poll interval (10 ms), as in
//   wait_until, without the main thread, so they also work while a save loads;
// - event: the count of the session's event window list read every 10 ms;
//   when it changes (and once a second while events are open) the open events
//   are read as a main-thread read and compared;
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
};

// The members of each `event` notification between the events known so far
// and the events now open (closed ones first); updates `known`. `chosen`
// answers the option Tongs chose for a closed event, as JSON, when it did.
using ChoiceLookup = std::function<std::optional<std::string>(std::uintptr_t controller, const std::string& id)>;
std::vector<std::string> EventChanges(std::vector<KnownEvent>& known, const std::vector<EventView>& now,
                                      const ChoiceLookup& chosen);

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

    Handlers h_;
    NotificationHub& hub_;

    bool state_seeded_ = false;
    StateSnapshot last_;

    bool events_seeded_ = false;
    int last_count_ = 0;
    std::uint64_t next_event_check_ = 0;
    std::vector<KnownEvent> known_;

    std::string log_path_;
    std::uint64_t next_locate_ = 0;
    LineTail tail_;
};

}  // namespace tongs
