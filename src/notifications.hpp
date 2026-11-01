#pragma once

#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// Notifications (docs/design.md, "Notifications"): messages Tongs sends to a
// subscribed connection on its own. Each subscription has a bounded queue;
// whoever publishes never waits for the reader, and the oldest notifications
// are dropped when the queue is full. Nothing here touches the game or the
// pipe, so the tests drive it directly.

enum class NotificationKind : unsigned {
    State = 1,
    Date = 2,
    Event = 4,
    ErrorLog = 8,
    Activity = 16,
};
inline constexpr unsigned kAllNotificationKinds = 31;

constexpr unsigned Bit(NotificationKind kind) { return static_cast<unsigned>(kind); }
const char* KindName(NotificationKind kind);

// The step of `date` notifications.
enum class DateUnit { Day, Month, Year };
const char* DateUnitName(DateUnit unit);

// True when `now` is in a later day / month / year than `previous`.
bool DateCrosses(std::int32_t previous, std::int32_t now, DateUnit unit);

// subscribe KINDS [date=day|month|year]: KINDS is `all` or kind names joined by
// commas (or given as separate words).
struct SubscribeRequest {
    unsigned kinds = 0;
    DateUnit date_unit = DateUnit::Month;
};
// Returns an error text, or an empty string.
std::string ParseSubscribe(std::string_view argument, SubscribeRequest& out);

inline constexpr std::size_t kNotificationQueue = 1000;

// One subscribed connection. Push() may be called from any thread; Take() is
// called by the connection's own pipe thread.
class Subscription {
public:
    Subscription(SubscribeRequest request, std::uint64_t start_ms, std::size_t capacity = kNotificationQueue);

    bool Wants(NotificationKind kind) const { return (request_.kinds & Bit(kind)) != 0; }
    const SubscribeRequest& request() const { return request_; }
    std::uint64_t start_ms() const { return start_ms_; }

    // Queues a notification; `members` are its own JSON members, each starting
    // with a comma. With a full queue the oldest is dropped. Never blocks for
    // the reader.
    void Push(NotificationKind kind, std::optional<std::int32_t> date_raw, std::uint64_t now_ms, std::string members);

    // Waits up to `wait_ms` for notifications and returns them as lines
    // ("{...}\n"), at most `max_lines`, numbered from seq 1. After a drop, an
    // overflow notification with the number of dropped ones comes first.
    std::string Take(int wait_ms, std::size_t max_lines = 256);

    struct Counts {
        std::uint64_t sent = 0;     // lines returned by Take, overflow notices included
        std::uint64_t dropped = 0;  // all dropped so far
        std::size_t pending = 0;    // still queued
    };
    Counts counts() const;
    // Empties the queue and returns how many were discarded (for `unsubscribe`).
    std::size_t Discard();

private:
    struct Item {
        NotificationKind kind;
        std::optional<std::int32_t> date_raw;
        std::uint64_t wall_ms;
        std::string members;
    };

    SubscribeRequest request_;
    std::uint64_t start_ms_;
    std::size_t capacity_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<Item> queue_;
    std::uint64_t seq_ = 0;
    std::uint64_t dropped_unreported_ = 0;
    std::optional<std::int32_t> drop_date_;  // of the last dropped one
    std::uint64_t drop_wall_ms_ = 0;
    std::uint64_t dropped_total_ = 0;
};

// Every subscription of the process. Sources ask Wants() first, so nothing is
// read or formatted while nobody subscribes to a kind.
class NotificationHub {
public:
    explicit NotificationHub(std::function<std::uint64_t()> now_ms);

    std::shared_ptr<Subscription> Subscribe(const SubscribeRequest& request);
    void Unsubscribe(const std::shared_ptr<Subscription>& subscription);

    // Kinds any subscription wants.
    unsigned wanted() const;
    bool Wants(NotificationKind kind) const { return (wanted() & Bit(kind)) != 0; }
    int subscriptions() const;

    void Publish(NotificationKind kind, std::optional<std::int32_t> date_raw, const std::string& members);
    // A `date` notification for each subscription whose step the change crosses.
    void PublishDate(std::int32_t previous, std::int32_t now);

    // For source threads: waits until a subscription wants one of `kinds`, up
    // to `timeout_ms`; returns whether one does.
    bool WaitFor(unsigned kinds, int timeout_ms);

    // Event choices made through Tongs (`event choose`, `run_until
    // events=first`), so the `closed` notification can name the option. Keyed
    // by the controller and the event id; the newest 32 are kept.
    void NoteChoice(std::uintptr_t controller, const std::string& event_id, std::string choice_json);
    std::optional<std::string> TakeChoice(std::uintptr_t controller, const std::string& event_id);

private:
    std::function<std::uint64_t()> now_ms_;
    mutable std::mutex mutex_;
    std::condition_variable changed_;
    std::vector<std::shared_ptr<Subscription>> subscriptions_;
    unsigned wanted_ = 0;
    struct Choice {
        std::uintptr_t controller;
        std::string event_id;
        std::string json;
    };
    std::deque<Choice> choices_;
};

// Follows the end of a growing text file (error.log, game.log), from chunks the caller
// reads at offset(). The first chunk only sets the starting point, so only
// lines written after it are reported.
class LineTail {
public:
    struct Chunk {
        std::uint64_t size = 0;     // the file's size when it was read
        std::uint64_t file_id = 0;  // tells a new file under the same name apart
        std::string data;           // bytes from offset()
        bool missing = false;       // there is no such file (yet)
    };
    struct Lines {
        std::vector<std::string> lines;  // UTF-8, without the line break; long lines are cut
        bool restarted = false;          // the file was replaced or truncated; reading starts again at 0
    };

    bool started() const { return started_; }
    std::uint64_t offset() const { return offset_; }
    void Reset();
    Lines Feed(const Chunk& chunk);

    static constexpr std::size_t kMaxLine = 4096;

private:
    bool started_ = false;
    std::uint64_t offset_ = 0;
    std::uint64_t file_id_ = 0;
    std::string partial_;
};

// Where the game writes a log, e.g. error.log (plugin: game_files.cpp).
struct ErrorLogLocation {
    std::string path;    // UTF-8, absolute
    std::string source;  // how it was found: "physfs_write_dir" (PHYSFS_getWriteDir)
    std::string error;   // why there is no path (yet)
};

// A log line as UTF-8: kept when it is valid UTF-8, otherwise read as Windows-1252.
std::string LogTextToUtf8(std::string_view text);

// A line that Anvil's anvil-log-throttle plugin writes in place of repeats of a
// game line (anvil#172), after the game's "[<file>:<line>]: " prefix:
//   notice: "[anvil-log-throttle] logged <n> times; further repeats are counted and reported every <s> s: <text>"
//   report: "[anvil-log-throttle] repeated <n> more times in the last <s> s: <text>"
// The notice stands in for the first counted line, so the lines of a message are
// its ordinary lines plus the counts of its reports.
struct ThrottledLine {
    bool report = false;        // a report; else a notice
    std::uint64_t count = 0;    // report: lines counted since the last report; notice: lines written before
    std::uint64_t seconds = 0;  // report: how long it counted; notice: how often it reports
    std::string text;           // the game's line as it wrote it each time: the prefix and the message
};

std::optional<ThrottledLine> ParseThrottledLine(std::string_view line);

// The fields of an error_log notification for a line: `line`, and for a line of
// anvil-log-throttle also `throttle` ("notice" or "report"), the count and `text`.
std::string ErrorLogFields(std::string_view line);

}  // namespace tongs
