// SPDX-License-Identifier: MIT
// Copyright (c) 2026 Tropibyte, Inc.

#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace tcrm::timeutil {

// Dates are whole days since 1970-01-01. Date-times are milliseconds since
// the Unix epoch, always UTC.

struct Civil {
    int year = 1970;
    int month = 1;  // 1..12
    int day = 1;    // 1..31
};

int64_t days_from_civil(int year, int month, int day);
Civil civil_from_days(int64_t days);

bool is_leap_year(int year);
int days_in_month(int year, int month);

/// 0 = Sunday ... 6 = Saturday.
int day_of_week(int64_t days);

int64_t now_ms();
int64_t today_utc();

constexpr int64_t kMsPerSecond = 1000;
constexpr int64_t kMsPerMinute = 60 * kMsPerSecond;
constexpr int64_t kMsPerHour = 60 * kMsPerMinute;
constexpr int64_t kMsPerDay = 24 * kMsPerHour;

/// Floor division of a date-time to its UTC date.
int64_t days_of(int64_t ms);

/// "YYYY-MM-DD". Rejects impossible dates such as 2026-02-30.
bool parse_date(std::string_view s, int64_t* days);

/// "YYYY-MM-DDThh:mm[:ss[.fff]]" followed by "Z", "+hh:mm", "+hhmm" or
/// nothing (UTC). A space may stand in for the "T".
bool parse_datetime(std::string_view s, int64_t* ms);

std::string format_date(int64_t days);

/// "2026-03-04T05:06:07.000Z"
std::string format_datetime(int64_t ms);

/// Adds calendar months, clamping the day to the end of the target month
/// (Jan 31 + 1 month = Feb 28 or 29).
int64_t add_months(int64_t days, int months);

/// A time zone with a fixed standard offset and an optional daylight-saving
/// rule. Only the rules the product supports are modeled.
class TimeZone {
public:
    enum class Dst { None, UnitedStates, EuropeanUnion };

    TimeZone() = default;
    TimeZone(std::string name, int standard_offset_minutes, Dst dst)
        : name_(std::move(name)), standard_offset_(standard_offset_minutes), dst_(dst) {}

    /// Accepts "UTC", supported IANA names such as "America/New_York", and
    /// fixed offsets such as "+05:30" or "-0800".
    static bool lookup(std::string_view name, TimeZone* out);
    static TimeZone utc() { return TimeZone("UTC", 0, Dst::None); }

    const std::string& name() const { return name_; }

    /// Offset from UTC in minutes at the given instant.
    int offset_minutes_at(int64_t utc_ms) const;

    /// The local calendar date at the given instant.
    int64_t local_date(int64_t utc_ms) const;

    /// The UTC instant of local midnight on `local_days`.
    int64_t start_of_day_utc(int64_t local_days) const;

private:
    bool in_dst(int64_t utc_ms) const;

    std::string name_ = "UTC";
    int standard_offset_ = 0;
    Dst dst_ = Dst::None;
};

}  // namespace tcrm::timeutil
