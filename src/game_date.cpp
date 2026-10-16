#include "game_date.hpp"

namespace lc {
namespace {

constexpr std::int64_t kHoursToYearZero = 43'800'000;  // 5000 years * 365 days * 24 hours
constexpr int kMonthDays[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};

std::int64_t FloorDiv(std::int64_t value, std::int64_t divisor) {
    std::int64_t q = value / divisor;
    if ((value % divisor != 0) && ((value < 0) != (divisor < 0))) --q;
    return q;
}

}  // namespace

GameDate DecodeGameDate(std::int32_t raw) {
    const std::int64_t hours = static_cast<std::int64_t>(raw) - kHoursToYearZero;
    const std::int64_t days = FloorDiv(hours, 24);
    const std::int64_t year = FloorDiv(days, 365);
    int day_of_year = static_cast<int>(days - year * 365);
    int month = 0;
    while (day_of_year >= kMonthDays[month]) {
        day_of_year -= kMonthDays[month];
        ++month;
    }
    return {static_cast<int>(year), month + 1, day_of_year + 1, static_cast<int>(hours - days * 24)};
}

std::string FormatGameDate(const GameDate& date) {
    return std::to_string(date.year) + "." + std::to_string(date.month) + "." + std::to_string(date.day);
}

}  // namespace lc
