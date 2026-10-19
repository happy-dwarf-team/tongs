#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace tongs {

// The game stores dates as hours since 1 January of year -5000 in 365-day
// years (no leap days); see docs/design.md.
struct GameDate {
    int year;
    int month;  // 1..12
    int day;    // 1..31
    int hour;   // 0..23
};

GameDate DecodeGameDate(std::int32_t raw);
// 1 January of year 1, hour 0: the date a session holds while a save is still
// being read into it (seen live when a save was reloaded); never a real date.
bool IsUnsetGameDate(std::int32_t raw);
std::string FormatGameDate(const GameDate& date);  // "1719.3.17", as the game writes it
std::int64_t EncodeGameDate(const GameDate& date);

// "1720.1.1" or "1720.1.1.12" (with an hour); nullopt when malformed or out of
// range (month 1-12, day within the month, no leap days, hour 0-23, the result
// within int32).
std::optional<std::int32_t> ParseGameDate(std::string_view text);

}  // namespace tongs
