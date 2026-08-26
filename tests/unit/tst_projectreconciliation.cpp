// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/projectreconciliation.h"
#include "mppio.h"

#include <QTest>
#include <QFile>

using namespace schedule;

namespace {
constexpr qint64 kHour = 3600LL * 1000LL;
const QDateTime kStart(QDate(2026, 8, 3), QTime(8, 0));

TimephasedValue bucket(int type, int day, qint64 work)
{
    TimephasedValue value;
    value.type = type;
    value.start = kStart.addDays(day);
    value.finish = value.start.addSecs(8 * 3600);
    value.value = QStringLiteral("PT%1H0M0S").arg(work / kHour);
    return value;
}

Project workProject()
{
    Project project;
    Task task;
    task.uniqueId = 1;
    task.id = 1;
    task.name = QStringLiteral("Build");
    task.start = kStart;
    task.finish = kStart.addDays(1).addSecs(9 * 3600);
    task.active = true;
    project.tasks.append(task);
    project.startDate = task.start;
    project.finishDate = task.finish;

    Resource resource;
    resource.uniqueId = 10;
    resource.id = 1;
    resource.name = QStringLiteral("Engineer");
    CostRate rate;
    rate.standardRate = 100.0;
    rate.standardRateUnit = 2;
    rate.overtimeRate = 150.0;
    rate.overtimeRateUnit = 2;
    rate.startDate = kStart.addDays(-1);
    rate.costPerUse = 50.0;
    resource.costRates.append(rate);
    project.resources.append(resource);

    Assignment assignment;
    assignment.uniqueId = 100;
    assignment.taskUniqueId = 1;
    assignment.resourceUniqueId = 10;
    assignment.start = kStart;
    assignment.finish = task.finish;
    project.assignments.append(assignment);
    return project;
}
}

class TstProjectReconciliation : public QObject
{
    Q_OBJECT
private slots:
    void bucketsAreAuthoritativeAndRollUp();
    void aggregateOnlyEstimateToCompleteIsPreserved();
    void effectiveDatedRatesApplyPerBucket();
    void overtimeIsSubsetAndUsesOvertimeRate();
    void materialAndCostResourcesDoNotCreateLabor();
    void importedMaterialCostSurvivesWithoutRateTable();
    void summaryExcludesInactiveLeaves();
    void validatorFindsStaleAggregates();
    void nativeRoundTripReconcilesAfterReopen();
};

void TstProjectReconciliation::bucketsAreAuthoritativeAndRollUp()
{
    Project project = workProject();
    Assignment &assignment = project.assignments.first();
    assignment.workMillis = 99 * kHour;
    assignment.actualWorkMillis = 20 * kHour;
    assignment.remainingWorkMillis = 79 * kHour;
    assignment.timephasedValues = {
        bucket(TimephasedValue::ActualWork, 0, 4 * kHour),
        bucket(TimephasedValue::RemainingWork, 1, 6 * kHour)
    };

    ProjectReconciliation::reconcile(project);
    QCOMPARE(assignment.actualWorkMillis, 4 * kHour);
    QCOMPARE(assignment.remainingWorkMillis, 6 * kHour);
    QCOMPARE(assignment.workMillis, 10 * kHour);
    QCOMPARE(assignment.actualCost, 450.0);
    QCOMPARE(assignment.remainingCost, 600.0);
    QCOMPARE(assignment.cost, 1050.0);
    QCOMPARE(project.tasks.first().workMillis, 10 * kHour);
    QCOMPARE(project.tasks.first().actualWorkMillis, 4 * kHour);
    QCOMPARE(project.resources.first().cost, 1050.0);
    QVERIFY(ProjectReconciliation::invariantViolations(project).isEmpty());
}

void TstProjectReconciliation::aggregateOnlyEstimateToCompleteIsPreserved()
{
    Project project = workProject();
    Assignment &assignment = project.assignments.first();
    assignment.workMillis = 16 * kHour;
    assignment.actualWorkMillis = 4 * kHour;
    assignment.remainingWorkMillis = 6 * kHour; // explicit estimate to complete

    ProjectReconciliation::reconcile(project);
    QCOMPARE(assignment.workMillis, 10 * kHour);
    QCOMPARE(assignment.actualWorkMillis, 4 * kHour);
    QCOMPARE(assignment.remainingWorkMillis, 6 * kHour);

    assignment.timephasedValues.clear();
    assignment.workMillis = 16 * kHour;
    assignment.actualWorkMillis = 4 * kHour;
    assignment.remainingWorkMillis = 0;
    ProjectReconciliation::reconcile(project);
    QCOMPARE(assignment.remainingWorkMillis, 12 * kHour);
}

