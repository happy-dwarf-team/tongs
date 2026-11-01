#include "navigation_service.hpp"

#include <atomic>
#include <cstdio>

namespace tongs {
namespace {

std::atomic<bool> g_navigation_tripped{false};

// The fields of version 1 a consumer's TongsProvinceView must hold.
constexpr std::uint32_t kProvinceViewV1 = sizeof(TongsProvinceView);

}  // namespace

bool NavigationTripped() { return g_navigation_tripped.load(); }
void TripNavigation() { g_navigation_tripped.store(true); }

std::uint32_t NavigationOutcome(const WorldFailure& refusal) {
    const std::string& e = refusal.error;
    if (e == "not_in_game") return TONGS_NAV_NOT_IN_GAME;
    if (e == "unknown_province") return TONGS_NAV_UNKNOWN_PROVINCE;
    if (e == "not_selectable") return TONGS_NAV_NOT_SELECTABLE;
    if (e == "terra_incognita") return TONGS_NAV_TERRA_INCOGNITA;
    if (e == "selection_blocked") return TONGS_NAV_SELECTION_BLOCKED;
    return TONGS_NAV_READ_FAILED;
}

bool NavigationService::OnMainThread() const {
    const std::uint32_t main = host_.main_thread ? host_.main_thread() : 0;
    return main != 0 && host_.current_thread && host_.current_thread() == main;
}

void NavigationService::Report(const std::string& error) {
    if (error == last_error_) return;
    last_error_ = error;
    if (host_.log_error) host_.log_error("tongs.navigation: " + error);
}

AnvilResult NavigationService::ProvinceView(TongsProvinceView* out) {
    if (out == nullptr || out->size < kProvinceViewV1) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!OnMainThread()) return ANVIL_ERROR_WRONG_PHASE;
    TongsProvinceView view{};
    view.size = kProvinceViewV1;
    if (host_.in_game && host_.in_game()) {
        ProvinceViewRaw raw;
        if (const WorldFailure f = host_.read_view(raw); f.failed()) {
            Report(f.error + ": " + f.detail);
            return ANVIL_ERROR_INTERNAL;
        }
        view.selected = raw.selection.last_selected;
        view.shown = raw.selection.view_province;
        view.view_id = raw.selection.view_widget;
        view.colonisation_id = raw.selection.colonisation_widget;
        if (raw.view_shown) view.flags |= TONGS_NAV_VIEW_SHOWN;
        if (raw.colonisation_shown) view.flags |= TONGS_NAV_COLONISATION_SHOWN;
        if (raw.views_open) view.flags |= TONGS_NAV_VIEWS_OPEN;
    }
    *out = view;
    return ANVIL_OK;
}

AnvilResult NavigationService::OpenProvince(std::int32_t id, std::uint32_t flags, std::uint32_t* outcome) {
    if (outcome == nullptr || (flags & ~kNavigationServiceFlags) != 0) return ANVIL_ERROR_INVALID_ARGUMENT;
    if (!OnMainThread()) return ANVIL_ERROR_WRONG_PHASE;
    if (host_.suspended && host_.suspended()) {
        *outcome = TONGS_NAV_SUSPENDED;
        return ANVIL_OK;
    }
    if (NavigationTripped()) {
        *outcome = TONGS_NAV_TRIPPED;
        return ANVIL_OK;
    }
    if (!host_.in_game || !host_.in_game()) {
        *outcome = TONGS_NAV_NOT_IN_GAME;
        return ANVIL_OK;
    }
    NavigationFacts facts;
    WorldFailure refusal = host_.read_facts(id, facts);
    if (!refusal.failed()) refusal = NavigationRefusal(true, facts);
    if (refusal.failed()) {
        *outcome = NavigationOutcome(refusal);
        if (*outcome == TONGS_NAV_READ_FAILED) Report(refusal.error + ": " + refusal.detail);
        return ANVIL_OK;
    }
    // The pipe promotes this case to a game call (ADR 0003); the service makes none (ADR 0004).
    if (facts.discover) {
        *outcome = TONGS_NAV_DISCOVER;
        return ANVIL_OK;
    }
    unsigned long code = 0;
    const bool ok = host_.call(facts, (flags & TONGS_NAV_KEEP_VIEWS) == 0, (flags & TONGS_NAV_NO_CAMERA) == 0, code);
    if (host_.count) host_.count();
    if (!ok) {
        TripNavigation();
        char text[16];
        std::snprintf(text, sizeof text, "0x%08lX", code);
        if (host_.log_error) {
            host_.log_error("tongs.navigation: open_province " + std::to_string(id) + " faulted (exception " + text +
                            " in the game's code); navigation is off until the game exits");
        }
        *outcome = TONGS_NAV_FAULTED;
        return ANVIL_OK;
    }
    *outcome = TONGS_NAV_OPENED;
    return ANVIL_OK;
}

}  // namespace tongs
