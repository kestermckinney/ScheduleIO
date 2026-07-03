// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/duration.h"
#include "model/scheduler.h"

#include <QtTest>

using namespace schedule;

namespace {

constexpr qint64 kHour = 3600LL * 1000LL;
constexpr qint64 kDay = 8 * kHour;   // one working day

// Mon 2026-07-06 is a Monday.
const QDate kMon(2026, 7, 6);

Task makeTask(int uid, const QDateTime &start, qint64 durMillis)
{
    Task t;
    t.uniqueId = uid;
    t.id = uid;
    t.name = QStringLiteral("T%1").arg(uid);
    t.start = start;
    t.durationMillis = durMillis;
    t.finish = Scheduler::addWork(start, durMillis);
    return t;
}

Relation link(int pred, int succ, int type = Relation::FinishToStart,
              qint64 lagMillis = 0, int lagFormat = Duration::Days)
{
    Relation r;
    r.uniqueId = pred * 100 + succ;
    r.predecessorTaskUid = pred;
    r.successorTaskUid = succ;
    r.type = type;
    r.lagMillis = lagMillis;
    r.lagFormat = lagFormat;
    return r;
}

} // namespace

class tst_scheduler : public QObject
{
    Q_OBJECT
private slots:
    void workingTimeMath();
    void finishToStartChain();
    void lagAndLead();
    void elapsedLag();
    void otherLinkTypes();
    void weekendSkipped();
    void constraints();
    void manualAndStartedUntouched();
    void cycleIsSafe();
    void reachability();
    void slackAndCriticalPath();
    void deadlineCapsLateFinish();
};

void tst_scheduler::workingTimeMath()
{
    const QDateTime mon8(kMon, QTime(8, 0));

    // A full working day from 08:00 ends at 17:00 (lunch 12:00-13:00 skipped).
    QCOMPARE(Scheduler::addWork(mon8, kDay), QDateTime(kMon, QTime(17, 0)));
    // Half a day ends at 13:00 + 0h -> 12:00 boundary belongs to the morning.
    QCOMPARE(Scheduler::addWork(mon8, 4 * kHour), QDateTime(kMon, QTime(12, 0)));
    // 5 working hours crosses lunch.
    QCOMPARE(Scheduler::addWork(mon8, 5 * kHour), QDateTime(kMon, QTime(14, 0)));
    // Starting work at Friday 17:00 rolls to Monday 08:00.
    QCOMPARE(Scheduler::nextWorkStart(QDateTime(kMon.addDays(4), QTime(17, 0))),
             QDateTime(kMon.addDays(7), QTime(8, 0)));
    // Backwards: one working day back from Monday 08:00 is Friday 08:00.
    QCOMPARE(Scheduler::addWork(mon8, -kDay), QDateTime(kMon.addDays(-3), QTime(8, 0)));
    // workBetween inverts addWork.
    QCOMPARE(Scheduler::workBetween(mon8, QDateTime(kMon.addDays(2), QTime(17, 0))), 3 * kDay);
}

void tst_scheduler::finishToStartChain()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), 2 * kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), 3 * kDay);
    p.relations << link(1, 2);

    Scheduler::reschedule(p);

    // T1 anchored: Mon-Tue. T2 starts Wed 08:00, finishes Fri 17:00.
    QCOMPARE(p.tasks[0].finish, QDateTime(kMon.addDays(1), QTime(17, 0)));
    QCOMPARE(p.tasks[1].start, QDateTime(kMon.addDays(2), QTime(8, 0)));
    QCOMPARE(p.tasks[1].finish, QDateTime(kMon.addDays(4), QTime(17, 0)));
}

void tst_scheduler::lagAndLead()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(3, QDateTime(kMon, QTime(8, 0)), kDay);
    p.relations << link(1, 2, Relation::FinishToStart, 2 * kDay)     // +2 days lag
                << link(1, 3, Relation::FinishToStart, -4 * kHour);  // half-day lead

    Scheduler::reschedule(p);

    // T1 finishes Mon 17:00; +2 working days of lag -> T2 starts Thu 08:00.
    QCOMPARE(p.tasks[1].start, QDateTime(kMon.addDays(3), QTime(8, 0)));
    // Lead of 4 working hours before Mon 17:00 -> Mon 13:00.
    QCOMPARE(p.tasks[2].start, QDateTime(kMon, QTime(13, 0)));
}

