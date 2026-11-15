#include "load_watch.hpp"

namespace tongs {

void LoadWatch::Observe(StateSnapshot& s) {
    std::lock_guard lock(mutex_);
    if (!s.in_game) {
        left_ = true;
        s.load = 0;
        return;
    }
    // A date earlier than the last one read is another save, even if no read
    // fell into the moment without a game.
    if (left_ || s.session != session_ || s.date_raw < date_raw_) ++load_;
    left_ = false;
    session_ = s.session;
    date_raw_ = s.date_raw;
    s.load = load_;
}

bool FrameGate::Settled(const StateSnapshot& s, std::uint64_t frame) {
    std::lock_guard lock(mutex_);
    if (!s.in_game || s.load == 0) return false;
    if (s.load != load_) {
        load_ = s.load;
        frame_at_load_ = frame;
    }
    return frame >= frame_at_load_ + 2;
}

void OosView::Apply(StateSnapshot& s, std::uint64_t frame, std::uint32_t flags) {
    s.out_of_sync = s.paused_oos = false;
    if (!gate_.Settled(s, frame)) return;
    s.out_of_sync = (flags & ANVIL_GAME_OOS_DETECTED) != 0;
    s.paused_oos = (flags & ANVIL_GAME_OOS_PAUSED) != 0;
}

void ModeView::Apply(StateSnapshot& s, std::uint64_t frame, const AnvilGameSession& mode) {
    s.mode_known = s.ironman = s.multiplayer = false;
    if (!gate_.Settled(s, frame)) return;
    s.mode_known = true;
    s.ironman = mode.ironman != 0;
    s.multiplayer = mode.multiplayer != 0;
}

}  // namespace tongs
