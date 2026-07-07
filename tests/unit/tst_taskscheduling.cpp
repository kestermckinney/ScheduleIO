// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/taskscheduling.h"

#include <QTest>

using schedule::Assignment;
using schedule::Project;
using schedule::Resource;
using schedule::Task;
using schedule::TaskScheduling;

namespace {

constexpr qint64 kHour = 3600LL * 1000LL;

// One leaf task (uid 1) starting Monday 2026-07-06 08:00 with a 40h span,
// resource uid 10 assigned 100% (40h of work), resource 11 unassigned.
Project makeProject(int taskType, bool effortDriven)
{
    Project p;
    Task t;
    t.uniqueId = 1;
    t.name = QStringLiteral("Build");
    t.start = QDateTime(QDate(2026, 7, 6), QTime(8, 0));
    t.durationMillis = 40 * kHour;
    t.taskType = taskType;
    t.effortDriven = effortDriven || taskType == 2;
    p.tasks.append(t);

    Resource r1;
    r1.uniqueId = 10;
    r1.name = QStringLiteral("Ann");
    p.resources.append(r1);
    Resource r2;
    r2.uniqueId = 11;
    r2.name = QStringLiteral("Bob");
    p.resources.append(r2);

    Assignment a;
    a.uniqueId = 100;
    a.taskUniqueId = 1;
    a.resourceUniqueId = 10;
    a.units = 1.0;
    a.workMillis = 40 * kHour;
    p.assignments.append(a);

    TaskScheduling::syncTask(p, 1);
    return p;
}

Task &task(Project &p) { return p.tasks[0]; }

Assignment *assn(Project &p, int uid)
{
    for (Assignment &a : p.assignments)
        if (a.uniqueId == uid)
            return &a;
    return nullptr;
}

} // namespace

class TstTaskScheduling : public QObject
{
    Q_OBJECT
private slots:
    void baselineSync();
    void fixedUnits_durationEdit_changesWork();
    void fixedUnits_workEdit_changesDuration();
    void fixedUnits_unitsEdit_changesDuration();
    void fixedDuration_workEdit_changesUnits();
    void fixedDuration_unitsEdit_changesWork();
    void fixedWork_durationEdit_changesUnits();
    void effortDriven_add_splitsWork();
    void nonEffortDriven_add_addsWork();
    void effortDriven_remove_redistributes();
    void addToEmptyTask_bringsOwnWork();
    void workOnResourcelessTask_holdsOnTask();
    void assignAfterWork_inheritsTaskWork();
};

void TstTaskScheduling::baselineSync()
{
    Project p = makeProject(0, false);
    // 40h at 100% on the Standard week: Monday 08:00 -> Friday 17:00.
    QCOMPARE(task(p).finish, QDateTime(QDate(2026, 7, 10), QTime(17, 0)));
    QCOMPARE(assn(p, 100)->start, task(p).start);
    QCOMPARE(assn(p, 100)->finish, task(p).finish);
    QCOMPARE(TaskScheduling::taskWork(p, 1), 40 * kHour);
}

void TstTaskScheduling::fixedUnits_durationEdit_changesWork()
{
    Project p = makeProject(0, false);
    TaskScheduling::setDuration(p, 1, 24 * kHour);   // 3 days
    QCOMPARE(assn(p, 100)->workMillis, 24 * kHour);  // work followed
    QCOMPARE(assn(p, 100)->units, 1.0);              // units held
    QCOMPARE(task(p).finish, QDateTime(QDate(2026, 7, 8), QTime(17, 0)));
}

void TstTaskScheduling::fixedUnits_workEdit_changesDuration()
{
    Project p = makeProject(0, false);
    TaskScheduling::setWork(p, 1, 16 * kHour);       // 2 days of work
    QCOMPARE(assn(p, 100)->units, 1.0);              // units held
    QCOMPARE(task(p).durationMillis, 16 * kHour);    // duration followed
    QCOMPARE(task(p).finish, QDateTime(QDate(2026, 7, 7), QTime(17, 0)));
}

void TstTaskScheduling::fixedUnits_unitsEdit_changesDuration()
{
    Project p = makeProject(0, false);
    TaskScheduling::setAssignmentUnits(p, 100, 2.0); // double the effort
    QCOMPARE(assn(p, 100)->workMillis, 40 * kHour);  // work held
    QCOMPARE(task(p).durationMillis, 20 * kHour);    // span halved
}

void TstTaskScheduling::fixedDuration_workEdit_changesUnits()
{
    Project p = makeProject(1, false);
    TaskScheduling::setWork(p, 1, 20 * kHour);
    QCOMPARE(task(p).durationMillis, 40 * kHour);    // span held
    QCOMPARE(assn(p, 100)->units, 0.5);              // units absorbed it
}

