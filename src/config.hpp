#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace lc {

// anvil/data/eu4-lab-control/config.toml. Only flat `key = value` lines are
// understood; the pipe stays closed unless `enabled = true`.
struct Config {
    bool enabled = false;
    std::vector<std::string> notes;  // problems found while parsing, for the log
};

Config ParseConfig(std::string_view text);

extern const char kDefaultConfig[];

}  // namespace lc
