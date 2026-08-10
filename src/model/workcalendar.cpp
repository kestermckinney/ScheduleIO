// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/workcalendar.h"
#include "model/project.h"

#include <QSet>

namespace schedule {

namespace {

constexpr qint64 kMsPerDay = 24LL * 60LL * 60LL * 1000LL;

inline qint64 msOfDay(const QDateTime &dt) { return QTime(0, 0).msecsTo(dt.time()); }
inline QDateTime at(const QDate &d, qint64 ms)
{
    return QDateTime(d, QTime::fromMSecsSinceStartOfDay(int(ms)));
}

// The Standard day: 08:00-12:00, 13:00-17:00.
QVector<WorkCalendar::Period> standardDay()
{
    constexpr qint64 kMin = 60LL * 1000LL;
    return { { 8 * 60 * kMin, 12 * 60 * kMin }, { 13 * 60 * kMin, 17 * 60 * kMin } };
}

QVector<WorkCalendar::Period> toPeriods(const QList<TimeRange> &times)
{
    QVector<WorkCalendar::Period> out;
    out.reserve(times.size());
    for (const TimeRange &r : times) {
        if (!r.start.isValid() || !r.end.isValid())
            continue;
        const qint64 b = QTime(0, 0).msecsTo(r.start);
        // A midnight end means "until the end of the day".
        const qint64 e = (r.end == QTime(0, 0)) ? kMsPerDay : QTime(0, 0).msecsTo(r.end);
        if (e > b)
            out.append({ b, e });
    }
    return out;
}

QVector<WorkCalendar::Period> intersectPeriods(const QVector<WorkCalendar::Period> &left,
                                                const QVector<WorkCalendar::Period> &right)
{
    QVector<WorkCalendar::Period> out;
    for (const WorkCalendar::Period &a : left) {
        for (const WorkCalendar::Period &b : right) {
            const qint64 begin = qMax(a.begin, b.begin);
            const qint64 end = qMin(a.end, b.end);
            if (end > begin)
                out.append({ begin, end });
        }
    }
    return out;
}

// True when the calendar carries its own weekly definition (base calendars
// always do; resource calendars usually inherit the whole week instead).
bool definesWeek(const Calendar &c)
{
    if (c.workingDayMask != 0)
        return true;
    for (const QList<TimeRange> &day : c.workingTimes)
        if (!day.isEmpty())
            return true;
    return false;
}

bool recurrenceMatches(const WorkCalendar::Exception &e, const QDate &date)
{
    if (date < e.from || (e.to.isValid() && date > e.to)) return false;
    const int interval = qMax(1, e.interval);
    bool match = false;
    switch (e.recurrence) {
    case CalendarException::Recurrence::None: match = true; break;
    case CalendarException::Recurrence::Daily:
        match = e.from.daysTo(date) % interval == 0; break;
    case CalendarException::Recurrence::Weekly: {
        const int weeks = e.from.daysTo(date) / 7;
        match = weeks % interval == 0
            && (e.weekDayMask & (1u << (date.dayOfWeek() - 1)));
        break;
    }
    case CalendarException::Recurrence::MonthlyByDate: {
        const int months = (date.year() - e.from.year()) * 12 + date.month() - e.from.month();
        match = months >= 0 && months % interval == 0 && date.day() == e.dayOfMonth;
        break;
    }
    case CalendarException::Recurrence::MonthlyByPosition:
    case CalendarException::Recurrence::YearlyByPosition: {
        const int months = (date.year() - e.from.year()) * 12 + date.month() - e.from.month();
        if (e.recurrence == CalendarException::Recurrence::YearlyByPosition && date.month() != e.month) break;
        if (e.recurrence == CalendarException::Recurrence::MonthlyByPosition && (months < 0 || months % interval)) break;
        if (!(e.weekDayMask & (1u << (date.dayOfWeek() - 1)))) break;
        const int position = (date.day() - 1) / 7 + 1;
        const bool last = date.addDays(7).month() != date.month();
        match = e.weekPosition == 5 ? last : position == e.weekPosition;
        break;
    }
    case CalendarException::Recurrence::YearlyByDate:
        match = date.month() == e.month && date.day() == e.dayOfMonth; break;
    }
    if (!match || e.occurrences <= 0) return match;
    int seen = 0;
    for (QDate d = e.from; d <= date; d = d.addDays(1)) {
        WorkCalendar::Exception unlimited = e; unlimited.occurrences = 0;
        if (recurrenceMatches(unlimited, d) && ++seen > e.occurrences) return false;
    }
    return seen <= e.occurrences;
}

} // namespace

WorkCalendar::WorkCalendar()
{
    for (int i = 0; i < 5; ++i)
        m_week[i] = standardDay();
}

WorkCalendar::WorkCalendar(const Project &project, int calendarUniqueId)
{
    // Leaf-to-base calendar chain (cycle-guarded).
    QList<const Calendar *> chain;
    QSet<int> seen;
    int cur = calendarUniqueId;
    while (cur >= 0 && !seen.contains(cur)) {
        seen.insert(cur);
        const Calendar *found = nullptr;
        for (const Calendar &c : project.calendars)
            if (c.uniqueId == cur) { found = &c; break; }
        if (!found)
            break;
        chain.append(found);
        cur = found->baseCalendarUniqueId;
    }

    // Weekly times come from the nearest calendar in the chain that defines a
    // week of its own; a derived calendar with no weekly data inherits its base.
    const Calendar *weekSource = nullptr;
    for (const Calendar *c : chain)
        if (definesWeek(*c)) { weekSource = c; break; }

    if (!weekSource) {
        for (int i = 0; i < 5; ++i)
            m_week[i] = standardDay();
    } else {
        for (int i = 0; i < 7; ++i) {
            const bool working = weekSource->workingDayMask & (1 << i);
            if (!working)
                continue;
            QVector<Period> p;
            if (i < weekSource->workingTimes.size())
                p = toPeriods(weekSource->workingTimes.at(i));
            // A working day with no recorded periods works the Standard hours.
            m_week[i] = p.isEmpty() ? standardDay() : p;
        }
    }

    // Exceptions from every level, leaf first, so the nearest calendar's
    // override wins when date ranges overlap.
    for (const Calendar *c : chain) {
        for (const CalendarException &x : c->exceptions) {
            if (!x.fromDate.isValid() || !x.toDate.isValid())
                continue;
            Exception e;
            e.from = x.fromDate;
            e.to = x.toDate;
            e.working = x.working;
            e.recurrence = x.recurrence;
            e.interval = x.interval;
            e.weekDayMask = x.weekDayMask;
            e.dayOfMonth = x.dayOfMonth;
            e.month = x.month;
            e.weekPosition = x.weekPosition;
            e.occurrences = x.occurrences;
            if (x.working) {
                e.periods = toPeriods(x.workingTimes);
                if (e.periods.isEmpty())
                    e.periods = standardDay();
            }
            m_exceptions.append(e);
        }
    }
}

WorkCalendar WorkCalendar::intersection(const QList<WorkCalendar> &calendars)
{
    if (calendars.isEmpty())
        return WorkCalendar();
    if (calendars.size() == 1)
        return calendars.first();

    WorkCalendar out = calendars.first();
    out.m_exceptions.clear();
    out.m_intersectionCalendars.clear();
    for (const WorkCalendar &calendar : calendars)
        out.m_intersectionCalendars.append(QSharedPointer<WorkCalendar>::create(calendar));
    for (int day = 0; day < 7; ++day) {
        QVector<Period> periods = calendars.first().m_week[day];
        for (int i = 1; i < calendars.size(); ++i)
            periods = intersectPeriods(periods, calendars.at(i).m_week[day]);
        out.m_week[day] = periods;
    }
    return out;
}

const QVector<WorkCalendar::Period> &WorkCalendar::periodsFor(const QDate &d) const
{
    if (!m_intersectionCalendars.isEmpty()) {
        m_intersectionScratch = m_intersectionCalendars.first()->periodsFor(d);
        for (int i = 1; i < m_intersectionCalendars.size(); ++i)
            m_intersectionScratch = intersectPeriods(
                m_intersectionScratch, m_intersectionCalendars.at(i)->periodsFor(d));
        return m_intersectionScratch;
    }
    for (const Exception &e : m_exceptions)
        if (recurrenceMatches(e, d))
            return e.working ? e.periods : m_none;
    return m_week[d.dayOfWeek() - 1];
}

bool WorkCalendar::isWorkingDay(const QDate &d) const
{
    return !periodsFor(d).isEmpty();
}

QList<TimeRange> WorkCalendar::workingTimes(const QDate &d) const
{
    QList<TimeRange> out;
    for (const Period &p : periodsFor(d)) {
        TimeRange r;
        r.start = QTime::fromMSecsSinceStartOfDay(int(p.begin));
        r.end = (p.end >= kMsPerDay) ? QTime(0, 0) : QTime::fromMSecsSinceStartOfDay(int(p.end));
        out.append(r);
    }
    return out;
}

QDateTime WorkCalendar::nextWorkStart(const QDateTime &dt) const
{
    if (!dt.isValid())
        return dt;
    QDate d = dt.date();
    qint64 ms = msOfDay(dt);
    for (int guard = 0; guard < 4000; ++guard) {   // > 10 years of non-working days
        for (const Period &p : periodsFor(d)) {
            if (ms < p.begin)
                return at(d, p.begin);
            if (ms < p.end)
                return at(d, ms);
        }
        d = d.addDays(1);
        ms = 0;
    }
    return dt;
}

QDateTime WorkCalendar::prevWorkEnd(const QDateTime &dt) const
{
    if (!dt.isValid())
        return dt;
    QDate d = dt.date();
    qint64 ms = msOfDay(dt);
    for (int guard = 0; guard < 4000; ++guard) {
        const QVector<Period> &periods = periodsFor(d);
        for (int i = periods.size() - 1; i >= 0; --i) {
            const Period &p = periods.at(i);
            if (ms > p.end)
                return at(d, p.end);
            if (ms > p.begin)
                return at(d, ms);
        }
        d = d.addDays(-1);
        ms = kMsPerDay;
    }
    return dt;
}

QDateTime WorkCalendar::addWork(const QDateTime &from, qint64 millis) const
{
    if (!from.isValid())
        return from;
    if (millis == 0)
        return nextWorkStart(from);

    if (millis > 0) {
        QDateTime cur = nextWorkStart(from);
        QDate d = cur.date();
        qint64 ms = msOfDay(cur);
        qint64 remaining = millis;
        for (int guard = 0; guard < 200000; ++guard) {   // ~770 working years
            for (const Period &p : periodsFor(d)) {
                if (ms >= p.end)
                    continue;
                const qint64 begin = qMax(ms, p.begin);
                const qint64 avail = p.end - begin;
                if (remaining <= avail)
                    return at(d, begin + remaining);
                remaining -= avail;
            }
            d = d.addDays(1);
            ms = 0;
        }
        return at(d, 0);
    }

    QDateTime cur = prevWorkEnd(from);
    QDate d = cur.date();
    qint64 ms = msOfDay(cur);
    qint64 remaining = -millis;
    for (int guard = 0; guard < 200000; ++guard) {
        const QVector<Period> &periods = periodsFor(d);
        for (int i = periods.size() - 1; i >= 0; --i) {
            const Period &p = periods.at(i);
            if (ms <= p.begin)
                continue;
            const qint64 end = qMin(ms, p.end);
            const qint64 avail = end - p.begin;
            if (remaining <= avail)
                return at(d, end - remaining);
            remaining -= avail;
        }
        d = d.addDays(-1);
        ms = kMsPerDay;
    }
    return at(d, 0);
}

qint64 WorkCalendar::workBetween(const QDateTime &from, const QDateTime &to) const
{
    if (!from.isValid() || !to.isValid() || to <= from)
        return 0;
    qint64 total = 0;
    QDate d = from.date();
    const QDate last = to.date();
    for (int guard = 0; d <= last && guard < 200000; ++guard, d = d.addDays(1)) {
        const qint64 dayBegin = (d == from.date()) ? msOfDay(from) : 0;
        const qint64 dayEnd = (d == last) ? msOfDay(to) : kMsPerDay;
        for (const Period &p : periodsFor(d)) {
            const qint64 b = qMax(dayBegin, p.begin);
            const qint64 e = qMin(dayEnd, p.end);
            if (e > b)
                total += e - b;
        }
    }
    return total;
}

qint64 WorkCalendar::workPerDay() const
{
    qint64 best = 0;
    for (const QVector<Period> &day : m_week) {
        qint64 sum = 0;
        for (const Period &p : day)
            sum += p.end - p.begin;
        best = qMax(best, sum);
    }
    return best > 0 ? best : 8LL * 60 * 60 * 1000;
}

} // namespace schedule
