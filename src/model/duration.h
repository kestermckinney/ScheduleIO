// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_DURATION_H
#define SCHEDULE_DURATION_H

#include "scheduleio_export.h"

#include <QString>
#include <QtGlobal>

namespace schedule {

// Duration display units, using Microsoft Project's numeric codes (the same
// values MSPDI's DurationFormat / LagFormat elements carry, and the same codes
// WINPROJ stores next to each duration field). All quantities are stored
// canonically in milliseconds; the unit only controls how a value is entered
// and displayed. "Elapsed" units measure wall-clock time (24-hour days, 7-day
// weeks) instead of working time (8-hour days, 5-day weeks).
namespace Duration {

enum Unit {
    Minutes = 3,  ElapsedMinutes = 4,
    Hours   = 5,  ElapsedHours   = 6,
    Days    = 7,  ElapsedDays    = 8,
    Weeks   = 9,  ElapsedWeeks   = 10,
    Months  = 11, ElapsedMonths  = 12,
};

// Working-time conversion constants, mirroring MS Project's defaults
// (Options > Schedule): 8 hours/day, 40 hours/week, 20 days/month.
constexpr qint64 kMillisPerMinute = 60LL * 1000LL;
constexpr qint64 kMillisPerHour   = 60LL * kMillisPerMinute;
constexpr qint64 kMillisPerDay    = 8LL * kMillisPerHour;
constexpr qint64 kMillisPerWeek   = 5LL * kMillisPerDay;
constexpr qint64 kMillisPerMonth  = 20LL * kMillisPerDay;
// Elapsed (calendar-time) variants: 24-hour days, 7-day weeks, 30-day months.
constexpr qint64 kMillisPerElapsedDay   = 24LL * kMillisPerHour;
constexpr qint64 kMillisPerElapsedWeek  = 7LL * kMillisPerElapsedDay;
constexpr qint64 kMillisPerElapsedMonth = 30LL * kMillisPerElapsedDay;

SCHEDULEIO_EXPORT bool isElapsed(int unit);

// Milliseconds represented by one unit (e.g. Days -> 8h). Unknown units fall
// back to Days.
SCHEDULEIO_EXPORT qint64 unitMillis(int unit);

// MS Project also stores "estimated" duration units as base + 32 (e.g. 39 =
// estimated days) and a null code 21. Map any such code onto the plain 3..12
// range, defaulting to Days for anything unrecognised.
SCHEDULEIO_EXPORT int normalizeUnit(int unit);

SCHEDULEIO_EXPORT qint64 toMillis(double value, int unit);
SCHEDULEIO_EXPORT double fromMillis(qint64 millis, int unit);

// "3 days", "2.5 wks", "4 hrs", "3 edays" - MS Project's column abbreviations.
SCHEDULEIO_EXPORT QString format(qint64 millis, int unit = Days);

// Parse a user-entered duration: "3d", "2 wks", "1.5w", "30 min", "2 emo",
// "4 hours", or a bare number (which takes `defaultUnit`, days by default,
// matching MS Project's "Duration is entered in" option). A leading '-' or '+'
// is accepted so lags/leads can be negative. Returns false (outputs untouched)
// when the text is not a valid duration.
SCHEDULEIO_EXPORT bool parse(const QString &text, qint64 *millis, int *unit,
                             int defaultUnit = Days);

} // namespace Duration
} // namespace schedule

#endif // SCHEDULE_DURATION_H