void TstProjectReconciliation::effectiveDatedRatesApplyPerBucket()
{
    Project project = workProject();
    project.resources.first().costRates.clear();
    CostRate first;
    first.standardRate = 100.0;
    first.standardRateUnit = 2;
    first.startDate = kStart.addDays(-1);
    first.endDate = kStart.addDays(1).addMSecs(-1);
    CostRate second = first;
    second.standardRate = 125.0;
    second.startDate = kStart.addDays(1);
    second.endDate = {};
    project.resources.first().costRates = { first, second };
    project.assignments.first().timephasedValues = {
        bucket(TimephasedValue::ActualWork, 0, 2 * kHour),
        bucket(TimephasedValue::RemainingWork, 1, 3 * kHour)
    };

    ProjectReconciliation::reconcile(project);
    QCOMPARE(project.assignments.first().actualCost, 200.0);
    QCOMPARE(project.assignments.first().remainingCost, 375.0);
}

void TstProjectReconciliation::overtimeIsSubsetAndUsesOvertimeRate()
{
    Project project = workProject();
    Assignment &assignment = project.assignments.first();
    assignment.remainingWorkMillis = 4 * kHour;
    assignment.remainingOvertimeWorkMillis = kHour;
    assignment.overtimeWorkMillis = 3 * kHour;
    assignment.timephasedValues = {
        bucket(TimephasedValue::ActualWork, 0, 8 * kHour),
        bucket(TimephasedValue::ActualOvertimeWork, 0, 2 * kHour)
    };

    ProjectReconciliation::reconcile(project);
    QCOMPARE(assignment.actualOvertimeWorkMillis, 2 * kHour);
    QCOMPARE(assignment.remainingOvertimeWorkMillis, kHour);
    QCOMPARE(assignment.overtimeWorkMillis, 3 * kHour);
    QCOMPARE(assignment.workMillis, 12 * kHour);
    QCOMPARE(assignment.actualOvertimeCost, 300.0);
    QCOMPARE(assignment.remainingOvertimeCost, 150.0);
    QCOMPARE(assignment.overtimeCost, 450.0);
    QCOMPARE(assignment.actualCost, 950.0);
    QCOMPARE(assignment.remainingCost, 450.0);
    QCOMPARE(assignment.cost, 1400.0);
    QVERIFY(ProjectReconciliation::invariantViolations(project).isEmpty());
}

void TstProjectReconciliation::materialAndCostResourcesDoNotCreateLabor()
{
    Project project = workProject();
    project.assignments.first().workMillis = 8 * kHour;

    Resource material;
    material.uniqueId = 20;
    material.type = Resource::Type::Material;
    material.materialLabel = QStringLiteral("tons");
    CostRate materialRate;
    materialRate.standardRate = 10.0;
    material.costRates.append(materialRate);
    project.resources.append(material);
    Assignment materialAssignment;
    materialAssignment.uniqueId = 200;
    materialAssignment.taskUniqueId = 1;
    materialAssignment.resourceUniqueId = 20;
    materialAssignment.start = kStart;
    materialAssignment.workMillis = 5 * kHour; // five material units
    materialAssignment.remainingWorkMillis = 5 * kHour;
    project.assignments.append(materialAssignment);

    Resource costResource;
    costResource.uniqueId = 30;
    costResource.type = Resource::Type::Cost;
    project.resources.append(costResource);
    Assignment costAssignment;
    costAssignment.uniqueId = 300;
    costAssignment.taskUniqueId = 1;
    costAssignment.resourceUniqueId = 30;
    costAssignment.cost = 500.0;
    project.assignments.append(costAssignment);

    ProjectReconciliation::reconcile(project);
    QCOMPARE(project.tasks.first().workMillis, 8 * kHour);
    QCOMPARE(project.tasks.first().cost, 8 * 100.0 + 50.0 + 5 * 10.0 + 500.0);
    QCOMPARE(project.assignments.at(2).remainingCost, 500.0);
}

