#include "frame_stats.hpp"

#include <algorithm>

namespace tongs {

void FrameStats::Roll(std::uint64_t now_ms) {
    if (!started_) {
        started_ = true;
        window_start_ = now_ms;
        take_at_ = now_ms;
        return;
    }
    const std::uint64_t elapsed = now_ms - window_start_;
    if (elapsed < 1000) return;
    // The window that just closed was complete; if a whole second passed after
    // it without a frame, the last complete second had none.
    outer_last_ = elapsed < 2000 ? outer_window_ : 0;
    nested_last_ = elapsed < 2000 ? nested_window_ : 0;
    outer_window_ = 0;
    nested_window_ = 0;
    window_start_ = now_ms;
}

void FrameStats::OnOuter(std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    Roll(now_ms);
    ++outer_window_;
    ++outer_total_;
    if (seen_outer_) longest_gap_ = std::max(longest_gap_, now_ms - last_outer_);
    seen_outer_ = true;
    last_outer_ = now_ms;
}

void FrameStats::OnNested(std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    Roll(now_ms);
    ++nested_window_;
    ++nested_total_;
}

FrameStats::Snapshot FrameStats::Take(std::uint64_t now_ms) {
    std::lock_guard lock(mutex_);
    Roll(now_ms);
    Snapshot s;
    s.outer_total = outer_total_;
    s.nested_total = nested_total_;
    s.outer_per_s = outer_last_;
    s.nested_per_s = nested_last_;
    s.outer_since = outer_total_ - outer_at_take_;
    s.nested_since = nested_total_ - nested_at_take_;
    s.since_ms = now_ms - take_at_;
    s.longest_gap_ms = seen_outer_ ? std::max(longest_gap_, now_ms - last_outer_) : 0;
    outer_at_take_ = outer_total_;
    nested_at_take_ = nested_total_;
    take_at_ = now_ms;
    longest_gap_ = 0;
    return s;
}

}  // namespace tongs
