// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/workcalendar.h"
#include "model/project.h"

#include <QRandomGenerator>
#include <QTest>

using schedule::Calendar;
using schedule::CalendarException;
using schedule::Project;
using schedule::TimeRange;
using schedule::WorkCalendar;

namespace {

QDateTime dt(int y, int m, int d, int h, int min = 0)
{
    return QDateTime(QDate(y, m, d), QTime(h, min));
}

TimeRange range(int h1, int m1, int h2, int m2)
{
    TimeRange r;
    r.start = QTime(h1, m1);
    r.end = QTime(h2, m2);
    return r;
}

constexpr qint64 kHour = 3600LL * 1000LL;

// A base calendar working Mon-Thu 06:00-12:00 and 13:00-17:00 (10h days).
Calendar fourTens()
{
    Calendar c;
    c.uniqueId = 3;
    c.name = QStringLiteral("Four Tens");
    c.workingDayMask = 0x0F;   // Mon..Thu
    c.workingTimes.resize(7);
    for (int i = 0; i < 4; ++i)
        c.workingTimes[i] = { range(6, 0, 12, 0), range(13, 0, 17, 0) };
    return c;
}

} // namespace

class TstWorkCalendar : public QObject
{
    Q_OBJECT
private slots:
    void defaultIsStandardWeek();
    void customWeek();
    void holidayException();
    void workingException();
    void inheritsBaseChain();
    void intersectsCalendarsAndExceptions();
    void unknownIdFallsBack();
    void longSpansMatchTheDayByDayWalk();
};

void TstWorkCalendar::defaultIsStandardWeek()
{
    const WorkCalendar cal;
    // Monday 2026-07-06 is a working day; Saturday is not.
    QVERIFY(cal.isWorkingDay(QDate(2026, 7, 6)));
    QVERIFY(!cal.isWorkingDay(QDate(2026, 7, 4)));
    QCOMPARE(cal.workPerDay(), 8 * kHour);
    // 17:00 Friday rolls to 08:00 Monday.
    QCOMPARE(cal.nextWorkStart(dt(2026, 7, 3, 17)), dt(2026, 7, 6, 8));
    // 8h from Monday 08:00 ends 17:00 the same day (lunch skipped).
    QCOMPARE(cal.addWork(dt(2026, 7, 6, 8), 8 * kHour), dt(2026, 7, 6, 17));
    QCOMPARE(cal.workBetween(dt(2026, 7, 6, 8), dt(2026, 7, 6, 17)), 8 * kHour);
}

void TstWorkCalendar::customWeek()
{
    Project p;
    p.calendars.append(fourTens());
    const WorkCalendar cal(p, 3);

    QCOMPARE(cal.workPerDay(), 10 * kHour);
    QVERIFY(cal.isWorkingDay(QDate(2026, 7, 9)));    // Thursday
    QVERIFY(!cal.isWorkingDay(QDate(2026, 7, 10)));  // Friday off
    // 10h from Monday 06:00 fills the whole day.
    QCOMPARE(cal.addWork(dt(2026, 7, 6, 6), 10 * kHour), dt(2026, 7, 6, 17));
    // 20h from Monday 06:00 = end of Tuesday.
    QCOMPARE(cal.addWork(dt(2026, 7, 6, 6), 20 * kHour), dt(2026, 7, 7, 17));
    // Thursday 17:00 rolls to Monday 06:00 (Friday-Sunday off).
    QCOMPARE(cal.nextWorkStart(dt(2026, 7, 9, 17)), dt(2026, 7, 13, 6));
}

void TstWorkCalendar::holidayException()
{
    Project p;
    Calendar std;
    std.uniqueId = 1;
    std.name = QStringLiteral("Standard");
    std.workingDayMask = 0x1F;
    std.workingTimes.resize(7);
    for (int i = 0; i < 5; ++i)
        std.workingTimes[i] = { range(8, 0, 12, 0), range(13, 0, 17, 0) };
    CalendarException holiday;
    holiday.fromDate = QDate(2026, 7, 3);
    holiday.toDate = QDate(2026, 7, 3);
    holiday.working = false;
    std.exceptions.append(holiday);
    p.calendars.append(std);

    const WorkCalendar cal(p, 1);
    QVERIFY(!cal.isWorkingDay(QDate(2026, 7, 3)));   // the holiday Friday
    QVERIFY(cal.isWorkingDay(QDate(2026, 7, 2)));
    // Work crossing the holiday: 8h from Thursday 08:00 ends Thursday 17:00,
    // 9h lands Monday 09:00 (Friday holiday + weekend skipped).
    QCOMPARE(cal.addWork(dt(2026, 7, 2, 8), 9 * kHour), dt(2026, 7, 6, 9));
}

