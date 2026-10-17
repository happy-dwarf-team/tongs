#include "game_date.hpp"

namespace tongs {
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

std::int64_t EncodeGameDate(const GameDate& date) {
    std::int64_t day_of_year = date.day - 1;
    for (int m = 0; m < date.month - 1; ++m) day_of_year += kMonthDays[m];
    return kHoursToYearZero + (static_cast<std::int64_t>(date.year) * 365 + day_of_year) * 24 + date.hour;
}

std::optional<std::int32_t> ParseGameDate(std::string_view text) {
    long long parts[4] = {0, 0, 0, 0};
    int count = 0;
    bool negative = false;
    if (!text.empty() && text.front() == '-') {
        negative = true;
        text.remove_prefix(1);
    }
    while (count < 4) {
        std::size_t digits = 0;
        long long value = 0;
        while (digits < text.size() && text[digits] >= '0' && text[digits] <= '9') {
            if (digits >= 6) return std::nullopt;
            value = value * 10 + (text[digits] - '0');
            ++digits;
        }
        if (digits == 0) return std::nullopt;
        parts[count++] = value;
        text.remove_prefix(digits);
        if (text.empty()) break;
        if (text.front() != '.') return std::nullopt;
        text.remove_prefix(1);
    }
    if (!text.empty() || count < 3) return std::nullopt;
    const GameDate date{static_cast<int>(negative ? -parts[0] : parts[0]), static_cast<int>(parts[1]),
                        static_cast<int>(parts[2]), static_cast<int>(parts[3])};
    if (date.month < 1 || date.month > 12 || date.day < 1 || date.day > kMonthDays[date.month - 1] ||
        date.hour < 0 || date.hour > 23) {
        return std::nullopt;
    }
    const std::int64_t raw = EncodeGameDate(date);
    if (raw < INT32_MIN || raw > INT32_MAX) return std::nullopt;
    return static_cast<std::int32_t>(raw);
}

}  // namespace tongs
