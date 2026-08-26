// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/resourceleveling.h"
#include "model/scheduler.h"
#include "model/taskscheduling.h"

#include <QTest>

using schedule::Assignment;
using schedule::Project;
using schedule::Resource;
using schedule::ResourceLeveling;
using schedule::Scheduler;
using schedule::Task;
using schedule::TaskScheduling;
using schedule::TimephasedValue;

namespace {
constexpr qint64 kHour = 3600LL * 1000LL;

// One resource (uid 10, 100% max units) and `n` two-day tasks all starting Monday
// 2026-07-06 08:00, each fully assigned to it -> they pile up and overallocate.
Project overlappingTasks(int n)
{
    Project p;
    Resource r;
    r.uniqueId = 10;
    r.name = QStringLiteral("Ann");
    r.maxUnits = 1.0;
    p.resources.append(r);

    for (int i = 0; i < n; ++i) {
        Task t;
        t.uniqueId = i + 1;
        t.id = i + 1;
        t.name = QStringLiteral("T%1").arg(i + 1);
        t.start = QDateTime(QDate(2026, 7, 6), QTime(8, 0));
        t.durationMillis = 16 * kHour;   // 2 working days
        t.priority = 500;
        p.tasks.append(t);

        Assignment a;
        a.uniqueId = 100 + i;
        a.taskUniqueId = t.uniqueId;
        a.resourceUniqueId = 10;
        a.units = 1.0;
        a.workMillis = 16 * kHour;
        p.assignments.append(a);
    }
    for (const Task &t : p.tasks)
        TaskScheduling::syncTask(p, t.uniqueId);
    Scheduler::reschedule(p);
    return p;
}

Task *task(Project &p, int uid)
{
    for (Task &t : p.tasks)
        if (t.uniqueId == uid) return &t;
    return nullptr;
}
} // namespace

class TstResourceLeveling : public QObject
{
    Q_OBJECT
private slots:
    void detectsOverallocation();
    void noFalsePositiveWhenSerial();
    void levelResolvesTwoTasks();
    void levelResolvesThreeTasks();
    void levelKeepsHigherPriority();
    void clearLevelingRestores();
    void timephasedWorkOverridesFlatDistribution();
    void materialIsNotCapacityLeveled();
    void availabilityTableChangesCapacityByDate();
    void levelingMovesWorkIntoAvailablePeriod();
    void idOnlyOrderOverridesPriority();
    void selectedScopeLimitsMovableTasks();
    void withinSlackResolvesOnlyFeasibleConflict();
    void withinSlackLeavesCriticalConflictUnresolved();
    void selectedScopeSkipsEarlierUnselectedConflict();
};

void TstResourceLeveling::detectsOverallocation()
{
    Project p = overlappingTasks(2);
    QVERIFY(ResourceLeveling::isOverallocated(p, 10));
    const auto over = ResourceLeveling::overallocations(p);
    QCOMPARE(over.size(), 1);
    QCOMPARE(over.first().resourceUniqueId, 10);
    QVERIFY(over.first().peakUnits > 1.5);   // two 100% assignments at once
}

void TstResourceLeveling::noFalsePositiveWhenSerial()
{
    Project p = overlappingTasks(2);
    // Chain T2 after T1 (finish-to-start): they no longer overlap.
    schedule::Relation rel;
    rel.uniqueId = 1;
    rel.predecessorTaskUid = 1;
    rel.successorTaskUid = 2;
    rel.type = schedule::Relation::FinishToStart;
    p.relations.append(rel);
    Scheduler::reschedule(p);
    QVERIFY(!ResourceLeveling::isOverallocated(p, 10));
}

void TstResourceLeveling::levelResolvesTwoTasks()
{
    Project p = overlappingTasks(2);
    const int delayed = ResourceLeveling::level(p);
    QCOMPARE(delayed, 1);
    QVERIFY(!ResourceLeveling::isOverallocated(p, 10));
    // The delayed task now starts no earlier than the other finishes.
    const Task *t1 = task(p, 1);
    const Task *t2 = task(p, 2);
    QVERIFY(t1->finish <= t2->start || t2->finish <= t1->start);
}

void TstResourceLeveling::levelResolvesThreeTasks()
{
    Project p = overlappingTasks(3);
    QVERIFY(ResourceLeveling::isOverallocated(p, 10));
    ResourceLeveling::level(p);
    QVERIFY(!ResourceLeveling::isOverallocated(p, 10));
}