void TstProjectReconciliation::importedMaterialCostSurvivesWithoutRateTable()
{
    Project project;
    Task task;
    task.uniqueId = 1;
    task.id = 1;
    project.tasks.append(task);
    Resource material;
    material.uniqueId = 20;
    material.type = Resource::Type::Material;
    project.resources.append(material);
    Assignment assignment;
    assignment.uniqueId = 200;
    assignment.taskUniqueId = 1;
    assignment.resourceUniqueId = 20;
    assignment.cost = 275.0;
    assignment.workMillis = 5 * kHour;
    assignment.remainingWorkMillis = 5 * kHour;
    project.assignments.append(assignment);

    ProjectReconciliation::reconcile(project);
    QCOMPARE(project.assignments.first().cost, 275.0);
    QCOMPARE(project.assignments.first().remainingCost, 275.0);
    QCOMPARE(project.tasks.first().cost, 275.0);
}

void TstProjectReconciliation::summaryExcludesInactiveLeaves()
{
    Project project = workProject();
    Task summary;
    summary.uniqueId = 50;
    summary.id = 0;
    summary.outlineLevel = 1;
    summary.summary = true;
    project.tasks.first().id = 1;
    project.tasks.first().outlineLevel = 2;
    project.tasks.prepend(summary);
    Task inactive = project.tasks.at(1);
    inactive.uniqueId = 2;
    inactive.id = 2;
    inactive.active = false;
    inactive.workMillis = 40 * kHour;
    project.tasks.append(inactive);
    project.assignments.first().workMillis = 8 * kHour;

    ProjectReconciliation::reconcile(project);
    QCOMPARE(project.tasks.first().workMillis, 8 * kHour);
}

void TstProjectReconciliation::validatorFindsStaleAggregates()
{
    Project project = workProject();
    project.assignments.first().workMillis = 8 * kHour;
    ProjectReconciliation::reconcile(project);
    project.tasks.first().workMillis += kHour;
    project.resources.first().cost += 1.0;

    const QStringList issues = ProjectReconciliation::invariantViolations(project);
    QVERIFY(issues.size() >= 2);
}

void TstProjectReconciliation::nativeRoundTripReconcilesAfterReopen()
{
    Project project = workProject();
    project.assignments.first().timephasedValues = {
        bucket(TimephasedValue::ActualWork, 0, 4 * kHour),
        bucket(TimephasedValue::ActualOvertimeWork, 0, 2 * kHour),
        bucket(TimephasedValue::RemainingWork, 1, 6 * kHour)
    };
    project.assignments.first().remainingOvertimeWorkMillis = kHour;
    ProjectReconciliation::reconcile(project);

    MppIO writer;
    writer.setProject(project);
    const QByteArray bytes = writer.saveToData();
    QVERIFY2(!bytes.isEmpty(), qPrintable(writer.errorString()));
    const QString artifactPath =
        qEnvironmentVariable("SCHEDULEIO_OVERTIME_ARTIFACT");
    if (!artifactPath.isEmpty()) {
        QFile artifact(artifactPath);
        QVERIFY(artifact.open(QIODevice::WriteOnly));
        QCOMPARE(artifact.write(bytes), bytes.size());
    }
    MppIO reader;
    QVERIFY2(reader.openFromData(bytes), qPrintable(reader.errorString()));
    Project reopened = reader.project();
    ProjectReconciliation::reconcile(reopened);

    QVERIFY(ProjectReconciliation::invariantViolations(reopened).isEmpty());
    QCOMPARE(reopened.assignments.first().actualWorkMillis, 4 * kHour);
    QCOMPARE(reopened.assignments.first().remainingWorkMillis, 6 * kHour);
    QCOMPARE(reopened.assignments.first().actualOvertimeWorkMillis, 2 * kHour);
    QCOMPARE(reopened.assignments.first().remainingOvertimeWorkMillis, kHour);
    QCOMPARE(reopened.assignments.first().overtimeWorkMillis, 3 * kHour);
    QCOMPARE(reopened.tasks.first().workMillis, 10 * kHour);
    QCOMPARE(reopened.resources.first().cost, 1200.0);
}

QTEST_MAIN(TstProjectReconciliation)
#include "tst_projectreconciliation.moc"
