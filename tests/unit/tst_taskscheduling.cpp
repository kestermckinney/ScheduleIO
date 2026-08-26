// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/taskscheduling.h"
#include "model/calendar.h"
#include "model/duration.h"
#include "mppio.h"

#include <QFile>
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
    void materialConsumptionDoesNotCreateLaborOrDuration();
    void resourceCalendarMovesAssignmentAndTask();
    void ignoreResourceCalendarUsesTaskCalendarOnly();
    void calendarOracleArtifactRoundTrips();
    void elapsedDurationIgnoresResourceCalendar();
};

void TstTaskScheduling::elapsedDurationIgnoresResourceCalendar()
{
    Project p = makeProject(0, false); // Fixed Units, one assigned work resource
    task(p).start = QDateTime(QDate(2026, 7, 10), QTime(17, 0)); // Friday
    task(p).durationFormat = schedule::Duration::ElapsedDays;

    TaskScheduling::setDuration(p, 1, 2 * schedule::Duration::kMillisPerElapsedDay);

    QCOMPARE(task(p).durationFormat, int(schedule::Duration::ElapsedDays));
    QCOMPARE(task(p).finish, QDateTime(QDate(2026, 7, 12), QTime(17, 0))); // Sunday
    QCOMPARE(assn(p, 100)->start, task(p).start);
    QCOMPARE(assn(p, 100)->finish, task(p).finish);
}

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

void TstTaskScheduling::materialConsumptionDoesNotCreateLaborOrDuration()
{
    Project p = makeProject(0, false);
    Resource material;
    material.uniqueId = 12;
    material.name = QStringLiteral("Concrete");
    material.type = Resource::Type::Material;
    material.materialLabel = QStringLiteral("tons");
    p.resources.append(material);

    const int materialUid = TaskScheduling::addAssignment(p, 1, 12, 1.0);
    QVERIFY(materialUid > 0);
    QCOMPARE(assn(p, materialUid)->workMillis, kHour);
    QCOMPARE(TaskScheduling::taskWork(p, 1), 40 * kHour);
    QCOMPARE(task(p).durationMillis, 40 * kHour);

    TaskScheduling::setAssignmentWork(p, materialUid, qint64(12.5 * kHour));
    QCOMPARE(assn(p, materialUid)->workMillis, qint64(12.5 * kHour));
    QCOMPARE(TaskScheduling::taskWork(p, 1), 40 * kHour);
    QCOMPARE(task(p).workMillis, 40 * kHour);
    QCOMPARE(task(p).durationMillis, 40 * kHour);

    TaskScheduling::setDuration(p, 1, 24 * kHour);
    QCOMPARE(assn(p, materialUid)->workMillis, qint64(12.5 * kHour));
    QCOMPARE(assn(p, 100)->workMillis, 24 * kHour);
    QCOMPARE(TaskScheduling::taskWork(p, 1), 24 * kHour);
}

void TstTaskScheduling::resourceCalendarMovesAssignmentAndTask()
{
    Project p = makeProject(0, false);
    p.calendars = schedule::Calendar::microsoftDefaults();
    p.calendarUniqueId = 1;
    schedule::Calendar resourceCalendar;
    resourceCalendar.uniqueId = 4;
    resourceCalendar.name = QStringLiteral("Ann");
    resourceCalendar.baseCalendarUniqueId = 1;
    schedule::CalendarException vacation;
    vacation.fromDate = QDate(2026, 7, 6);
    vacation.toDate = QDate(2026, 7, 6);
    vacation.working = false;
    resourceCalendar.exceptions.append(vacation);
    p.calendars.append(resourceCalendar);
    p.resources[0].calendarUniqueId = 4;
    p.assignments[0].workMillis = 8 * kHour;
    p.tasks[0].durationMillis = 8 * kHour;

    TaskScheduling::syncTask(p, 1);
    QCOMPARE(task(p).start, QDateTime(QDate(2026, 7, 7), QTime(8, 0)));
    QCOMPARE(task(p).finish, QDateTime(QDate(2026, 7, 7), QTime(17, 0)));
    QCOMPARE(assn(p, 100)->start, task(p).start);
    QCOMPARE(assn(p, 100)->finish, task(p).finish);
}

void TstTaskScheduling::ignoreResourceCalendarUsesTaskCalendarOnly()
{
    Project p = makeProject(0, false);
    p.calendars = schedule::Calendar::microsoftDefaults();
    p.calendarUniqueId = 1;
    p.tasks[0].calendarUniqueId = 2; // 24 Hours
    p.tasks[0].ignoreResourceCalendar = true;
    p.tasks[0].start = QDateTime(QDate(2026, 7, 6), QTime(0, 0));
    p.tasks[0].durationMillis = 8 * kHour;
    p.assignments[0].workMillis = 8 * kHour;
    p.resources[0].calendarUniqueId = 1; // Standard would not start until 08:00

    TaskScheduling::syncTask(p, 1);
    QCOMPARE(task(p).start, QDateTime(QDate(2026, 7, 6), QTime(0, 0)));
    QCOMPARE(task(p).finish, QDateTime(QDate(2026, 7, 6), QTime(8, 0)));
}