void tst_scheduler::elapsedLag()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon.addDays(4), QTime(8, 0)), kDay)   // Fri
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), kDay);
    // 2 elapsed days after Friday 17:00 = Sunday 17:00 -> next work Monday 08:00.
    p.relations << link(1, 2, Relation::FinishToStart,
                        2 * Duration::kMillisPerElapsedDay, Duration::ElapsedDays);

    Scheduler::reschedule(p);
    QCOMPARE(p.tasks[1].start, QDateTime(kMon.addDays(7), QTime(8, 0)));
}

void tst_scheduler::otherLinkTypes()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), 2 * kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), kDay)       // SS
            << makeTask(3, QDateTime(kMon, QTime(8, 0)), kDay)       // FF
            << makeTask(4, QDateTime(kMon, QTime(8, 0)), kDay);      // SF
    p.relations << link(1, 2, Relation::StartToStart)
                << link(1, 3, Relation::FinishToFinish)
                << link(1, 4, Relation::StartToFinish);

    Scheduler::reschedule(p);

    // SS: T2 starts with T1 (Mon 08:00).
    QCOMPARE(p.tasks[1].start, QDateTime(kMon, QTime(8, 0)));
    // FF: T3 finishes when T1 finishes (Tue 17:00), so starts Tue 08:00.
    QCOMPARE(p.tasks[2].finish, QDateTime(kMon.addDays(1), QTime(17, 0)));
    QCOMPARE(p.tasks[2].start, QDateTime(kMon.addDays(1), QTime(8, 0)));
    // SF: T4 finishes when T1 starts (Mon 08:00) -> works the previous Friday.
    // (Friday 17:00 and Monday 08:00 are the same instant in working time; the
    // forward pass normalises the finish to the working-period end.)
    QCOMPARE(p.tasks[3].start, QDateTime(kMon.addDays(-3), QTime(8, 0)));
    QCOMPARE(p.tasks[3].finish, QDateTime(kMon.addDays(-3), QTime(17, 0)));
}

void tst_scheduler::weekendSkipped()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon.addDays(3), QTime(8, 0)), 2 * kDay)   // Thu-Fri
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), 2 * kDay);
    p.relations << link(1, 2);

    Scheduler::reschedule(p);

    // T1 finishes Fri 17:00 -> T2 starts the following Monday.
    QCOMPARE(p.tasks[1].start, QDateTime(kMon.addDays(7), QTime(8, 0)));
    QCOMPARE(p.tasks[1].finish, QDateTime(kMon.addDays(8), QTime(17, 0)));
}

void tst_scheduler::constraints()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), kDay)    // SNET beyond dep
            << makeTask(3, QDateTime(kMon, QTime(8, 0)), kDay);   // MSO pins exactly
    p.relations << link(1, 2) << link(1, 3);

    p.tasks[1].constraintType = 4;   // Start No Earlier Than
    p.tasks[1].constraintDate = QDateTime(kMon.addDays(9), QTime(8, 0));
    p.tasks[2].constraintType = 2;   // Must Start On
    p.tasks[2].constraintDate = QDateTime(kMon.addDays(14), QTime(8, 0));

    Scheduler::reschedule(p);

    QCOMPARE(p.tasks[1].start, QDateTime(kMon.addDays(9), QTime(8, 0)));
    QCOMPARE(p.tasks[2].start, QDateTime(kMon.addDays(14), QTime(8, 0)));
}

void tst_scheduler::manualAndStartedUntouched()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), 2 * kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(3, QDateTime(kMon, QTime(8, 0)), kDay);
    p.tasks[1].manual = true;
    p.tasks[2].actualStart = QDateTime(kMon, QTime(8, 0));
    p.relations << link(1, 2) << link(1, 3);

    const QDateTime manualStart = p.tasks[1].start;
    const QDateTime startedStart = p.tasks[2].start;
    Scheduler::reschedule(p);

    QCOMPARE(p.tasks[1].start, manualStart);    // manual task not moved
    QCOMPARE(p.tasks[2].start, startedStart);   // started task not moved
}

