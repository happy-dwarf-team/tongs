#pragma once

#include <cstdint>
#include <mutex>

namespace tongs {

// Counts the game's frames, as the frame hook sees them (docs/design.md,
// "Dispatcher"): outer frames of the main loop and nested frames drawn from
// inside a game update or a save load. The hook reports on the main thread;
// `frames` reads on a pipe thread.
class FrameStats {
public:
    struct Snapshot {
        std::uint64_t outer_total = 0;
        std::uint64_t nested_total = 0;
        std::uint32_t outer_per_s = 0;   // in the last complete second
        std::uint32_t nested_per_s = 0;
        std::uint64_t outer_since = 0;   // since the previous Take()
        std::uint64_t nested_since = 0;
        std::uint64_t since_ms = 0;      // time since the previous Take() (or the first frame)
        std::uint64_t longest_gap_ms = 0;  // between outer frames since the previous Take(), the open gap included
    };

    void OnOuter(std::uint64_t now_ms);
    void OnNested(std::uint64_t now_ms);
    Snapshot Take(std::uint64_t now_ms);

private:
    void Roll(std::uint64_t now_ms);

    std::mutex mutex_;
    bool started_ = false;
    std::uint64_t window_start_ = 0;
    std::uint32_t outer_window_ = 0;
    std::uint32_t nested_window_ = 0;
    std::uint32_t outer_last_ = 0;
    std::uint32_t nested_last_ = 0;
    std::uint64_t outer_total_ = 0;
    std::uint64_t nested_total_ = 0;
    std::uint64_t outer_at_take_ = 0;
    std::uint64_t nested_at_take_ = 0;
    std::uint64_t take_at_ = 0;
    bool seen_outer_ = false;
    std::uint64_t last_outer_ = 0;
    std::uint64_t longest_gap_ = 0;
};

}  // namespace tongs