void TstResourceLeveling::levelKeepsHigherPriority()
{
    Project p = overlappingTasks(2);
    task(p, 1)->priority = 100;    // low priority -> should be the one delayed
    task(p, 2)->priority = 900;    // high priority -> should keep its early slot
    Scheduler::reschedule(p);
    ResourceLeveling::level(p);
    QVERIFY(!ResourceLeveling::isOverallocated(p, 10));
    // The high-priority task keeps the original Monday start; the low one is pushed out.
    QCOMPARE(task(p, 2)->start, QDateTime(QDate(2026, 7, 6), QTime(8, 0)));
    QVERIFY(task(p, 1)->start > task(p, 2)->start);
}

void TstResourceLeveling::clearLevelingRestores()
{
    Project p = overlappingTasks(2);
    ResourceLeveling::level(p);
    QVERIFY(task(p, 1)->levelingDelayMillis > 0 || task(p, 2)->levelingDelayMillis > 0);
    ResourceLeveling::clearLeveling(p);
    QCOMPARE(task(p, 1)->levelingDelayMillis, qint64(0));
    QCOMPARE(task(p, 2)->levelingDelayMillis, qint64(0));
    QVERIFY(ResourceLeveling::isOverallocated(p, 10));   // back to overlapping
}

void TstResourceLeveling::timephasedWorkOverridesFlatDistribution()
{
    Project p = overlappingTasks(1);
    Assignment &a = p.assignments.first();
    a.workMillis = 16 * kHour;

    TimephasedValue first;
    first.type = TimephasedValue::ActualWork;
    first.start = QDateTime(QDate(2026, 7, 6), QTime(0, 0));
    first.finish = QDateTime(QDate(2026, 7, 7), QTime(0, 0));
    first.value = QStringLiteral("PT6H0M0S");
    a.timephasedValues.append(first);

    TimephasedValue second = first;
    second.type = TimephasedValue::RemainingWork;
    second.start = QDateTime(QDate(2026, 7, 7), QTime(0, 0));
    second.finish = QDateTime(QDate(2026, 7, 8), QTime(0, 0));
    second.value = QStringLiteral("PT10H0M0S");
    a.timephasedValues.append(second);

    QCOMPARE(ResourceLeveling::workInPeriod(
                 p, a, first.start, first.finish), 6 * kHour);
    QCOMPARE(ResourceLeveling::workInPeriod(
                 p, a, first.start, second.finish), 16 * kHour);
}

void TstResourceLeveling::materialIsNotCapacityLeveled()
{
    Project p = overlappingTasks(2);
    p.resources.first().type = Resource::Type::Material;
    p.resources.first().materialLabel = QStringLiteral("tons");
    QVERIFY(!ResourceLeveling::isOverallocated(p, 10));
    QVERIFY(ResourceLeveling::overallocations(p).isEmpty());
}

void TstResourceLeveling::availabilityTableChangesCapacityByDate()
{
    Project p = overlappingTasks(1);
    schedule::AvailabilityPeriod halfTime;
    halfTime.startDate = QDateTime(QDate(2026, 7, 6), QTime(0, 0));
    halfTime.endDate = QDateTime(QDate(2026, 7, 6), QTime(0, 0));
    halfTime.units = 0.5;
    schedule::AvailabilityPeriod fullTime;
    fullTime.startDate = QDateTime(QDate(2026, 7, 7), QTime(0, 0));
    fullTime.units = 1.0;
    p.resources.first().availabilityTable = { halfTime, fullTime };

    const auto over = ResourceLeveling::overallocations(p);
    QCOMPARE(over.size(), 1);
    QCOMPARE(over.first().start, QDateTime(QDate(2026, 7, 6), QTime(8, 0)));
    QCOMPARE(over.first().finish, QDateTime(QDate(2026, 7, 7), QTime(0, 0)));
    QCOMPARE(over.first().maxUnits, 0.5);
}

void TstResourceLeveling::levelingMovesWorkIntoAvailablePeriod()
{
    Project p = overlappingTasks(1);
    TaskScheduling::setDuration(p, 1, 8 * kHour);
    schedule::AvailabilityPeriod available;
    available.startDate = QDateTime(QDate(2026, 7, 7), QTime(0, 0));
    available.units = 1.0;
    p.resources.first().availabilityTable = { available };

    QVERIFY(ResourceLeveling::isOverallocated(p, 10));
    QCOMPARE(ResourceLeveling::level(p), 1);
    QCOMPARE(task(p, 1)->start, QDateTime(QDate(2026, 7, 7), QTime(8, 0)));
    QVERIFY(!ResourceLeveling::isOverallocated(p, 10));
}