void tst_scheduler::cycleIsSafe()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), kDay);
    p.relations << link(1, 2) << link(2, 1);   // A <-> B

    const QDateTime s1 = p.tasks[0].start;
    const QDateTime s2 = p.tasks[1].start;
    Scheduler::reschedule(p);   // must not hang or move the cycle's tasks

    QCOMPARE(p.tasks[0].start, s1);
    QCOMPARE(p.tasks[1].start, s2);
}

void tst_scheduler::reachability()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(3, QDateTime(kMon, QTime(8, 0)), kDay);
    p.relations << link(1, 2) << link(2, 3);

    QVERIFY(Scheduler::reachable(p, 1, 3));    // 1 -> 2 -> 3
    QVERIFY(!Scheduler::reachable(p, 3, 1));   // links are directed
}

void tst_scheduler::slackAndCriticalPath()
{
    // Two parallel chains between T1 and T4: T2 (3d) is critical, T3 (1d)
    // floats by 2 working days.
    //
    //          +--> T2 (3d) --+
    //   T1(1d)-+              +--> T4 (1d)
    //          +--> T3 (1d) --+
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), 3 * kDay)
            << makeTask(3, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(4, QDateTime(kMon, QTime(8, 0)), kDay);
    p.relations << link(1, 2) << link(1, 3) << link(2, 4) << link(3, 4);

    Scheduler::reschedule(p);
    Scheduler::computeSlack(p);

    const Task &t1 = p.tasks[0], &t2 = p.tasks[1], &t3 = p.tasks[2], &t4 = p.tasks[3];
    // Forward pass sanity: the project runs Monday..Friday.
    QCOMPARE(t4.finish, QDateTime(kMon.addDays(4), QTime(17, 0)));

    QVERIFY(t1.critical);
    QVERIFY(t2.critical);
    QVERIFY(t4.critical);
    QCOMPARE(t2.totalSlackMillis, qint64(0));

    QVERIFY(!t3.critical);
    QCOMPARE(t3.totalSlackMillis, 2 * kDay);
    QCOMPARE(t3.freeSlackMillis, 2 * kDay);
    // T3 can end as late as Friday 08:00 (== Thursday 17:00 in working time).
    QCOMPARE(t3.lateFinish, QDateTime(kMon.addDays(4), QTime(8, 0)));
    QCOMPARE(t3.lateStart, QDateTime(kMon.addDays(3), QTime(8, 0)));
}

void tst_scheduler::deadlineCapsLateFinish()
{
    Project p;
    p.tasks << makeTask(1, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(2, QDateTime(kMon, QTime(8, 0)), 3 * kDay)
            << makeTask(3, QDateTime(kMon, QTime(8, 0)), kDay)
            << makeTask(4, QDateTime(kMon, QTime(8, 0)), kDay);
    p.relations << link(1, 2) << link(1, 3) << link(2, 4) << link(3, 4);
    // T3 must be done by Wednesday 17:00: one floating day left.
    p.tasks[2].deadline = QDateTime(kMon.addDays(2), QTime(17, 0));

    Scheduler::reschedule(p);
    Scheduler::computeSlack(p);

    const Task &t3 = p.tasks[2];
    QCOMPARE(t3.lateFinish, p.tasks[2].deadline);
    QCOMPARE(t3.totalSlackMillis, kDay);
    QVERIFY(!t3.critical);

    // An impossible deadline (before the scheduled finish) turns it critical.
    p.tasks[2].deadline = QDateTime(kMon, QTime(17, 0));
    Scheduler::computeSlack(p);
    QVERIFY(p.tasks[2].totalSlackMillis < 0);
    QVERIFY(p.tasks[2].critical);
}

QTEST_APPLESS_MAIN(tst_scheduler)
#include "tst_scheduler.moc"
