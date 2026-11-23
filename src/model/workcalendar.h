// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_WORKCALENDAR_H
#define SCHEDULE_WORKCALENDAR_H

#include "scheduleio_export.h"
#include "model/calendar.h"

#include <QDateTime>
#include <QList>
#include <QSharedPointer>
#include <QVector>

namespace schedule {

class Project;

// A resolved working-time calendar the scheduling engine can walk: the
// base-calendar chain, weekly working times and date exceptions of a
// schedule::Calendar flattened into one queryable object (WINPROJ's
// TBkndCalTimeMap plays this role over its Bknd calendar objects).
//
// Defaults to MS Project's Standard calendar: Monday-Friday, 08:00-12:00 and
// 13:00-17:00 (8 working hours per day).
class SCHEDULEIO_EXPORT WorkCalendar
{
public:
    struct Period { qint64 begin = 0; qint64 end = 0; };   // ms of day

    WorkCalendar();   // the built-in Standard week, no exceptions

    // Resolve `calendarUniqueId` against project.calendars: the calendar's own
    // weekly times where defined (falling back through its base chain), plus
    // the chain's exceptions (nearest calendar wins on overlapping dates).
    // Unknown ids resolve to the built-in Standard week.
    WorkCalendar(const Project &project, int calendarUniqueId);

    // Return a calendar whose working periods are the overlap of every input
    // calendar. This is the rule Microsoft Project applies to a task calendar
    // (or project calendar) and an assigned work resource's calendar.
    static WorkCalendar intersection(const QList<WorkCalendar> &calendars);

    bool isWorkingDay(const QDate &d) const;

    // Working periods for a calendar date, exceptions applied.
    QList<TimeRange> workingTimes(const QDate &d) const;

    // Roll forward to the next instant work can start (a period end such as
    // 17:00 rolls to the next period's begin).
    QDateTime nextWorkStart(const QDateTime &dt) const;

    // Roll back to the previous instant work can end.
    QDateTime prevWorkEnd(const QDateTime &dt) const;

    // Advance (negative: rewind) by an amount of working time.
    // addWork(x, 0) == nextWorkStart(x).
    QDateTime addWork(const QDateTime &from, qint64 millis) const;

    // Working milliseconds between two instants (0 when to <= from).
    qint64 workBetween(const QDateTime &from, const QDateTime &to) const;

    // Working milliseconds in a typical working day (the longest weekly day),
    // for duration display in "days". The Standard week gives 8h.
    qint64 workPerDay() const;

public: // exposed for deterministic recurrence evaluation helpers
    // One date-range override, leaf-most calendar first in m_exceptions.
    struct Exception {
        QDate from, to;
        bool working = false;
        QVector<Period> periods;
        CalendarException::Recurrence recurrence = CalendarException::Recurrence::None;
        int interval = 1;
        quint8 weekDayMask = 0;
        int dayOfMonth = 0;
        int month = 0;
        int weekPosition = 0;
        int occurrences = 0;
    };

private:

    const QVector<Period> &periodsFor(const QDate &d) const;

    // Working milliseconds per day as prefix sums over a date range, built on
    // first use and grown as needed. workBetween and addWork use it to cross
    // long spans in one step instead of walking day by day: scheduling asks
    // for slack out to the project finish for every task, which on a large
    // schedule is thousands of days per task. Shared by copies (the calendar
    // never changes after construction), so a calendar passed around by value
    // builds it once.
    struct DayWorkTable;
    bool ensureDayWork(const QDate &first, const QDate &last) const;
    qint64 workBefore(const QDate &d) const;   // needs ensureDayWork() covering d
    qint64 workOnDay(const QDate &d, qint64 fromMs, qint64 toMs) const;

    QVector<Period> m_week[7];        // resolved weekly periods, index 0=Monday
    QList<Exception> m_exceptions;    // leaf calendar's first (they win)
    QVector<Period> m_none;           // empty list for non-working days
    QList<QSharedPointer<WorkCalendar>> m_intersectionCalendars;
    mutable QVector<Period> m_intersectionScratch;
    QSharedPointer<DayWorkTable> m_dayWork;
};

} // namespace schedule

#endif // SCHEDULE_WORKCALENDAR_H
