// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#include "util/time.hpp"

#include <array>
#include <chrono>
#include <cstdio>

#include "util/strutil.hpp"

namespace tcrm::timeutil {
namespace {

bool read_digits(std::string_view s, size_t pos, size_t count, int* out) {
    if (pos + count > s.size()) return false;
    int v = 0;
    for (size_t i = 0; i < count; ++i) {
        const char c = s[pos + i];
        if (c < '0' || c > '9') return false;
        v = v * 10 + (c - '0');
    }
    *out = v;
    return true;
}

int64_t floor_div(int64_t a, int64_t b) {
    int64_t q = a / b;
    if ((a % b != 0) && ((a < 0) != (b < 0))) --q;
    return q;
}

// Day number of the n-th (1-based) given weekday in a month; n = -1 means
// the last one.
int64_t nth_weekday(int year, int month, int weekday, int n) {
    if (n > 0) {
        const int64_t first = days_from_civil(year, month, 1);
        const int delta = (weekday - day_of_week(first) + 7) % 7;
        return first + delta + 7 * (n - 1);
    }
    const int64_t last = days_from_civil(year, month, days_in_month(year, month));
    const int delta = (day_of_week(last) - weekday + 7) % 7;
    return last - delta;
}

struct NamedZone {
    const char* name;
    int offset;
    TimeZone::Dst dst;
};

constexpr std::array<NamedZone, 14> kZones = {{
    {"UTC", 0, TimeZone::Dst::None},
    {"GMT", 0, TimeZone::Dst::None},
    {"America/New_York", -300, TimeZone::Dst::UnitedStates},
    {"America/Chicago", -360, TimeZone::Dst::UnitedStates},
    {"America/Denver", -420, TimeZone::Dst::UnitedStates},
    {"America/Phoenix", -420, TimeZone::Dst::None},
    {"America/Los_Angeles", -480, TimeZone::Dst::UnitedStates},
    {"America/Anchorage", -540, TimeZone::Dst::UnitedStates},
    {"Pacific/Honolulu", -600, TimeZone::Dst::None},
    {"Europe/London", 0, TimeZone::Dst::EuropeanUnion},
    {"Europe/Paris", 60, TimeZone::Dst::EuropeanUnion},
    {"Europe/Berlin", 60, TimeZone::Dst::EuropeanUnion},
    {"Asia/Kolkata", 330, TimeZone::Dst::None},
    {"Asia/Tokyo", 540, TimeZone::Dst::None},
}};

}  // namespace

// Howard Hinnant's days_from_civil / civil_from_days.
int64_t days_from_civil(int year, int month, int day) {
    const int64_t y = static_cast<int64_t>(year) - (month <= 2 ? 1 : 0);
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t mp = (month + 9) % 12;
    const int64_t doy = (153 * mp + 2) / 5 + day - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

Civil civil_from_days(int64_t z) {
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const int64_t doe = z - era * 146097;
    const int64_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const int64_t y = yoe + era * 400;
    const int64_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const int64_t mp = (5 * doy + 2) / 153;
    const int64_t d = doy - (153 * mp + 2) / 5 + 1;
    const int64_t m = mp < 10 ? mp + 3 : mp - 9;
    Civil c;
    c.year = static_cast<int>(y + (m <= 2 ? 1 : 0));
    c.month = static_cast<int>(m);
    c.day = static_cast<int>(d);
    return c;
}

bool is_leap_year(int year) {
    return year % 4 == 0 && year % 100 != 0;
}

int days_in_month(int year, int month) {
    static const int kDays[] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
    if (month < 1 || month > 12) return 0;
    if (month == 2 && is_leap_year(year)) return 29;
    return kDays[month - 1];
}

int day_of_week(int64_t days) {
    // 1970-01-01 was a Thursday.
    const int64_t w = (days + 4) % 7;
    return static_cast<int>(w < 0 ? w + 7 : w);
}

int64_t now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(system_clock::now().time_since_epoch()).count();
}

int64_t today_utc() {
    return days_of(now_ms());
}

int64_t days_of(int64_t ms) {
    return floor_div(ms, kMsPerDay);
}

bool parse_date(std::string_view s, int64_t* days) {
    s = str::trim(s);
    if (s.size() != 10 || s[4] != '-' || s[7] != '-') return false;
    int y = 0, m = 0, d = 0;
    if (!read_digits(s, 0, 4, &y) || !read_digits(s, 5, 2, &m) || !read_digits(s, 8, 2, &d)) return false;
    if (m < 1 || m > 12 || d < 1 || d > days_in_month(y, m)) return false;
    *days = days_from_civil(y, m, d);
    return true;
}

bool parse_datetime(std::string_view s, int64_t* ms) {
    s = str::trim(s);
    if (s.size() < 16) return false;
    int64_t days = 0;
    if (!parse_date(s.substr(0, 10), &days)) return false;
    if (s[10] != 'T' && s[10] != 't' && s[10] != ' ') return false;
    int hh = 0, mi = 0, ss = 0, frac = 0;
    if (!read_digits(s, 11, 2, &hh) || s[13] != ':' || !read_digits(s, 14, 2, &mi)) return false;
    size_t pos = 16;
    if (pos < s.size() && s[pos] == ':') {
        if (!read_digits(s, pos + 1, 2, &ss)) return false;
        pos += 3;
        if (pos < s.size() && s[pos] == '.') {
            ++pos;
            int scale = 100;
            size_t digits = 0;
            while (pos < s.size() && s[pos] >= '0' && s[pos] <= '9') {
                if (digits < 3) {
                    frac += (s[pos] - '0') * scale;
                    scale /= 10;
                }
                ++digits;
                ++pos;
            }
            if (digits == 0) return false;
        }
    }
    if (hh > 23 || mi > 59 || ss > 59) return false;
    int offset_minutes = 0;
    if (pos < s.size()) {
        const char c = s[pos];
        if (c == 'Z' || c == 'z') {
            ++pos;
        } else if (c == '+' || c == '-') {
            int oh = 0, om = 0;
            if (!read_digits(s, pos + 1, 2, &oh)) return false;
            size_t mpos = pos + 3;
            if (mpos < s.size() && s[mpos] == ':') ++mpos;
            if (!read_digits(s, mpos, 2, &om)) return false;
            if (oh > 14 || om > 59) return false;
            offset_minutes = oh * 60 + om;
            if (c == '-') offset_minutes = -offset_minutes;
            pos = mpos + 2;
        } else {
            return false;
        }
    }
    if (pos != s.size()) return false;
    *ms = days * kMsPerDay + hh * kMsPerHour + mi * kMsPerMinute + ss * kMsPerSecond + frac -
          offset_minutes * kMsPerMinute;
    return true;
}

std::string format_date(int64_t days) {
    const Civil c = civil_from_days(days);
    char buf[32];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02d", c.year, c.month, c.day);
    return buf;
}

std::string format_datetime(int64_t ms) {
    const int64_t days = days_of(ms);
    int64_t rem = ms - days * kMsPerDay;
    const Civil c = civil_from_days(days);
    const int hh = static_cast<int>(rem / kMsPerHour);
    rem %= kMsPerHour;
    const int mi = static_cast<int>(rem / kMsPerMinute);
    rem %= kMsPerMinute;
    const int ss = static_cast<int>(rem / kMsPerSecond);
    const int fff = static_cast<int>(rem % kMsPerSecond);
    char buf[48];
    std::snprintf(buf, sizeof buf, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", c.year, c.month, c.day, hh, mi, ss,
                  fff);
    return buf;
}

int64_t add_months(int64_t days, int months) {
    const Civil c = civil_from_days(days);
    int64_t total = static_cast<int64_t>(c.year) * 12 + (c.month - 1) + months;
    const int year = static_cast<int>(floor_div(total, 12));
    const int month = static_cast<int>(total - static_cast<int64_t>(year) * 12) + 1;
    int day = c.day;
    const int dim = days_in_month(year, month);
    if (day > dim) day = dim;
    return days_from_civil(year, month, day);
}

bool TimeZone::lookup(std::string_view name, TimeZone* out) {
    name = str::trim(name);
    for (const NamedZone& z : kZones) {
        if (str::iequals(name, z.name)) {
            *out = TimeZone(z.name, z.offset, z.dst);
            return true;
        }
    }
    if (!name.empty() && (name[0] == '+' || name[0] == '-')) {
        int oh = 0, om = 0;
        if (!read_digits(name, 1, 2, &oh)) return false;
        size_t mpos = 3;
        if (mpos < name.size() && name[mpos] == ':') ++mpos;
        if (!read_digits(name, mpos, 2, &om) || mpos + 2 != name.size()) return false;
        if (oh > 14 || om > 59) return false;
        int off = oh * 60 + om;
        if (name[0] == '-') off = -off;
        *out = TimeZone(std::string(name), off, Dst::None);
        return true;
    }
    return false;
}

bool TimeZone::in_dst(int64_t utc_ms) const {
    if (dst_ == Dst::None) return false;
    const Civil c = civil_from_days(days_of(utc_ms));
    const int year = c.year;
    if (dst_ == Dst::UnitedStates) {
        // Second Sunday in March at 02:00 local standard time through the
        // first Sunday in November at 02:00 local daylight time.
        const int64_t start_day = nth_weekday(year, 3, 0, 2);
        const int64_t end_day = nth_weekday(year, 11, 0, 1);
        const int64_t start = start_day * kMsPerDay + 2 * kMsPerHour - standard_offset_ * kMsPerMinute;
        const int64_t end = end_day * kMsPerDay + 2 * kMsPerHour - (standard_offset_ + 60) * kMsPerMinute;
        return utc_ms >= start && utc_ms < end;
    }
    // EU: last Sunday in March 01:00 UTC to last Sunday in October 01:00 UTC.
    const int64_t start = nth_weekday(year, 3, 0, -1) * kMsPerDay + kMsPerHour;
    const int64_t end = nth_weekday(year, 10, 0, -1) * kMsPerDay + kMsPerHour;
    return utc_ms >= start && utc_ms < end;
}

int TimeZone::offset_minutes_at(int64_t utc_ms) const {
    return standard_offset_ + (in_dst(utc_ms) ? 60 : 0);
}

int64_t TimeZone::local_date(int64_t utc_ms) const {
    return days_of(utc_ms + offset_minutes_at(utc_ms) * kMsPerMinute);
}

int64_t TimeZone::start_of_day_utc(int64_t local_days) const {
    // Guess with the offset at local midnight interpreted as UTC, then
    // correct once for a DST change between the guess and the answer.
    const int64_t naive = local_days * kMsPerDay;
    int64_t guess = naive - offset_minutes_at(naive) * kMsPerMinute;
    guess = naive - offset_minutes_at(guess) * kMsPerMinute;
    return guess;
}

}  // namespace tcrm::timeutil
