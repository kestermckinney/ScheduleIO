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

QTEST_MAIN(TstResourceLeveling)
#include "tst_resourceleveling.moc"