void TstWorkCalendar::workingException()
{
    Project p;
    Calendar std;
    std.uniqueId = 1;
    std.workingDayMask = 0x1F;
    std.workingTimes.resize(7);
    for (int i = 0; i < 5; ++i)
        std.workingTimes[i] = { range(8, 0, 12, 0), range(13, 0, 17, 0) };
    CalendarException crunch;   // a working Saturday, 09:00-15:00
    crunch.fromDate = QDate(2026, 7, 4);
    crunch.toDate = QDate(2026, 7, 4);
    crunch.working = true;
    crunch.workingTimes = { range(9, 0, 15, 0) };
    std.exceptions.append(crunch);
    p.calendars.append(std);

    const WorkCalendar cal(p, 1);
    QVERIFY(cal.isWorkingDay(QDate(2026, 7, 4)));
    QCOMPARE(cal.workBetween(dt(2026, 7, 4, 0), dt(2026, 7, 5, 0)), 6 * kHour);
    // Friday 17:00 now rolls into Saturday 09:00 instead of Monday.
    QCOMPARE(cal.nextWorkStart(dt(2026, 7, 3, 17)), dt(2026, 7, 4, 9));
}

void TstWorkCalendar::inheritsBaseChain()
{
    Project p;
    p.calendars.append(fourTens());
    Calendar rsc;   // a resource calendar: no weekly data of its own
    rsc.uniqueId = 7;
    rsc.baseCalendarUniqueId = 3;
    CalendarException vacation;
    vacation.fromDate = QDate(2026, 7, 6);
    vacation.toDate = QDate(2026, 7, 7);
    vacation.working = false;
    rsc.exceptions.append(vacation);
    p.calendars.append(rsc);

    const WorkCalendar cal(p, 7);
    QCOMPARE(cal.workPerDay(), 10 * kHour);          // week inherited from base
    QVERIFY(!cal.isWorkingDay(QDate(2026, 7, 6)));   // own exception applies
    QVERIFY(!cal.isWorkingDay(QDate(2026, 7, 7)));
    QVERIFY(cal.isWorkingDay(QDate(2026, 7, 8)));    // Wednesday works again
}

void TstWorkCalendar::intersectsCalendarsAndExceptions()
{
    Project p;
    Calendar standard;
    standard.uniqueId = 1;
    standard.workingDayMask = 0x1F;
    standard.workingTimes.resize(7);
    for (int i = 0; i < 5; ++i)
        standard.workingTimes[i] = { range(8, 0, 12, 0), range(13, 0, 17, 0) };
    p.calendars.append(standard);

    Calendar resource = fourTens();
    CalendarException vacation;
    vacation.fromDate = QDate(2026, 7, 6);
    vacation.toDate = QDate(2026, 7, 6);
    vacation.working = false;
    resource.exceptions.append(vacation);
    p.calendars.append(resource);

    const WorkCalendar combined = WorkCalendar::intersection(
        { WorkCalendar(p, 1), WorkCalendar(p, 3) });
    QVERIFY(!combined.isWorkingDay(QDate(2026, 7, 6)));
    QCOMPARE(combined.workBetween(dt(2026, 7, 7, 0), dt(2026, 7, 8, 0)),
             8 * kHour);
    QCOMPARE(combined.addWork(dt(2026, 7, 6, 8), 8 * kHour),
             dt(2026, 7, 7, 17));
    QVERIFY(!combined.isWorkingDay(QDate(2026, 7, 10))); // resource Friday off
}

void TstWorkCalendar::unknownIdFallsBack()
{
    Project p;
    const WorkCalendar cal(p, 42);
    QCOMPARE(cal.workPerDay(), 8 * kHour);
    QVERIFY(cal.isWorkingDay(QDate(2026, 7, 6)));
}

