#include "notifications.hpp"

#include "game_date.hpp"
#include "protocol.hpp"
#include "ui_tree.hpp"

#include <algorithm>
#include <chrono>
#include <utility>

namespace tongs {
namespace {

constexpr std::size_t kMaxChoices = 32;

struct KindEntry {
    NotificationKind kind;
    const char* name;
};
constexpr KindEntry kKinds[] = {
    {NotificationKind::State, "state"},       {NotificationKind::Date, "date"},
    {NotificationKind::Event, "event"},       {NotificationKind::ErrorLog, "error_log"},
    {NotificationKind::Activity, "activity"},
};

std::string DateJson(std::optional<std::int32_t> raw) {
    return raw ? "\"" + FormatGameDate(DecodeGameDate(*raw)) + "\"" : std::string("null");
}

std::string Line(const char* kind, std::uint64_t seq, std::optional<std::int32_t> date_raw, std::uint64_t wall_ms,
                 const std::string& members) {
    return std::string("{\"notification\":\"") + kind + "\",\"seq\":" + std::to_string(seq) +
           ",\"date\":" + DateJson(date_raw) + ",\"wall_ms\":" + std::to_string(wall_ms) + members + "}\n";
}

bool ValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        std::size_t extra = 0;
        char32_t cp = 0;
        if (c < 0x80) {
            ++i;
            continue;
        } else if ((c & 0xE0) == 0xC0) {
            extra = 1;
            cp = c & 0x1F;
        } else if ((c & 0xF0) == 0xE0) {
            extra = 2;
            cp = c & 0x0F;
        } else if ((c & 0xF8) == 0xF0) {
            extra = 3;
            cp = c & 0x07;
        } else {
            return false;
        }
        if (text.size() - i <= extra) return false;
        for (std::size_t k = 1; k <= extra; ++k) {
            const auto d = static_cast<unsigned char>(text[i + k]);
            if ((d & 0xC0) != 0x80) return false;
            cp = (cp << 6) | (d & 0x3F);
        }
        // Overlong forms, surrogates and values past U+10FFFF.
        if ((extra == 1 && cp < 0x80) || (extra == 2 && cp < 0x800) || (extra == 3 && cp < 0x10000) ||
            (cp >= 0xD800 && cp <= 0xDFFF) || cp > 0x10FFFF) {
            return false;
        }
        i += extra + 1;
    }
    return true;
}

}  // namespace

const char* KindName(NotificationKind kind) {
    for (const KindEntry& e : kKinds) {
        if (e.kind == kind) return e.name;
    }
    return "unknown";
}

const char* DateUnitName(DateUnit unit) {
    switch (unit) {
        case DateUnit::Day: return "day";
        case DateUnit::Month: return "month";
        case DateUnit::Year: return "year";
    }
    return "month";
}

bool DateCrosses(std::int32_t previous, std::int32_t now, DateUnit unit) {
    if (now <= previous) return false;
    const GameDate a = DecodeGameDate(previous);
    const GameDate b = DecodeGameDate(now);
    switch (unit) {
        case DateUnit::Day: return a.year != b.year || a.month != b.month || a.day != b.day;
        case DateUnit::Month: return a.year != b.year || a.month != b.month;
        case DateUnit::Year: return a.year != b.year;
    }
    return false;
}

