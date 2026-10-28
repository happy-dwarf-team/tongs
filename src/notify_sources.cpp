#include "notify_sources.hpp"

#include <algorithm>
#include <charconv>
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
    const bool new_save =
        before.in_game && after.in_game && (before.session != after.session || before.load != after.load);
    if (before.in_game && (!after.in_game || new_save)) {
        std::string m = Change("left_game");
        if (!after.in_game) m += ",\"reason\":" + Quote(after.reason ? after.reason : "");
        out.push_back(m);
    }
    if (after.in_game && (!before.in_game || new_save)) {
        std::string m = Change("entered_game") + InGameFields(after);
        if (after.load != 0) m += ",\"session\":" + std::to_string(after.load);
        out.push_back(m);
        return out;
    }
    if (!before.in_game || !after.in_game) return out;
    if (before.paused != after.paused) out.push_back(Change(after.paused ? "paused" : "resumed") + InGameFields(after));
    if (before.speed != after.speed) {
        out.push_back(Change("speed") + InGameFields(after) + ",\"previous_speed\":" + std::to_string(before.speed + 1));
    }
    return out;
}

EventDiff DiffEvents(std::vector<KnownEvent>& known, const std::vector<EventView>& now) {
    EventDiff out;
    const auto open_now = [&now](const KnownEvent& k) {
        return std::any_of(now.begin(), now.end(), [&k](const EventView& v) { return v.controller == k.controller && v.id == k.id; });
    };
    for (const KnownEvent& k : known) {
        if (!open_now(k)) out.closed.push_back(k);
    }
    std::vector<KnownEvent> next;
    // `now` is topmost first; report openings oldest first.
    for (auto it = now.rbegin(); it != now.rend(); ++it) {
        const EventView& v = *it;
        const auto same = [&v](const KnownEvent& k) { return k.controller == v.controller && k.id == v.id; };
        const auto old = std::find_if(known.begin(), known.end(), same);
        if (old == known.end()) {
            out.opened.push_back(Change("opened") + ",\"id\":" + Quote(v.id) + ",\"title\":" + Quote(v.title) + OptionsJson(v));
            next.push_back({v.controller, v.id, v.title, v.options});
        } else {
            next.push_back({v.controller, v.id, v.title.empty() ? old->title : v.title,
                            v.options.empty() ? old->options : v.options});
        }
    }
    known = std::move(next);
    return out;
}

std::string ClosedMembers(const KnownEvent& event, std::string_view chosen) {
    return Change("closed") + ",\"id\":" + Quote(event.id) + ",\"title\":" + Quote(event.title) + ",\"chosen\":" +
           std::string(chosen);
}

std::optional<LoggedChoice> ParseChoiceLine(std::string_view line) {
    constexpr std::string_view kSource = "[eventwindow.cpp:";
    constexpr std::string_view kAuto = "Auto-selected event option ";
    constexpr std::string_view kSelected = "Selected event option ";
    constexpr std::string_view kFor = " for event ";
    if (!line.starts_with(kSource)) return std::nullopt;
    const std::size_t close = line.find("]: ");
    if (close == std::string_view::npos) return std::nullopt;
    line.remove_prefix(close + 3);
    LoggedChoice out;
    if (line.starts_with(kAuto)) {
        out.automatic = true;
        line.remove_prefix(kAuto.size());
    } else if (line.starts_with(kSelected)) {
        line.remove_prefix(kSelected.size());
    } else {
        return std::nullopt;
    }
    const auto [ptr, ec] = std::from_chars(line.data(), line.data() + line.size(), out.option_index);
    if (ec != std::errc{} || out.option_index < 0) return std::nullopt;
    line.remove_prefix(static_cast<std::size_t>(ptr - line.data()));
    if (!line.starts_with(kFor)) return std::nullopt;
    line.remove_prefix(kFor.size());
    while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.remove_suffix(1);
    if (line.empty()) return std::nullopt;
    out.id = std::string(line);
    return out;
}

std::string LoggedChoiceJson(const KnownEvent& event, const LoggedChoice& choice) {
    const auto o = std::find_if(event.options.begin(), event.options.end(),
                                [&choice](const EventOption& o) { return o.index == choice.option_index; });
    const bool shown = o != event.options.end();
    return "{\"number\":" + (shown ? std::to_string(o->number) : std::string("null")) +
           ",\"text\":" + (shown ? Quote(o->text) : std::string("null")) +
           ",\"option_index\":" + std::to_string(choice.option_index) +
           ",\"option_key\":" + (shown ? Quote(o->key) : std::string("null")) +
           ",\"by\":" + (choice.automatic ? "\"auto\"" : "\"player\"") + "}";
}

void ChoiceMatcher::Closed(std::vector<std::pair<KnownEvent, std::optional<std::string>>> events, std::uint64_t now_ms) {
    ++batches_;
    for (auto& [event, tongs] : events) pending_.push_back({std::move(event), std::move(tongs), batches_, now_ms + kWaitMs});
}

void ChoiceMatcher::Logged(LoggedChoice choice, std::uint64_t now_ms) { lines_.push_back({std::move(choice), now_ms}); }