namespace {

// The day-by-day definitions of workBetween/addWork, from the per-day working
// times alone. WorkCalendar crosses long spans with a cumulative day table;
// these are what it has to agree with to the millisecond.
qint64 msOf(const QTime &t) { return QTime(0, 0).msecsTo(t); }
QList<QPair<qint64, qint64>> periodsOn(const WorkCalendar &cal, const QDate &d)
{
    QList<QPair<qint64, qint64>> out;
    for (const TimeRange &r : cal.workingTimes(d))
        out.append({ msOf(r.start), r.end == QTime(0, 0) ? 24 * kHour : msOf(r.end) });
    return out;
}
QDateTime atMs(const QDate &d, qint64 ms)
{
    return QDateTime(d, QTime::fromMSecsSinceStartOfDay(int(ms)));
}

qint64 refWorkBetween(const WorkCalendar &cal, const QDateTime &from, const QDateTime &to)
{
    if (to <= from)
        return 0;
    qint64 total = 0;
    for (QDate d = from.date(); d <= to.date(); d = d.addDays(1)) {
        const qint64 lo = d == from.date() ? msOf(from.time()) : 0;
        const qint64 hi = d == to.date() ? msOf(to.time()) : 24 * kHour;
        for (const auto &p : periodsOn(cal, d)) {
            const qint64 b = qMax(lo, p.first), e = qMin(hi, p.second);
            if (e > b)
                total += e - b;
        }
    }
    return total;
}

QDateTime refAddWork(const WorkCalendar &cal, const QDateTime &from, qint64 millis)
{
    if (millis == 0)
        return cal.nextWorkStart(from);
    if (millis > 0) {
        const QDateTime cur = cal.nextWorkStart(from);
        QDate d = cur.date();
        qint64 ms = msOf(cur.time());
        qint64 remaining = millis;
        for (;;) {
            for (const auto &p : periodsOn(cal, d)) {
                if (ms >= p.second)
                    continue;
                const qint64 begin = qMax(ms, p.first);
                if (remaining <= p.second - begin)
                    return atMs(d, begin + remaining);
                remaining -= p.second - begin;
            }
            d = d.addDays(1);
            ms = 0;
        }
    }
    const QDateTime cur = cal.prevWorkEnd(from);
    QDate d = cur.date();
    qint64 ms = msOf(cur.time());
    qint64 remaining = -millis;
    for (;;) {
        const auto periods = periodsOn(cal, d);
        for (int i = periods.size() - 1; i >= 0; --i) {
            const auto &p = periods.at(i);
            if (ms <= p.first)
                continue;
            const qint64 end = qMin(ms, p.second);
            if (remaining <= end - p.first)
                return atMs(d, end - remaining);
            remaining -= end - p.first;
        }
        d = d.addDays(-1);
        ms = 24 * kHour;
    }
}

} // namespace

// Slack runs out to the project finish for every task, so scheduling asks for
// work over spans of years. Those go through the day table; every answer has
// to match walking the days one by one -- holidays, a recurring holiday, a
// working Saturday, a resource calendar intersected in, exact whole-day
// amounts that end on a period boundary, and backwards as well as forwards.
void TstWorkCalendar::longSpansMatchTheDayByDayWalk()
{
    Project p;
    Calendar standard;
    standard.uniqueId = 1;
    standard.workingDayMask = 0x1F;
    standard.workingTimes.resize(7);
    for (int i = 0; i < 5; ++i)
        standard.workingTimes[i] = { range(8, 0, 12, 0), range(13, 0, 17, 0) };
    CalendarException christmas;   // every 25 December
    christmas.fromDate = QDate(2020, 12, 25);
    christmas.toDate = QDate(2035, 12, 25);
    christmas.recurrence = CalendarException::Recurrence::YearlyByDate;
    christmas.month = 12;
    christmas.dayOfMonth = 25;
    standard.exceptions.append(christmas);
    CalendarException shutdown;   // a two-week shutdown
    shutdown.fromDate = QDate(2027, 8, 2);
    shutdown.toDate = QDate(2027, 8, 13);
    standard.exceptions.append(shutdown);
    CalendarException crunch;   // a working Saturday
    crunch.fromDate = QDate(2026, 10, 3);
    crunch.toDate = QDate(2026, 10, 3);
    crunch.working = true;
    crunch.workingTimes = { range(9, 0, 15, 0) };
    standard.exceptions.append(crunch);
    p.calendars.append(standard);
    Calendar resource = fourTens();
    CalendarException vacation;
    vacation.fromDate = QDate(2028, 3, 6);
    vacation.toDate = QDate(2028, 3, 17);
    resource.exceptions.append(vacation);
    p.calendars.append(resource);

    // One instance per calendar for the whole run, so each table grows
    // forwards and backwards as the random dates wander.
    const WorkCalendar calendars[] = {
        WorkCalendar(p, 1),
        WorkCalendar::intersection({ WorkCalendar(p, 1), WorkCalendar(p, 3) }),
    };
    QRandomGenerator rng(20261123);
    const QDate base(2024, 1, 1);
    for (const WorkCalendar &cal : calendars) {
        for (int i = 0; i < 1500; ++i) {
            const QDateTime from(base.addDays(rng.bounded(6 * 365)),
                                 QTime(rng.bounded(24), rng.bounded(4) * 15));
            const QDateTime to(from.date().addDays(rng.bounded(3 * 365)),
                               QTime(rng.bounded(24), rng.bounded(4) * 15));
            QCOMPARE(cal.workBetween(from, to), refWorkBetween(cal, from, to));
            QCOMPARE(cal.workBetween(to, from), refWorkBetween(cal, to, from));

            // Amounts up to ~2 working years, half of them whole 8h/10h days.
            qint64 amount = qint64(rng.bounded(4000)) * kHour / 4;
            if (i % 2)
                amount = qint64(rng.bounded(500) + 1) * (cal.workPerDay());
            QCOMPARE(cal.addWork(from, amount), refAddWork(cal, from, amount));
            QCOMPARE(cal.addWork(from, -amount), refAddWork(cal, from, -amount));
        }
    }
}

QTEST_MAIN(TstWorkCalendar)
#include "tst_workcalendar.moc"
