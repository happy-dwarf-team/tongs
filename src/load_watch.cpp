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

}  // namespace tongs
