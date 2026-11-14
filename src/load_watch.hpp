#pragma once

#include "protocol.hpp"

#include <anvil/anvil_game.h>

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

// `out_of_sync` and `paused_oos` from anvil-game's out_of_sync() (docs/design.md, "Out of sync").
// Its flags are as of the last outer frame, and no outer frame runs between Tongs seeing a new load
// and the first frame of that session, so in that gap they can still be the previous session's: a
// resync loads a save in the middle of a desynced game. The view reports neither flag until an outer
// frame ran after the read that saw the load. Thread-safe.
class OosView {
public:
    // `s` after LoadWatch::Observe; `frame` is frame_number(), `flags` out_of_sync() (ANVIL_GAME_OOS_*).
    void Apply(StateSnapshot& s, std::uint64_t frame, std::uint32_t flags);

private:
    std::mutex mutex_;
    std::uint64_t load_ = 0;
    std::uint64_t frame_at_load_ = 0;
};

}  // namespace tongs
