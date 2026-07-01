// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_CALENDAR_H
#define SCHEDULE_CALENDAR_H

#include "scheduleio_export.h"

#include <QDate>
#include <QList>
#include <QString>
#include <QTime>

namespace schedule {

// A working-time period within a day (e.g. 08:00-12:00).
class SCHEDULEIO_EXPORT TimeRange
{
public:
    QTime start;
    QTime end;

    bool operator==(const TimeRange &o) const { return start == o.start && end == o.end; }
    bool operator!=(const TimeRange &o) const { return !(*this == o); }
};

// A calendar exception: a date range that overrides the normal working week
// (e.g. a holiday, or a one-off working day). `working` is false for a non-working
// exception (a day off); when true, `workingTimes` holds the special hours.
class SCHEDULEIO_EXPORT CalendarException
{
public:
    QDate fromDate;
    QDate toDate;
    QString name;
    bool working = false;
    QList<TimeRange> workingTimes;

    bool operator==(const CalendarException &o) const
    {
        return fromDate == o.fromDate && toDate == o.toDate && name == o.name
            && working == o.working && workingTimes == o.workingTimes;
    }
    bool operator!=(const CalendarException &o) const { return !(*this == o); }
};

// A working-time calendar (entity type 0xb / "Calendar").
// Working days are represented as a 7-bit mask, Monday(bit0)..Sunday(bit6).
class SCHEDULEIO_EXPORT Calendar
{
public:
    int uniqueId = 0;
    QString name;
    int baseCalendarUniqueId = -1;   // -1 == none (this is a base calendar)
    quint8 workingDayMask = 0;

    // Working-time periods per weekday: 7 entries, index 0=Monday..6=Sunday. Each
    // entry is the list of periods worked that day (empty == a non-working day).
    QList<QList<TimeRange>> workingTimes;
    // Date-range overrides of the normal week (holidays, one-off working days).
    QList<CalendarException> exceptions;

    bool operator==(const Calendar &o) const;
    bool operator!=(const Calendar &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_CALENDAR_H