std::vector<std::string> ChoiceMatcher::Due(std::uint64_t now_ms, bool wait) {
    std::vector<std::string> out;
    for (Pending& p : pending_) {
        if (p.tongs && !p.published) {
            out.push_back(ClosedMembers(p.event, *p.tongs));
            p.published = true;
        }
    }
    while (!pending_.empty()) {
        std::size_t n = 0;
        while (n < pending_.size() && pending_[n].batch == pending_.front().batch) ++n;
        // For each event of the oldest batch in order, the oldest free line with its id.
        const std::size_t none = lines_.size();
        std::vector<std::size_t> line_of(n, none);
        std::vector<bool> taken(lines_.size(), false);
        bool complete = true;
        for (std::size_t i = 0; i < n; ++i) {
            for (std::size_t l = 0; l < lines_.size(); ++l) {
                if (!taken[l] && lines_[l].choice.id == pending_[i].event.id) {
                    line_of[i] = l;
                    taken[l] = true;
                    break;
                }
            }
            complete = complete && line_of[i] != none;
        }
        if (!complete && wait && now_ms < pending_.front().deadline) break;
        for (std::size_t i = 0; i < n; ++i) {
            const Pending& p = pending_[i];
            if (p.published) continue;
            bool named = line_of[i] != none;
            for (std::size_t j = 0; named && j < n; ++j) {
                if (j != i && pending_[j].event.id == p.event.id &&
                    (line_of[j] == none || !(lines_[line_of[j]].choice == lines_[line_of[i]].choice))) {
                    named = false;
                }
            }
            out.push_back(ClosedMembers(p.event, named ? LoggedChoiceJson(p.event, lines_[line_of[i]].choice) : "null"));
        }
        for (std::size_t l = lines_.size(); l-- > 0;) {
            if (taken[l]) lines_.erase(lines_.begin() + static_cast<std::ptrdiff_t>(l));
        }
        pending_.erase(pending_.begin(), pending_.begin() + static_cast<std::ptrdiff_t>(n));
    }
    while (!lines_.empty() && lines_.front().at + kKeepLineMs <= now_ms) lines_.pop_front();
    return out;
}

void ChoiceMatcher::Reset() {
    pending_.clear();
    lines_.clear();
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
    matcher_.Reset();
    game_log_path_.clear();
    next_game_log_ = 0;
    game_tail_.Reset();
}

void NotificationSources::PollEvents() {
    PollEventList();
    // game.log every 100 ms, and every poll while a closed event waits for its line.
    PublishClosed(FollowGameLog(matcher_.waiting()));
}

bool NotificationSources::FollowGameLog(bool force) {
    if (!h_.locate_game_log || !h_.read_log_chunk || !h_.now_ms) return false;
    const std::uint64_t now = h_.now_ms();
    if (game_log_path_.empty()) {
        if (now < next_game_log_) return false;
        const ErrorLogLocation where = h_.locate_game_log();
        if (!where.error.empty() || where.path.empty()) {
            next_game_log_ = now + kLocateRetryMs;
            return false;
        }
        game_log_path_ = where.path;
        game_tail_.Reset();
    } else if (!force && now < next_game_log_) {
        return true;
    }
    next_game_log_ = now + kErrorLogPollMs;
    for (;;) {
        LineTail::Chunk chunk;
        if (!h_.read_log_chunk(game_log_path_, game_tail_.offset(), kErrorLogChunk, chunk).empty()) {
            // The game writes game.log anew at start: follow it from its beginning once it appears.
            if (chunk.missing && !game_tail_.started()) game_tail_.Feed({});
            return true;
        }
        for (const std::string& line : game_tail_.Feed(chunk).lines) {
            if (std::optional<LoggedChoice> choice = ParseChoiceLine(line)) matcher_.Logged(std::move(*choice), now);
        }
        if (chunk.data.size() < kErrorLogChunk) return true;
    }
}

void NotificationSources::PublishClosed(bool wait, bool in_session) {
    const std::vector<std::string> due = matcher_.Due(h_.now_ms ? h_.now_ms() : 0, wait);
    if (due.empty()) return;
    const std::optional<std::int32_t> date = in_session ? Date() : std::nullopt;
    for (const std::string& m : due) hub_.Publish(NotificationKind::Event, date, m);
}

void NotificationSources::PollEventList() {
    if (!h_.dispatcher || !h_.read_ui || !h_.read_events || !h_.count_event_windows || !h_.now_ms) return;
    const int count = h_.count_event_windows();
    const std::uint64_t now = h_.now_ms();
    if (count < 0) {
        // No session: whatever was open is gone with it.
        if (events_seeded_ && !known_.empty()) {
            std::vector<std::pair<KnownEvent, std::optional<std::string>>> closed;
            for (KnownEvent& k : known_) {
                std::optional<std::string> tongs = hub_.TakeChoice(k.controller, k.id);
                closed.emplace_back(std::move(k), std::move(tongs));
            }
            matcher_.Closed(std::move(closed), now);
        }
        // Without a session no line is coming, nor a date.
        PublishClosed(false, false);
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
        for (auto it = views.rbegin(); it != views.rend(); ++it) {
            known_.push_back({it->controller, it->id, it->title, it->options});
        }
        events_seeded_ = true;
    } else {
        EventDiff diff = DiffEvents(known_, views);
        if (!diff.closed.empty()) {
            std::vector<std::pair<KnownEvent, std::optional<std::string>>> closed;
            for (KnownEvent& k : diff.closed) {
                std::optional<std::string> tongs = hub_.TakeChoice(k.controller, k.id);
                closed.emplace_back(std::move(k), std::move(tongs));
            }
            matcher_.Closed(std::move(closed), h_.now_ms());
            // Closed before opened, when the lines are already there.
            PublishClosed(FollowGameLog(true));
        }
        if (!diff.opened.empty()) {
            const std::optional<std::int32_t> date = Date();
            for (const std::string& m : diff.opened) hub_.Publish(NotificationKind::Event, date, m);
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
