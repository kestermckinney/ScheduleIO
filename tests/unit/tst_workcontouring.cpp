// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/taskscheduling.h"
#include "model/workcalendar.h"
#include "model/workcontouring.h"

#include <QTest>

namespace {
constexpr qint64 kHour = 3600LL * 1000LL;

schedule::Project contourProject()
{
    schedule::Project project;
    schedule::Task task;
    task.uniqueId = 1;
    task.start = QDateTime(QDate(2026, 8, 3), QTime(8, 0), Qt::UTC);
    task.durationMillis = 80 * kHour;
    task.finish = schedule::WorkCalendar().addWork(task.start, task.durationMillis);
    task.workMillis = 80 * kHour;
    project.tasks.append(task);

    schedule::Resource resource;
    resource.uniqueId = 2;
    resource.type = schedule::Resource::Type::Work;
    project.resources.append(resource);

    schedule::Assignment assignment;
    assignment.uniqueId = 10;
    assignment.taskUniqueId = 1;
    assignment.resourceUniqueId = 2;
    assignment.units = 1.0;
    assignment.workMillis = 80 * kHour;
    assignment.remainingWorkMillis = 80 * kHour;
    assignment.start = task.start;
    assignment.finish = task.finish;
    project.assignments.append(assignment);
    return project;
}
}

class TstWorkContouring : public QObject
{
    Q_OBJECT
private slots:
    void predefinedSegmentTables();
    void backAndFrontLoadedPreserveAggregate();
    void customContourPreservesManualBuckets();
};

void TstWorkContouring::predefinedSegmentTables()
{
    QCOMPARE(schedule::WorkContouring::segmentPercentages(
                 schedule::WorkContouring::BackLoaded),
             QVector<int>({10, 15, 25, 50, 50, 75, 75, 100, 100, 100}));
    QCOMPARE(schedule::WorkContouring::segmentPercentages(
                 schedule::WorkContouring::Bell),
             QVector<int>({10, 20, 40, 80, 100, 100, 80, 40, 20, 10}));
}

void TstWorkContouring::backAndFrontLoadedPreserveAggregate()
{
    schedule::Project project = contourProject();
    QVERIFY(schedule::TaskScheduling::setWorkContour(
        project, 10, schedule::WorkContouring::BackLoaded));
    const QDateTime firstDay(QDate(2026, 8, 3), QTime(0, 0), Qt::UTC);
    const QDateTime lastDay(QDate(2026, 8, 14), QTime(0, 0), Qt::UTC);
    const auto &back = project.assignments.first();
    QCOMPARE(back.workContour, int(schedule::WorkContouring::BackLoaded));
    QCOMPARE(back.timephasedWorkInPeriod(
                 schedule::TimephasedValue::RemainingWork,
                 back.start, back.finish), 80 * kHour);
    QVERIFY(back.timephasedWorkInPeriod(
                schedule::TimephasedValue::RemainingWork,
                firstDay, firstDay.addDays(1))
            < back.timephasedWorkInPeriod(
                schedule::TimephasedValue::RemainingWork,
                lastDay, lastDay.addDays(1)));

    QVERIFY(schedule::TaskScheduling::setWorkContour(
        project, 10, schedule::WorkContouring::FrontLoaded));
    const auto &front = project.assignments.first();
    QVERIFY(front.timephasedWorkInPeriod(
                schedule::TimephasedValue::RemainingWork,
                firstDay, firstDay.addDays(1))
            > front.timephasedWorkInPeriod(
                schedule::TimephasedValue::RemainingWork,
                lastDay, lastDay.addDays(1)));
    QCOMPARE(front.remainingWorkMillis, 80 * kHour);
}

void TstWorkContouring::customContourPreservesManualBuckets()
{
    schedule::Project project = contourProject();
    schedule::Assignment &assignment = project.assignments.first();
    const QDateTime from(QDate(2026, 8, 3), QTime(8, 0), Qt::UTC);
    const QDateTime to(QDate(2026, 8, 3), QTime(17, 0), Qt::UTC);
    QVERIFY(assignment.setTimephasedWorkInPeriod(
        schedule::TimephasedValue::RemainingWork, from, to, 12 * kHour));
    assignment.workContour = schedule::WorkContouring::Contoured;
    const auto before = assignment.timephasedValues;
    QVERIFY(schedule::WorkContouring::regenerate(project, 10));
    QCOMPARE(project.assignments.first().timephasedValues, before);
}

QTEST_MAIN(TstWorkContouring)
#include "tst_workcontouring.moc"
