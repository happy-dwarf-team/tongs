#pragma once

#include <cstdint>
#include <string>

namespace lc {

// The game stores dates as hours since 1 January of year -5000 in 365-day
// years (no leap days); see docs/design.md.
struct GameDate {
    int year;
    int month;  // 1..12
    int day;    // 1..31
    int hour;   // 0..23
};

GameDate DecodeGameDate(std::int32_t raw);
std::string FormatGameDate(const GameDate& date);  // "1719.3.17", as the game writes it

}  // namespace lc
