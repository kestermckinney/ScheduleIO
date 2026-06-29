// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPCALENDAR_H
#define MPPCALENDAR_H

#include "mppio_export.h"

#include <QDate>
#include <QList>
#include <QString>
#include <QTime>

// A working-time period within a day (e.g. 08:00-12:00).
class MPPIO_EXPORT MppTimeRange
{
public:
    QTime start;
    QTime end;

    bool operator==(const MppTimeRange &o) const { return start == o.start && end == o.end; }
    bool operator!=(const MppTimeRange &o) const { return !(*this == o); }
};

// A calendar exception: a date range that overrides the normal working week
// (e.g. a holiday, or a one-off working day). `working` is false for a non-working
// exception (a day off); when true, `workingTimes` holds the special hours.
class MPPIO_EXPORT MppCalendarException
{
public:
    QDate fromDate;
    QDate toDate;
    QString name;
    bool working = false;
    QList<MppTimeRange> workingTimes;

    bool operator==(const MppCalendarException &o) const
    {
        return fromDate == o.fromDate && toDate == o.toDate && name == o.name
            && working == o.working && workingTimes == o.workingTimes;
    }
    bool operator!=(const MppCalendarException &o) const { return !(*this == o); }
};

// A working-time calendar (entity type 0xb / "Calendar").
// Working days are represented as a 7-bit mask, Monday(bit0)..Sunday(bit6).
class MPPIO_EXPORT MppCalendar
{
public:
    int uniqueId = 0;
    QString name;
    int baseCalendarUniqueId = -1;   // -1 == none (this is a base calendar)
    quint8 workingDayMask = 0;

    // Working-time periods per weekday: 7 entries, index 0=Monday..6=Sunday. Each
    // entry is the list of periods worked that day (empty == a non-working day).
    QList<QList<MppTimeRange>> workingTimes;
    // Date-range overrides of the normal week (holidays, one-off working days).
    QList<MppCalendarException> exceptions;

    bool operator==(const MppCalendar &o) const;
    bool operator!=(const MppCalendar &o) const { return !(*this == o); }
};

#endif // MPPCALENDAR_H