std::string ParseSubscribe(std::string_view argument, SubscribeRequest& out) {
    out = {};
    bool date_given = false;
    std::size_t i = 0;
    while (i < argument.size()) {
        while (i < argument.size() && (argument[i] == ' ' || argument[i] == '\t')) ++i;
        std::size_t end = i;
        while (end < argument.size() && argument[end] != ' ' && argument[end] != '\t') ++end;
        const std::string_view word = argument.substr(i, end - i);
        i = end;
        if (word.empty()) continue;
        if (word.starts_with("date=")) {
            const std::string_view value = word.substr(5);
            if (date_given) return "date= given twice";
            date_given = true;
            if (value == "day") {
                out.date_unit = DateUnit::Day;
            } else if (value == "month") {
                out.date_unit = DateUnit::Month;
            } else if (value == "year") {
                out.date_unit = DateUnit::Year;
            } else {
                return "date= takes day, month or year";
            }
            continue;
        }
        for (std::size_t start = 0; start <= word.size();) {
            const std::size_t comma = std::min(word.find(',', start), word.size());
            const std::string_view name = word.substr(start, comma - start);
            start = comma + 1;
            if (name.empty()) {
                if (comma == word.size()) break;
                return "empty kind in " + std::string(word.substr(0, 64));
            }
            if (name == "all") {
                out.kinds |= kAllNotificationKinds;
                continue;
            }
            bool known = false;
            for (const KindEntry& e : kKinds) {
                if (name == e.name) {
                    out.kinds |= Bit(e.kind);
                    known = true;
                }
            }
            if (!known) {
                return "unknown kind: " + std::string(name.substr(0, 32)) +
                       " (state, date, event, error_log, activity or all)";
            }
        }
    }
    if (out.kinds == 0) return "usage: subscribe KINDS [date=day|month|year], KINDS from state, date, event, error_log, activity or all";
    if (date_given && (out.kinds & Bit(NotificationKind::Date)) == 0) return "date= needs the date kind";
    return {};
}

Subscription::Subscription(SubscribeRequest request, std::uint64_t start_ms, std::size_t capacity)
    : request_(request), start_ms_(start_ms), capacity_(std::max<std::size_t>(capacity, 1)) {}

void Subscription::Push(NotificationKind kind, std::optional<std::int32_t> date_raw, std::uint64_t now_ms,
                        std::string members) {
    const std::uint64_t wall = now_ms >= start_ms_ ? now_ms - start_ms_ : 0;
    {
        std::lock_guard lock(mutex_);
        if (queue_.size() >= capacity_) {
            drop_date_ = queue_.front().date_raw;
            drop_wall_ms_ = queue_.front().wall_ms;
            queue_.pop_front();
            ++dropped_unreported_;
            ++dropped_total_;
        }
        queue_.push_back({kind, date_raw, wall, std::move(members)});
    }
    ready_.notify_one();
}

std::string Subscription::Take(int wait_ms, std::size_t max_lines) {
    std::unique_lock lock(mutex_);
    if (queue_.empty() && dropped_unreported_ == 0 && wait_ms > 0) {
        ready_.wait_for(lock, std::chrono::milliseconds(wait_ms),
                        [this] { return !queue_.empty() || dropped_unreported_ != 0; });
    }
    std::string out;
    std::size_t lines = 0;
    if (dropped_unreported_ != 0 && lines < max_lines) {
        out += Line("overflow", ++seq_, drop_date_, drop_wall_ms_, ",\"dropped\":" + std::to_string(dropped_unreported_));
        dropped_unreported_ = 0;
        ++lines;
    }
    while (!queue_.empty() && lines < max_lines) {
        const Item& item = queue_.front();
        out += Line(KindName(item.kind), ++seq_, item.date_raw, item.wall_ms, item.members);
        queue_.pop_front();
        ++lines;
    }
    return out;
}

Subscription::Counts Subscription::counts() const {
    std::lock_guard lock(mutex_);
    return {seq_, dropped_total_, queue_.size()};
}

std::size_t Subscription::Discard() {
    std::lock_guard lock(mutex_);
    const std::size_t count = queue_.size();
    queue_.clear();
    dropped_unreported_ = 0;
    return count;
}

NotificationHub::NotificationHub(std::function<std::uint64_t()> now_ms) : now_ms_(std::move(now_ms)) {}

std::shared_ptr<Subscription> NotificationHub::Subscribe(const SubscribeRequest& request) {
    auto subscription = std::make_shared<Subscription>(request, now_ms_());
    {
        std::lock_guard lock(mutex_);
        subscriptions_.push_back(subscription);
        wanted_ |= request.kinds;
    }
    changed_.notify_all();
    return subscription;
}