void TstTaskScheduling::fixedDuration_unitsEdit_changesWork()
{
    Project p = makeProject(1, false);
    TaskScheduling::setAssignmentUnits(p, 100, 0.5);
    QCOMPARE(task(p).durationMillis, 40 * kHour);    // span held
    QCOMPARE(assn(p, 100)->workMillis, 20 * kHour);  // work followed
}

void TstTaskScheduling::fixedWork_durationEdit_changesUnits()
{
    Project p = makeProject(2, false);
    TaskScheduling::setDuration(p, 1, 80 * kHour);   // double the span
    QCOMPARE(assn(p, 100)->workMillis, 40 * kHour);  // work held (fixed!)
    QCOMPARE(assn(p, 100)->units, 0.5);              // units halved
    QCOMPARE(task(p).durationMillis, 80 * kHour);
}

void TstTaskScheduling::effortDriven_add_splitsWork()
{
    Project p = makeProject(0, true);   // Fixed Units, effort-driven
    const int newUid = TaskScheduling::addAssignment(p, 1, 11, 1.0);
    QVERIFY(newUid > 0);
    QCOMPARE(assn(p, 100)->workMillis, 20 * kHour);      // half each
    QCOMPARE(assn(p, newUid)->workMillis, 20 * kHour);
    QCOMPARE(TaskScheduling::taskWork(p, 1), 40 * kHour);   // total held
    QCOMPARE(task(p).durationMillis, 20 * kHour);        // task shortens
}

void TstTaskScheduling::nonEffortDriven_add_addsWork()
{
    Project p = makeProject(0, false);
    const int newUid = TaskScheduling::addAssignment(p, 1, 11, 1.0);
    QVERIFY(newUid > 0);
    QCOMPARE(assn(p, 100)->workMillis, 40 * kHour);      // untouched
    QCOMPARE(assn(p, newUid)->workMillis, 40 * kHour);   // brings its own
    QCOMPARE(TaskScheduling::taskWork(p, 1), 80 * kHour);
    QCOMPARE(task(p).durationMillis, 40 * kHour);        // span unchanged
}

void TstTaskScheduling::effortDriven_remove_redistributes()
{
    Project p = makeProject(0, true);
    const int newUid = TaskScheduling::addAssignment(p, 1, 11, 1.0);
    TaskScheduling::removeAssignment(p, newUid);
    QCOMPARE(assn(p, 100)->workMillis, 40 * kHour);      // Ann has it all again
    QCOMPARE(task(p).durationMillis, 40 * kHour);
}

void TstTaskScheduling::addToEmptyTask_bringsOwnWork()
{
    Project p = makeProject(0, true);
    TaskScheduling::removeAssignment(p, 100);
    QCOMPARE(TaskScheduling::taskWork(p, 1), qint64(0));
    const int newUid = TaskScheduling::addAssignment(p, 1, 11, 1.0);
    // First resource on an effort-driven task still takes duration x units.
    QCOMPARE(assn(p, newUid)->workMillis, 40 * kHour);
    QCOMPARE(task(p).durationMillis, 40 * kHour);
}

void TstTaskScheduling::workOnResourcelessTask_holdsOnTask()
{
    // A brand-new task with no resources: entering Work must stick (it used to
    // be silently rejected) and, for a Fixed Units task, drive the duration.
    Project p = makeProject(0, false);
    TaskScheduling::removeAssignment(p, 100);
    QCOMPARE(TaskScheduling::taskWork(p, 1), qint64(0));
    TaskScheduling::setWork(p, 1, 16 * kHour);
    QCOMPARE(TaskScheduling::taskWork(p, 1), 16 * kHour);   // held on the task
    QCOMPARE(task(p).workMillis, 16 * kHour);
    QCOMPARE(task(p).durationMillis, 16 * kHour);           // duration followed
}

void TstTaskScheduling::assignAfterWork_inheritsTaskWork()
{
    // Work entered before any resource is assigned; the first resource assigned
    // at 100% then picks up that work.
    Project p = makeProject(0, false);
    TaskScheduling::removeAssignment(p, 100);
    TaskScheduling::setWork(p, 1, 16 * kHour);
    const int newUid = TaskScheduling::addAssignment(p, 1, 11, 1.0);
    QCOMPARE(assn(p, newUid)->workMillis, 16 * kHour);
    QCOMPARE(TaskScheduling::taskWork(p, 1), 16 * kHour);
    QCOMPARE(task(p).durationMillis, 16 * kHour);
}

QTEST_MAIN(TstTaskScheduling)
#include "tst_taskscheduling.moc"
