// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/materialcosting.h"
#include "model/taskscheduling.h"

#include <QTest>

namespace {
constexpr qint64 kHour = 3600LL * 1000LL;

schedule::Project materialProject()
{
    schedule::Project project;
    schedule::Task task;
    task.uniqueId = 1;
    task.name = QStringLiteral("Pour concrete");
    task.start = QDateTime(QDate(2026, 8, 3), QTime(8, 0), Qt::UTC);
    task.finish = QDateTime(QDate(2026, 8, 7), QTime(17, 0), Qt::UTC);
    task.durationMillis = 40 * kHour;
    project.tasks.append(task);

    schedule::Resource resource;
    resource.uniqueId = 2;
    resource.name = QStringLiteral("Concrete");
    resource.type = schedule::Resource::Type::Material;
    resource.materialLabel = QStringLiteral("tons");
    schedule::CostRate rate;
    rate.table = 0;
    rate.standardRate = 100.0;
    rate.costPerUse = 350.0;
    resource.costRates.append(rate);
    project.resources.append(resource);

    schedule::Assignment assignment;
    assignment.uniqueId = 10;
    assignment.taskUniqueId = 1;
    assignment.resourceUniqueId = 2;
    assignment.units = 40.0;
    assignment.workMillis = 40 * kHour;
    assignment.remainingWorkMillis = 40 * kHour;
    assignment.start = task.start;
    assignment.finish = task.finish;
    project.assignments.append(assignment);
    return project;
}
}

class TstMaterialCosting : public QObject
{
    Q_OBJECT
private slots:
    void fixedQuantityRateAndPerUseRollUp();
    void actualConsumptionMovesPerUseToActualCost();
    void variableConsumptionFollowsTaskDuration();
};

void TstMaterialCosting::fixedQuantityRateAndPerUseRollUp()
{
    schedule::Project project = materialProject();
    schedule::MaterialCosting::recalculate(project);
    const schedule::Assignment &assignment = project.assignments.first();
    QCOMPARE(assignment.remainingCost, 4350.0);
    QCOMPARE(assignment.actualCost, 0.0);
    QCOMPARE(assignment.cost, 4350.0);
    QCOMPARE(project.resources.first().cost, 4350.0);
    QCOMPARE(project.tasks.first().cost, 4350.0);
}

void TstMaterialCosting::actualConsumptionMovesPerUseToActualCost()
{
    schedule::Project project = materialProject();
    schedule::Assignment &assignment = project.assignments.first();
    const QDateTime first(QDate(2026, 8, 3), QTime(0, 0), Qt::UTC);
    const QDateTime second(QDate(2026, 8, 4), QTime(0, 0), Qt::UTC);
    const QDateTime third(QDate(2026, 8, 5), QTime(0, 0), Qt::UTC);
    QVERIFY(assignment.setTimephasedMaterialInPeriod(
        schedule::TimephasedValue::ActualWork, first, second, 10.0));
    QVERIFY(assignment.setTimephasedMaterialInPeriod(
        schedule::TimephasedValue::RemainingWork, second, third, 30.0));
    schedule::MaterialCosting::recalculate(project);
    QCOMPARE(assignment.actualCost, 1350.0);
    QCOMPARE(assignment.remainingCost, 3000.0);
    QCOMPARE(assignment.cost, 4350.0);
}

void TstMaterialCosting::variableConsumptionFollowsTaskDuration()
{
    schedule::Project project = materialProject();
    schedule::TaskScheduling::setMaterialRate(project, 10, 2.0, 3, 0);
    QCOMPARE(project.assignments.first().variableRateUnits, 3);
    QCOMPARE(project.assignments.first().workMillis, 10 * kHour);
    QCOMPARE(project.assignments.first().cost, 1350.0);

    schedule::TaskScheduling::setDuration(project, 1, 24 * kHour);
    QCOMPARE(project.assignments.first().workMillis, 6 * kHour);
    QCOMPARE(project.assignments.first().cost, 950.0);
    QCOMPARE(project.tasks.first().durationMillis, 24 * kHour);
    QCOMPARE(project.tasks.first().workMillis, qint64(0));
}

QTEST_MAIN(TstMaterialCosting)
#include "tst_materialcosting.moc"