void NotificationHub::Unsubscribe(const std::shared_ptr<Subscription>& subscription) {
    {
        std::lock_guard lock(mutex_);
        std::erase(subscriptions_, subscription);
        wanted_ = 0;
        for (const auto& s : subscriptions_) wanted_ |= s->request().kinds;
    }
    changed_.notify_all();
}

unsigned NotificationHub::wanted() const {
    std::lock_guard lock(mutex_);
    return wanted_;
}

int NotificationHub::subscriptions() const {
    std::lock_guard lock(mutex_);
    return static_cast<int>(subscriptions_.size());
}

void NotificationHub::Publish(NotificationKind kind, std::optional<std::int32_t> date_raw, const std::string& members) {
    const std::uint64_t now = now_ms_();
    std::lock_guard lock(mutex_);
    for (const auto& s : subscriptions_) {
        if (s->Wants(kind)) s->Push(kind, date_raw, now, members);
    }
}

void NotificationHub::PublishDate(std::int32_t previous, std::int32_t now_raw) {
    const std::uint64_t now = now_ms_();
    const std::string previous_text = FormatGameDate(DecodeGameDate(previous));
    std::lock_guard lock(mutex_);
    for (const auto& s : subscriptions_) {
        const DateUnit unit = s->request().date_unit;
        if (!s->Wants(NotificationKind::Date) || !DateCrosses(previous, now_raw, unit)) continue;
        s->Push(NotificationKind::Date, now_raw, now,
                std::string(",\"unit\":\"") + DateUnitName(unit) + "\",\"previous\":\"" + previous_text + "\"");
    }
}

bool NotificationHub::WaitFor(unsigned kinds, int timeout_ms) {
    std::unique_lock lock(mutex_);
    return changed_.wait_for(lock, std::chrono::milliseconds(timeout_ms), [this, kinds] { return (wanted_ & kinds) != 0; });
}

void NotificationHub::NoteChoice(std::uintptr_t controller, const std::string& event_id, std::string choice_json) {
    std::lock_guard lock(mutex_);
    choices_.push_back({controller, event_id, std::move(choice_json)});
    while (choices_.size() > kMaxChoices) choices_.pop_front();
}

std::optional<std::string> NotificationHub::TakeChoice(std::uintptr_t controller, const std::string& event_id) {
    std::lock_guard lock(mutex_);
    for (auto it = choices_.rbegin(); it != choices_.rend(); ++it) {
        if (it->controller == controller && it->event_id == event_id) {
            std::string json = std::move(it->json);
            choices_.erase(std::next(it).base());
            return json;
        }
    }
    return std::nullopt;
}

void LineTail::Reset() { *this = LineTail(); }

LineTail::Lines LineTail::Feed(const Chunk& chunk) {
    Lines out;
    if (!started_) {
        started_ = true;
        offset_ = chunk.size;
        file_id_ = chunk.file_id;
        return out;
    }
    if (chunk.file_id != file_id_ || chunk.size < offset_) {
        // A new file, or the old one cut short: what was read belongs to neither.
        offset_ = 0;
        file_id_ = chunk.file_id;
        partial_.clear();
        out.restarted = true;
        return out;
    }
    offset_ += chunk.data.size();
    for (const char c : chunk.data) {
        if (c == '\n') {
            if (!partial_.empty() && partial_.back() == '\r') partial_.pop_back();
            out.lines.push_back(LogTextToUtf8(partial_));
            partial_.clear();
        } else if (partial_.size() < kMaxLine) {
            partial_ += c;  // the rest of a longer line is dropped
        }
    }
    return out;
}

std::string LogTextToUtf8(std::string_view text) {
    if (ValidUtf8(text)) return std::string(text);
    // Windows-1252, as GameTextToUtf8 reads it, but without dropping colour codes.
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if (c == 0xA7) {
            out += "\xC2\xA7";
            continue;
        }
        out += GameTextToUtf8(std::string_view(&ch, 1));
    }
    return out;
}

}  // namespace tongs