void TstTaskScheduling::calendarOracleArtifactRoundTrips()
{
    Project p;
    p.formatVersion = Project::FormatVersion::Mpp14;
    p.title = QStringLiteral("Resource Calendar Oracle");
    p.calendars = schedule::Calendar::microsoftDefaults();
    p.calendarUniqueId = 1;
    p.startDate = QDateTime(QDate(2026, 7, 6), QTime(0, 0));

    schedule::Calendar annCalendar;
    annCalendar.uniqueId = 4;
    annCalendar.name = QStringLiteral("Ann");
    annCalendar.baseCalendarUniqueId = 1;
    schedule::CalendarException vacation;
    vacation.name = QStringLiteral("Monday vacation");
    vacation.fromDate = QDate(2026, 7, 6);
    vacation.toDate = QDate(2026, 7, 6);
    vacation.working = false;
    annCalendar.exceptions.append(vacation);
    p.calendars.append(annCalendar);

    Resource ann;
    ann.uniqueId = 10;
    ann.id = 1;
    ann.name = QStringLiteral("Ann");
    ann.calendarUniqueId = 4;
    schedule::AvailabilityPeriod availability;
    // Wall clock, the spec every model date carries: these are compared against values
    // that have been through the .mpp codec below.
    availability.startDate = QDateTime(QDate(2026, 7, 7), QTime(0, 0));
    availability.endDate = QDateTime(QDate(2026, 7, 31), QTime(23, 59));
    availability.units = 1.0;
    ann.availabilityTable = { availability };
    p.resources.append(ann);

    Task intersected;
    intersected.uniqueId = 1;
    intersected.id = 1;
    intersected.name = QStringLiteral("Calendar intersection");
    intersected.start = QDateTime(QDate(2026, 7, 6), QTime(8, 0));
    intersected.durationMillis = 8 * kHour;
    p.tasks.append(intersected);
    Assignment a1;
    a1.uniqueId = 100;
    a1.taskUniqueId = 1;
    a1.resourceUniqueId = 10;
    a1.units = 1.0;
    a1.workMillis = 8 * kHour;
    p.assignments.append(a1);
    TaskScheduling::syncTask(p, 1);
    QCOMPARE(p.tasks[0].start, QDateTime(QDate(2026, 7, 7), QTime(8, 0)));

    Resource ben;
    ben.uniqueId = 11;
    ben.id = 2;
    ben.name = QStringLiteral("Ben");
    ben.calendarUniqueId = 1;
    p.resources.append(ben);
    Task ignored;
    ignored.uniqueId = 2;
    ignored.id = 2;
    ignored.name = QStringLiteral("Ignore resource calendar");
    // Start at 08:00 rather than midnight: Project's task fixed-data timestamp
    // uses midnight as an NA sentinel in this row shape. The 24 Hours calendar
    // is still proven by the uninterrupted 08:00-16:00 eight-hour span.
    ignored.start = QDateTime(QDate(2026, 7, 6), QTime(8, 0));
    ignored.durationMillis = 8 * kHour;
    ignored.calendarUniqueId = 2;
    ignored.ignoreResourceCalendar = true;
    p.tasks.append(ignored);
    Assignment a2;
    a2.uniqueId = 101;
    a2.taskUniqueId = 2;
    a2.resourceUniqueId = 11;
    a2.units = 1.0;
    a2.workMillis = 8 * kHour;
    p.assignments.append(a2);
    TaskScheduling::syncTask(p, 2);
    QCOMPARE(p.tasks[1].finish, QDateTime(QDate(2026, 7, 6), QTime(16, 0)));

    MppIO writer;
    writer.setProject(p);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));
    MppIO reader;
    QVERIFY2(reader.openFromData(bytes), qPrintable(reader.errorString()));
    QCOMPARE(reader.project().tasks.at(0).start.date(), p.tasks.at(0).start.date());
    QCOMPARE(reader.project().tasks.at(0).start.time(), p.tasks.at(0).start.time());
    QVERIFY(reader.project().tasks.at(1).ignoreResourceCalendar);
    QCOMPARE(reader.project().resources.at(0).availabilityTable, ann.availabilityTable);

    const QString artifactPath = qEnvironmentVariable("SCHEDULEIO_CALENDAR_ORACLE_MPP");
    if (!artifactPath.isEmpty()) {
        QFile artifact(artifactPath);
        QVERIFY(artifact.open(QIODevice::WriteOnly));
        QCOMPARE(artifact.write(bytes), bytes.size());
    }
}

QTEST_MAIN(TstTaskScheduling)
#include "tst_taskscheduling.moc"
