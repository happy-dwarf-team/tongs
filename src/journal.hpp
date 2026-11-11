// The command journal (docs/commands.md, "Journal"): every request a client sends
// on the pipe, with the outcome, one JSON line each, in a file of its own per game
// process under anvil/data/tongs/journal/. It lives outside the game: nothing of it
// is in game memory, a save or the checksums multiplayer compares.
#pragma once

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

namespace tongs {

inline constexpr int kJournalFiles = 20;                          // the newest files kept
inline constexpr std::uint64_t kJournalMaxBytes = 64ull << 20;    // one file stops growing here

struct JournalEntry {
    std::uint64_t unix_ms = 0;  // when the request came
    int client = 0;             // the pipe instance
    std::string_view request;   // the line as sent
    std::string_view reply;     // the answer line
    std::uint64_t ms = 0;       // from the request to the answer
};

// What a request is, by its command word: "input" (the input commands), "client_call"
// (province open|goto, map_mode), "game_call" (console, save load), "stream"
// (subscribe, unsubscribe) or "read" (everything else, refusals of unknown commands too).
const char* JournalKind(std::string_view request);

// One line, without the newline: seq, t, client, kind, request, ms, then from the answer
// ok, error, date and session (null when the answer has none).
std::string JournalLine(std::uint64_t seq, const JournalEntry& entry);

// The first line of a file: the format, Tongs' version and the game process.
std::string JournalHeader(std::string_view version, std::uint32_t pid, std::uint64_t process_start_ms,
                          std::uint64_t opened_unix_ms);

// journal-<process start ms>-<pid>.jsonl: the files sort by the start of their game.
std::string JournalFileName(std::uint32_t pid, std::uint64_t process_start_ms);

// Removes all but the newest `keep` journal files of `folder` (by name).
void PruneJournals(const std::filesystem::path& folder, int keep);

class Journal {
public:
    // Creates the folder and the file, writes the header and prunes old files; empty on
    // success, else why not (the journal then records nothing).
    std::string Open(const std::filesystem::path& folder, std::string_view version, std::uint32_t pid,
                     std::uint64_t process_start_ms, std::uint64_t now_unix_ms);
    // Any thread; each line is flushed, so a crash keeps what came before it.
    void Record(const JournalEntry& entry);
    std::string Path() const;
    std::uint64_t Lines() const;
    bool Truncated() const;
    bool IsOpen() const;

private:
    mutable std::mutex mutex_;
    std::ofstream out_;
    std::filesystem::path path_;
    std::uint64_t seq_ = 0;
    std::uint64_t bytes_ = 0;
    bool truncated_ = false;
    bool open_ = false;
};

}  // namespace tongs
