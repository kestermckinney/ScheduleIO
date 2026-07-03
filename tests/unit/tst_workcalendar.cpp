// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/workcalendar.h"
#include "model/project.h"

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
    void unknownIdFallsBack();
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

void TstWorkCalendar::unknownIdFallsBack()
{
    Project p;
    const WorkCalendar cal(p, 42);
    QCOMPARE(cal.workPerDay(), 8 * kHour);
    QVERIFY(cal.isWorkingDay(QDate(2026, 7, 6)));
}

QTEST_MAIN(TstWorkCalendar)
#include "tst_workcalendar.moc"
