#pragma once

#include "navigation.hpp"
#include "world.hpp"

#include <tongs/tongs_navigation.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace tongs {

// The tongs.navigation service (include/tongs/tongs_navigation.h, docs/adr/0004):
// the pipe's navigation (navigation.hpp, game_navigation.cpp) for other plugins.
// The checks are the pipe's `province open`; the calls the game's. Nothing here
// touches game memory, so the tests give it facts built by hand and play the
// main thread.

// The selection and the views as the main thread read them (game_navigation.cpp).
struct ProvinceViewRaw {
    SelectionRaw selection;
    bool view_shown = false;
    bool colonisation_shown = false;
    bool views_open = false;
};

// What the service needs from the game; the plugin fills it from game_navigation.cpp.
struct NavigationServiceHost {
    std::function<std::uint32_t()> current_thread;
    std::function<std::uint32_t()> main_thread;  // 0 before the first frame
    std::function<bool()> in_game;
    std::function<bool()> suspended;             // Anvil suspended Tongs
    std::function<WorldFailure(ProvinceViewRaw&)> read_view;
    std::function<WorldFailure(std::int32_t, NavigationFacts&)> read_facts;
    // The calls of one open: close the shown views, the camera, then select. False on a fault.
    std::function<bool(const NavigationFacts&, bool close_views, bool camera, unsigned long& fault_code)> call;
    std::function<void()> count;  // one client call, for the pipe's counts; may be unset
    std::function<void(const std::string&)> log_error;
};

class NavigationService {
public:
    explicit NavigationService(NavigationServiceHost host) : host_(std::move(host)) {}

    AnvilResult ProvinceView(TongsProvinceView* out);
    AnvilResult OpenProvince(std::int32_t id, std::uint32_t flags, std::uint32_t* outcome);

private:
    bool OnMainThread() const;
    void Report(const std::string& error);

    NavigationServiceHost host_;
    std::string last_error_;  // main thread only
};

constexpr std::uint32_t kNavigationServiceFlags = TONGS_NAV_KEEP_VIEWS | TONGS_NAV_NO_CAMERA;

// The outcome for a refusal of NavigationRefusal or of the reads.
std::uint32_t NavigationOutcome(const WorldFailure& refusal);

// Shared with the pipe's province open and goto: set when the game's code faulted
// in a navigation call, which refuses every later one until the game exits.
bool NavigationTripped();
void TripNavigation();

}  // namespace tongs