void TstResourceLeveling::idOnlyOrderOverridesPriority()
{
    Project p = overlappingTasks(2);
    task(p, 1)->priority = 100;
    task(p, 2)->priority = 900;
    Scheduler::reschedule(p);

    ResourceLeveling::Options options;
    options.order = ResourceLeveling::Order::IdOnly;
    QCOMPARE(ResourceLeveling::level(p, options), 1);
    QCOMPARE(task(p, 1)->start, QDateTime(QDate(2026, 7, 6), QTime(8, 0)));
    QVERIFY(task(p, 2)->start > task(p, 1)->start);
}

void TstResourceLeveling::selectedScopeLimitsMovableTasks()
{
    Project p = overlappingTasks(2);
    task(p, 1)->priority = 900;
    task(p, 2)->priority = 100;
    Scheduler::reschedule(p);

    ResourceLeveling::Options options;
    options.taskUniqueIds.insert(1);
    QCOMPARE(ResourceLeveling::level(p, options), 1);
    QVERIFY(task(p, 1)->start > task(p, 2)->start);
}

void TstResourceLeveling::withinSlackResolvesOnlyFeasibleConflict()
{
    Project p = overlappingTasks(2);
    // An unassigned three-day task holds the project finish, giving both one-day
    // resource tasks enough float for one of them to move without extending it.
    Task driver;
    driver.uniqueId = 3;
    driver.id = 3;
    driver.name = QStringLiteral("Driver");
    driver.start = QDateTime(QDate(2026, 7, 6), QTime(8, 0));
    driver.durationMillis = 24 * kHour;
    p.tasks.append(driver);
    TaskScheduling::setDuration(p, 1, 8 * kHour);
    TaskScheduling::setDuration(p, 2, 8 * kHour);
    Scheduler::reschedule(p);
    Scheduler::computeSlack(p);
    QVERIFY(task(p, 1)->totalSlackMillis >= 8 * kHour);

    ResourceLeveling::Options options;
    options.levelOnlyWithinAvailableSlack = true;
    QCOMPARE(ResourceLeveling::level(p, options), 1);
    QVERIFY(!ResourceLeveling::isOverallocated(p, 10));
    QCOMPARE(p.tasks.at(2).finish, QDateTime(QDate(2026, 7, 8), QTime(17, 0)));
}

void TstResourceLeveling::withinSlackLeavesCriticalConflictUnresolved()
{
    Project p = overlappingTasks(2);
    Scheduler::computeSlack(p);
    QCOMPARE(task(p, 1)->totalSlackMillis, qint64(0));
    QCOMPARE(task(p, 2)->totalSlackMillis, qint64(0));

    ResourceLeveling::Options options;
    options.levelOnlyWithinAvailableSlack = true;
    QCOMPARE(ResourceLeveling::level(p, options), 0);
    QVERIFY(ResourceLeveling::isOverallocated(p, 10));
}

void TstResourceLeveling::selectedScopeSkipsEarlierUnselectedConflict()
{
    Project p = overlappingTasks(4);
    for (int uid = 1; uid <= 4; ++uid)
        TaskScheduling::setDuration(p, uid, 8 * kHour);
    task(p, 3)->start = QDateTime(QDate(2026, 7, 7), QTime(8, 0));
    task(p, 4)->start = QDateTime(QDate(2026, 7, 7), QTime(8, 0));
    TaskScheduling::syncTask(p, 3);
    TaskScheduling::syncTask(p, 4);
    Scheduler::reschedule(p);

    ResourceLeveling::Options options;
    options.taskUniqueIds.insert(4);
    QCOMPARE(ResourceLeveling::level(p, options), 1);
    QCOMPARE(task(p, 1)->levelingDelayMillis, qint64(0));
    QCOMPARE(task(p, 2)->levelingDelayMillis, qint64(0));
    QCOMPARE(task(p, 3)->levelingDelayMillis, qint64(0));
    QVERIFY(task(p, 4)->levelingDelayMillis > 0);
    // The earlier unselected conflict remains, but it no longer starves the selected one.
    QVERIFY(ResourceLeveling::isOverallocated(p, 10));
}

QTEST_MAIN(TstResourceLeveling)
#include "tst_resourceleveling.moc"
