#include "journal.hpp"

#include "protocol.hpp"

#include <algorithm>
#include <array>
#include <system_error>
#include <vector>

namespace tongs {
namespace {

std::string_view FirstWord(std::string_view text, std::size_t& rest) {
    const std::size_t start = text.find_first_not_of(" \t\r");
    if (start == std::string_view::npos) {
        rest = text.size();
        return {};
    }
    std::size_t end = text.find_first_of(" \t\r", start);
    if (end == std::string_view::npos) end = text.size();
    rest = end;
    return text.substr(start, end - start);
}

// The JSON string value of the first `"key":"` in `json`, still escaped; empty view when none.
bool StringField(std::string_view json, std::string_view key, std::string_view& out) {
    const std::string needle = "\"" + std::string(key) + "\":\"";
    const std::size_t at = json.find(needle);
    if (at == std::string_view::npos) return false;
    const std::size_t start = at + needle.size();
    std::size_t end = start;
    while (end < json.size() && json[end] != '"') end += json[end] == '\\' ? 2 : 1;
    if (end > json.size()) return false;
    out = json.substr(start, end - start);
    return true;
}

bool NumberField(std::string_view json, std::string_view key, std::string_view& out) {
    const std::string needle = "\"" + std::string(key) + "\":";
    const std::size_t at = json.find(needle);
    if (at == std::string_view::npos) return false;
    const std::size_t start = at + needle.size();
    std::size_t end = start;
    while (end < json.size() && json[end] >= '0' && json[end] <= '9') ++end;
    if (end == start) return false;
    out = json.substr(start, end - start);
    return true;
}

}  // namespace

const char* JournalKind(std::string_view request) {
    std::size_t rest = 0;
    const std::string_view word = FirstWord(request, rest);
    std::size_t ignored = 0;
    const std::string_view next = FirstWord(request.substr(rest), ignored);
    static constexpr std::array kInputs = {"key", "text", "move", "click", "scroll", "release",
                                           "pause", "resume", "speed", "run_until", "unit"};
    for (const char* name : kInputs) {
        if (word == name) return "input";
    }
    if ((word == "ui" && next == "click") || (word == "event" && next == "choose")) return "input";
    if ((word == "province" && (next == "open" || next == "goto")) || word == "map_mode") return "client_call";
    if (word == "console" || word == "save") return "game_call";
    if (word == "subscribe" || word == "unsubscribe") return "stream";
    return "read";
}

std::string JournalLine(std::uint64_t seq, const JournalEntry& e) {
    std::string out = "{\"seq\":" + std::to_string(seq) + ",\"t\":" + std::to_string(e.unix_ms) +
                      ",\"client\":" + std::to_string(e.client) + ",\"kind\":\"" + JournalKind(e.request) +
                      "\",\"request\":\"" + JsonEscape(e.request) + "\",\"ms\":" + std::to_string(e.ms);
    const bool ok = e.reply.starts_with("{\"ok\":true");
    out += std::string(",\"ok\":") + (ok ? "true" : "false");
    std::string_view value;
    out += ",\"error\":" + (!ok && StringField(e.reply, "error", value) ? "\"" + std::string(value) + "\"" : std::string("null"));
    out += ",\"date\":" + (StringField(e.reply, "date", value) ? "\"" + std::string(value) + "\"" : std::string("null"));
    out += ",\"session\":" + (NumberField(e.reply, "session", value) ? std::string(value) : std::string("null"));
    return out + "}";
}

std::string JournalHeader(std::string_view version, std::uint32_t pid, std::uint64_t process_start_ms,
                          std::uint64_t opened_unix_ms) {
    return "{\"journal\":1,\"tongs\":\"" + JsonEscape(version) + "\",\"pid\":" + std::to_string(pid) +
           ",\"process_start_ms\":" + std::to_string(process_start_ms) + ",\"opened\":" + std::to_string(opened_unix_ms) +
           "}";
}

std::string JournalFileName(std::uint32_t pid, std::uint64_t process_start_ms) {
    return "journal-" + std::to_string(process_start_ms) + "-" + std::to_string(pid) + ".jsonl";
}

void PruneJournals(const std::filesystem::path& folder, int keep) {
    std::error_code ec;
    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        const std::wstring name = entry.path().filename().wstring();
        if (entry.is_regular_file(ec) && name.starts_with(L"journal-") && entry.path().extension() == L".jsonl") {
            files.push_back(entry.path());
        }
    }
    if (static_cast<int>(files.size()) <= keep) return;
    // The names hold the start time in milliseconds, all of the same length for centuries.
    std::sort(files.begin(), files.end());
    for (std::size_t i = 0; i + keep < files.size(); ++i) std::filesystem::remove(files[i], ec);
}

std::string Journal::Open(const std::filesystem::path& folder, std::string_view version, std::uint32_t pid,
                          std::uint64_t process_start_ms, std::uint64_t now_unix_ms) {
    std::lock_guard lock(mutex_);
    std::error_code ec;
    std::filesystem::create_directories(folder, ec);
    if (ec) return "cannot create the journal folder: " + ec.message();
    path_ = folder / JournalFileName(pid, process_start_ms);
    out_.open(path_, std::ios::binary | std::ios::app);
    if (!out_) return "cannot open the journal file";
    const std::string header = JournalHeader(version, pid, process_start_ms, now_unix_ms) + "\n";
    out_ << header;
    out_.flush();
    bytes_ = header.size();
    open_ = static_cast<bool>(out_);
    PruneJournals(folder, kJournalFiles);
    return open_ ? std::string() : "cannot write the journal file";
}

void Journal::Record(const JournalEntry& entry) {
    std::lock_guard lock(mutex_);
    if (!open_ || truncated_) return;
    ++seq_;
    std::string line = JournalLine(seq_, entry) + "\n";
    if (bytes_ + line.size() > kJournalMaxBytes) {
        truncated_ = true;
        line = "{\"truncated\":true,\"seq\":" + std::to_string(seq_) + "}\n";
    }
    out_ << line;
    out_.flush();
    bytes_ += line.size();
}

std::string Journal::Path() const {
    std::lock_guard lock(mutex_);
    const std::u8string u8 = path_.u8string();
    return std::string(reinterpret_cast<const char*>(u8.data()), u8.size());
}

std::uint64_t Journal::Lines() const {
    std::lock_guard lock(mutex_);
    return seq_;
}

bool Journal::Truncated() const {
    std::lock_guard lock(mutex_);
    return truncated_;
}

bool Journal::IsOpen() const {
    std::lock_guard lock(mutex_);
    return open_;
}

}  // namespace tongs
