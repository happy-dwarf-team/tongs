#include "notify_sources.hpp"

#include <algorithm>
#include <thread>
#include <utility>

namespace tongs {
namespace {

const char* Bool(bool value) { return value ? "true" : "false"; }

std::string Quote(std::string_view text) { return "\"" + JsonEscape(text) + "\""; }

// Speed and pause of a loaded game, the tail of every `state` notification in a game.
std::string InGameFields(const StateSnapshot& s) {
    return ",\"speed\":" + std::to_string(s.speed + 1) + ",\"paused\":" + Bool(s.paused) +
           ",\"paused_flag\":" + Bool(s.paused_flag);
}

std::string Change(const char* change) { return std::string(",\"change\":\"") + change + "\""; }

std::string OptionsJson(const EventView& e) {
    std::string out = ",\"options\":[";
    for (std::size_t i = 0; i < e.options.size(); ++i) {
        const EventOption& o = e.options[i];
        out += (i ? "," : "") + std::string("{\"number\":") + std::to_string(o.number) + ",\"text\":" + Quote(o.text) +
               ",\"option_index\":" + std::to_string(o.index) + ",\"option_key\":" + Quote(o.key) + "}";
    }
    return out + "]";
}

// The controllers the events view would list: waiting for a choice, with a filled window.
std::vector<std::pair<std::uintptr_t, std::string>> OpenKeys(const EventsRaw& raw) {
    std::vector<std::pair<std::uintptr_t, std::string>> keys;
    for (const OpenEventRaw& e : raw.events) {
        if (e.waiting && e.window != 0) keys.emplace_back(e.controller, GameTextToUtf8(e.id));
    }
    std::sort(keys.begin(), keys.end());
    return keys;
}

std::vector<std::pair<std::uintptr_t, std::string>> KnownKeys(const std::vector<KnownEvent>& known) {
    std::vector<std::pair<std::uintptr_t, std::string>> keys;
    for (const KnownEvent& e : known) keys.emplace_back(e.controller, e.id);
    std::sort(keys.begin(), keys.end());
    return keys;
}

}  // namespace

std::vector<std::string> StateChanges(const StateSnapshot& before, const StateSnapshot& after) {
    std::vector<std::string> out;
    const bool new_save = before.in_game && after.in_game && before.session != after.session;
    if (before.in_game && (!after.in_game || new_save)) {
        std::string m = Change("left_game");
        if (!after.in_game) m += ",\"reason\":" + Quote(after.reason ? after.reason : "");
        out.push_back(m);
    }
    if (after.in_game && (!before.in_game || new_save)) {
        out.push_back(Change("entered_game") + InGameFields(after));
        return out;
    }
    if (!before.in_game || !after.in_game) return out;
    if (before.paused != after.paused) out.push_back(Change(after.paused ? "paused" : "resumed") + InGameFields(after));
    if (before.speed != after.speed) {
        out.push_back(Change("speed") + InGameFields(after) + ",\"previous_speed\":" + std::to_string(before.speed + 1));
    }
    return out;
}

std::vector<std::string> EventChanges(std::vector<KnownEvent>& known, const std::vector<EventView>& now,
                                      const ChoiceLookup& chosen) {
    std::vector<std::string> out;
    const auto open_now = [&now](const KnownEvent& k) {
        return std::any_of(now.begin(), now.end(), [&k](const EventView& v) { return v.controller == k.controller && v.id == k.id; });
    };
    for (const KnownEvent& k : known) {
        if (open_now(k)) continue;
        const std::optional<std::string> choice = chosen ? chosen(k.controller, k.id) : std::nullopt;
        out.push_back(Change("closed") + ",\"id\":" + Quote(k.id) + ",\"title\":" + Quote(k.title) +
                      ",\"chosen\":" + (choice ? *choice : std::string("null")));
    }
    std::vector<KnownEvent> next;
    // `now` is topmost first; report openings oldest first.
    for (auto it = now.rbegin(); it != now.rend(); ++it) {
        const EventView& v = *it;
        const auto same = [&v](const KnownEvent& k) { return k.controller == v.controller && k.id == v.id; };
        const auto old = std::find_if(known.begin(), known.end(), same);
        if (old == known.end()) {
            out.push_back(Change("opened") + ",\"id\":" + Quote(v.id) + ",\"title\":" + Quote(v.title) + OptionsJson(v));
            next.push_back({v.controller, v.id, v.title});
        } else {
            next.push_back({v.controller, v.id, v.title.empty() ? old->title : v.title});
        }
    }
    known = std::move(next);
    return out;
}

NotificationSources::NotificationSources(Handlers handlers, NotificationHub& hub) : h_(std::move(handlers)), hub_(hub) {}

std::optional<std::int32_t> NotificationSources::Date() {
    if (!h_.read_state) return std::nullopt;
    const StateSnapshot s = h_.read_state();
    return s.in_game ? std::optional<std::int32_t>(s.date_raw) : std::nullopt;
}

void NotificationSources::PollState() {
    if (!h_.read_state) return;
    const StateSnapshot s = h_.read_state();
    if (!state_seeded_) {
        state_seeded_ = true;
        last_ = s;
        return;
    }
    const std::optional<std::int32_t> date = s.in_game ? std::optional<std::int32_t>(s.date_raw) : std::nullopt;
    if (hub_.Wants(NotificationKind::State)) {
        for (const std::string& m : StateChanges(last_, s)) hub_.Publish(NotificationKind::State, date, m);
    }
    if (last_.in_game && s.in_game && last_.session == s.session && hub_.Wants(NotificationKind::Date)) {
        hub_.PublishDate(last_.date_raw, s.date_raw);
    }
    last_ = s;
}

void NotificationSources::ResetEvents() {
    events_seeded_ = false;
    last_count_ = 0;
    next_event_check_ = 0;
    known_.clear();
}

void NotificationSources::PollEvents() {
    if (!h_.dispatcher || !h_.read_ui || !h_.read_events || !h_.count_event_windows || !h_.now_ms) return;
    const int count = h_.count_event_windows();
    const std::uint64_t now = h_.now_ms();
    if (count < 0) {
        // No session: whatever was open is gone with it.
        if (events_seeded_ && !known_.empty()) {
            for (const std::string& m : EventChanges(known_, {}, [this](std::uintptr_t c, const std::string& id) {
                     return hub_.TakeChoice(c, id);
                 })) {
                hub_.Publish(NotificationKind::Event, std::nullopt, m);
            }
        }
        known_.clear();
        events_seeded_ = true;
        last_count_ = count;
        return;
    }
    bool full = !events_seeded_ || count != last_count_;
    if (!full) {
        if ((count == 0 && known_.empty()) || now < next_event_check_) return;
        // Once a second while events are open: the cheap list alone, and the
        // widget tree only when it differs from what is known.
        EventsRaw raw;
        std::string error;
        const auto& read_events = h_.read_events;
        const Dispatcher::Outcome o = h_.dispatcher->Run(
            Dispatcher::Kind::Read,
            [&raw, &error, &read_events] {
                error = read_events(raw);
                return std::string();
            },
            kEventReadTimeoutMs);
        next_event_check_ = h_.now_ms() + kEventCheckMs;
        if (!o.ran || !error.empty() || OpenKeys(raw) == KnownKeys(known_)) return;
        full = true;
    }
    UiTree tree;
    EventsRaw raw;
    std::string error;
    const auto& read_ui = h_.read_ui;
    const auto& read_events = h_.read_events;
    const Dispatcher::Outcome o = h_.dispatcher->Run(
        Dispatcher::Kind::Read,
        [&] {
            error = read_events(raw);
            // The tree only for the titles and options of events that are open.
            if (error.empty() && !OpenKeys(raw).empty()) error = read_ui(tree);
            return std::string();
        },
        kEventReadTimeoutMs);
    if (!o.ran || !error.empty()) return;  // tried again on the next poll
    const std::vector<EventView> views = BuildEvents(tree, raw);
    if (!events_seeded_) {
        known_.clear();
        for (auto it = views.rbegin(); it != views.rend(); ++it) known_.push_back({it->controller, it->id, it->title});
        events_seeded_ = true;
    } else {
        const std::vector<std::string> changes =
            EventChanges(known_, views, [this](std::uintptr_t c, const std::string& id) { return hub_.TakeChoice(c, id); });
        if (!changes.empty()) {
            const std::optional<std::int32_t> date = Date();
            for (const std::string& m : changes) hub_.Publish(NotificationKind::Event, date, m);
        }
    }
    last_count_ = count;
    next_event_check_ = h_.now_ms() + kEventCheckMs;
}

void NotificationSources::ResetErrorLog() {
    log_path_.clear();
    next_locate_ = 0;
    tail_.Reset();
}

std::size_t NotificationSources::PollErrorLog() {
    if (!h_.locate_error_log || !h_.read_log_chunk || !h_.now_ms) return 0;
    if (log_path_.empty()) {
        if (h_.now_ms() < next_locate_) return 0;
        const ErrorLogLocation where = h_.locate_error_log();
        if (!where.error.empty() || where.path.empty()) {
            next_locate_ = h_.now_ms() + kLocateRetryMs;
            return 0;
        }
        log_path_ = where.path;
        tail_.Reset();
    }
    LineTail::Chunk chunk;
    if (const std::string error = h_.read_log_chunk(log_path_, tail_.offset(), kErrorLogChunk, chunk); !error.empty()) {
        // No file yet (the game moves error.log aside at start): start from its beginning once it appears.
        if (chunk.missing && !tail_.started()) tail_.Feed({});
        return 0;
    }
    const LineTail::Lines got = tail_.Feed(chunk);
    if (got.restarted || !got.lines.empty()) {
        const std::optional<std::int32_t> date = Date();
        if (got.restarted) hub_.Publish(NotificationKind::ErrorLog, date, ",\"restarted\":true");
        for (const std::string& line : got.lines) hub_.Publish(NotificationKind::ErrorLog, date, ",\"line\":" + Quote(line));
    }
    return chunk.data.size();
}

void NotificationSources::Start() {
    constexpr int kIdleWaitMs = 1000;
    const auto sleep = [this](int ms) { h_.sleep_ms(ms); };
    std::thread([this, sleep] {
        for (;;) {
            if (!hub_.WaitFor(Bit(NotificationKind::State) | Bit(NotificationKind::Date), kIdleWaitMs)) {
                ResetState();
                continue;
            }
            PollState();
            sleep(h_.poll_interval_ms);
        }
    }).detach();
    std::thread([this, sleep] {
        for (;;) {
            if (!hub_.WaitFor(Bit(NotificationKind::Event), kIdleWaitMs)) {
                ResetEvents();
                continue;
            }
            PollEvents();
            sleep(h_.poll_interval_ms);
        }
    }).detach();
    std::thread([this, sleep] {
        for (;;) {
            if (!hub_.WaitFor(Bit(NotificationKind::ErrorLog), kIdleWaitMs)) {
                ResetErrorLog();
                continue;
            }
            if (PollErrorLog() < kErrorLogChunk) sleep(kErrorLogPollMs);
        }
    }).detach();
}

}  // namespace tongs
