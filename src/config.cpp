#include "config.hpp"

namespace lc {
namespace {

std::string_view Trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t' || s.front() == '\r')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r')) s.remove_suffix(1);
    return s;
}

}  // namespace

const char kDefaultConfig[] =
    "# EU4 Lab Control\n"
    "# enabled = true opens the local pipe \\\\.\\pipe\\eu4-lab-control for the current Windows user.\n"
    "# It only reads game state; nothing in the game is changed.\n"
    "enabled = false\n";

Config ParseConfig(std::string_view text) {
    Config config;
    int line_number = 0;
    while (!text.empty()) {
        const std::size_t end = text.find('\n');
        std::string_view line = text.substr(0, end);
        text = end == std::string_view::npos ? std::string_view{} : text.substr(end + 1);
        ++line_number;
        if (const std::size_t hash = line.find('#'); hash != std::string_view::npos) line = line.substr(0, hash);
        line = Trim(line);
        if (line.empty()) continue;
        const std::size_t eq = line.find('=');
        if (eq == std::string_view::npos) {
            config.notes.push_back("config line " + std::to_string(line_number) + ": expected key = value");
            continue;
        }
        const std::string_view key = Trim(line.substr(0, eq));
        const std::string_view value = Trim(line.substr(eq + 1));
        if (key == "enabled") {
            if (value == "true") {
                config.enabled = true;
            } else if (value == "false") {
                config.enabled = false;
            } else {
                config.notes.push_back("config line " + std::to_string(line_number) + ": enabled must be true or false");
            }
        } else {
            config.notes.push_back("config line " + std::to_string(line_number) + ": unknown key '" +
                                   std::string(key) + "' ignored");
        }
    }
    return config;
}

}  // namespace lc
