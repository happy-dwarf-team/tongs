#pragma once

#include "ui_tree.hpp"

#include <tongs/tongs_ui.h>

#include <cstdint>
#include <functional>
#include <string>
#include <utility>

namespace tongs {

// The tongs.ui service (include/tongs/tongs_ui.h, docs/adr/0002): a second
// adapter of the tree the pipe's `ui find` reads. The tree and its view
// (ui_tree.hpp) are the same; the service turns them into visits instead of
// JSON. Nothing here touches game memory, so the tests give it trees built by
// hand and play the main thread.

// What the service needs from the game; the plugin fills it from game_ui.cpp
// and anvil-game.
struct UiServiceHost {
    std::function<std::uint32_t()> current_thread;
    std::function<std::uint32_t()> main_thread;    // 0 before the first frame
    std::function<std::string(UiTree&)> read_tree;  // the whole tree; an error text or empty
    std::function<std::uintptr_t()> read_hovered;   // mgr+0x130; 0 when nothing is hovered
    std::function<void(const std::string&)> log_error;
};

class UiService {
public:
    explicit UiService(UiServiceHost host) : host_(std::move(host)) {}

    AnvilResult Find(const char* selector, std::uint32_t flags, TongsUiVisit visit, void* context, std::uint32_t* count);
    AnvilResult Hovered(std::uint32_t flags, TongsUiVisit visit, void* context);

private:
    // ANVIL_OK when a call may read: on the main thread, not inside a visit.
    AnvilResult Admit() const;
    // Reads the tree; logs an error once until the next one that differs.
    AnvilResult ReadTree(UiTree& tree);

    UiServiceHost host_;
    bool visiting_ = false;  // main thread only
    std::string last_error_;
};

constexpr std::uint32_t kUiServiceFlags = TONGS_UI_HIDDEN | TONGS_UI_STALE | TONGS_UI_PATHS;

// find over a tree: the matches of `selector` in walk order, each after its
// ancestors (once), with parents as visit indices. Returns the number of matches.
std::uint32_t VisitUiFind(const UiTree& tree, const UiSelector& selector, std::uint32_t flags, std::uintptr_t hovered,
                          TongsUiVisit visit, void* context);

// hovered over a tree: the widget at `hovered`, then its ancestors; paths with TONGS_UI_PATHS.
void VisitUiHovered(const UiTree& tree, std::uintptr_t hovered, std::uint32_t flags, TongsUiVisit visit, void* context);

}  // namespace tongs
