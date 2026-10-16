#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace tongs {

// anvil/data/tongs/config.toml. Only flat `key = value` lines are
// understood; the pipe stays closed unless `enabled = true`, and the input
// commands (key, pause, resume, speed) answer an error unless `input = true`.
struct Config {
    bool enabled = false;
    bool input = false;
    std::vector<std::string> notes;  // problems found while parsing, for the log
};

Config ParseConfig(std::string_view text);

extern const char kDefaultConfig[];

}  // namespace tongs
