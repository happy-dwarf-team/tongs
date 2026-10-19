#pragma once

#include "protocol.hpp"

#include <cstdint>
#include <mutex>

namespace tongs {

// Tells loads of a save apart. Reloading the same save can keep the session
// object at its address and the date where it was, so neither tells that the
// game state was replaced; the session object being gone (or not yet a session)
// for a moment does. Every state read is observed; a read that sees a loaded
// game after one that saw none starts a new load. Thread-safe.
class LoadWatch {
public:
    // Records what `s` saw and sets s.load to the load it belongs to (0 without a game).
    void Observe(StateSnapshot& s);

private:
    std::mutex mutex_;
    std::uint64_t load_ = 0;
    bool left_ = true;  // no loaded game seen since the last one
    std::uintptr_t session_ = 0;
    std::int32_t date_raw_ = 0;
};

}  // namespace tongs
