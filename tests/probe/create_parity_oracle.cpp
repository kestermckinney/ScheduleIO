// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

// Generate one deterministic native MPP containing the seven parity features
// being exercised by Schedule Vault's remote Microsoft Project COM oracle.
#include "mppio.h"
#include "codec/mppfieldids.h"
#include "model/customfieldlogic.h"
#include "model/duration.h"
#include "model/projectreconciliation.h"
#include "model/scheduler.h"

#include <cstdio>

namespace {
constexpr qint64 kHour = 3600000LL;

schedule::Task task(int uid, const QString &name, const QDateTime &start,
                    qint64 duration)
{
    schedule::Task value;
    value.uniqueId = uid;
    value.id = uid;
    value.outlineLevel = 1;
    value.name = name;
    value.start = start;
    value.durationMillis = duration;
    value.durationFormat = schedule::Duration::Days;
    value.finish = schedule::Scheduler::addWork(start, duration);
    return value;
}

schedule::Relation link(int uid, int predecessor, int successor)
{
    schedule::Relation value;
    value.uniqueId = uid;
    value.predecessorTaskUid = predecessor;
    value.successorTaskUid = successor;
    value.type = schedule::Relation::FinishToStart;
    return value;
}
}

int main(int argc, char **argv)
{
    if (argc != 2) {
        std::fprintf(stderr, "usage: create_parity_oracle <out.mpp>\n");
        return 2;
    }

    schedule::Project project;
    project.formatVersion = schedule::Project::FormatVersion::Mpp14;
    project.title = QStringLiteral("Schedule Vault Seven Feature Oracle");
    project.startDate = QDateTime(QDate(2026, 8, 3), QTime(8, 0));
    project.calendarUniqueId = 1;
    project.multipleCriticalPaths = true;

    schedule::Calendar calendar = schedule::Calendar::microsoftDefaults().first();
    schedule::CalendarException exception;
    exception.name = QStringLiteral("Biweekly maintenance shutdown");
    exception.fromDate = QDate(2026, 8, 5);
    exception.toDate = QDate(2026, 9, 30);
    exception.working = false;
    exception.recurrence = schedule::CalendarException::Recurrence::Weekly;
    exception.interval = 2;
    exception.weekDayMask = 1u << 2; // Wednesday
    exception.occurrences = 4;
    calendar.exceptions.append(exception);
    project.calendars = {calendar};

    // Two independent dependency networks of unequal length. With the project
    // option enabled, each terminal chain should contain critical tasks.
    project.tasks = {
        task(1, QStringLiteral("Long network - design"), project.startDate, 16 * kHour),
        task(2, QStringLiteral("Long network - build"), project.startDate, 16 * kHour),
        task(3, QStringLiteral("Short network - prepare"), project.startDate, 8 * kHour),
        task(4, QStringLiteral("Short network - deliver"), project.startDate, 8 * kHour),
        task(5, QStringLiteral("Split leveled installation"), project.startDate, 20 * kHour),
        task(6, QStringLiteral("Cost-resource review"), project.startDate, 8 * kHour)
    };
    project.relations = {link(1, 1, 2), link(2, 3, 4)};

    schedule::Task &customTask = project.tasks[0];
    customTask.fixedCost = 1250.0;
    customTask.cost = 1250.0;
    schedule::CustomField score;
    score.fieldId = (int(MppFieldIds::kTaskHigh) << 16) | 87; // Number1
    score.name = QStringLiteral("Schedule Risk Score");
    score.formula = QStringLiteral("IIf([Cost] > 1000, 10, 1)");
    score.lookupValues = {QStringLiteral("1"), QStringLiteral("10")};
    score.graphicalIndicators.append({QStringLiteral("ge"), 10,
                                      QStringLiteral("warning")});
    customTask.customFields.append(score);
    project.customFieldDefinitions.append(score);

    // Explicit split plus gapped remaining-work buckets, matching the output
    // produced by split-capable leveling.
    schedule::Task &split = project.tasks[4];
    split.segments = {
        {split.start, QDateTime(QDate(2026, 8, 3), QTime(12, 0))},
        {QDateTime(QDate(2026, 8, 6), QTime(8, 0)),
         QDateTime(QDate(2026, 8, 7), QTime(17, 0))}
    };
    split.finish = split.segments.last().finish;

    schedule::Resource worker;
    worker.uniqueId = 10; worker.id = 1; worker.name = QStringLiteral("Installer");
    schedule::Resource budget;
    budget.uniqueId = 11; budget.id = 2; budget.name = QStringLiteral("Program budget");
    budget.budget = true; budget.budgetCost = 50000.0; budget.budgetWorkMillis = 80 * kHour;
    schedule::Resource cost;
    cost.uniqueId = 12; cost.id = 3; cost.name = QStringLiteral("Permit fee");
    cost.type = schedule::Resource::Type::Cost;
    project.resources = {worker, budget, cost};

    schedule::Assignment labor;
    labor.uniqueId = 100; labor.taskUniqueId = split.uniqueId;
    labor.resourceUniqueId = worker.uniqueId; labor.units = 1.0;
    labor.start = split.start; labor.finish = split.finish;
    labor.workMillis = 20 * kHour; labor.remainingWorkMillis = labor.workMillis;
    labor.workContour = 8;
    schedule::TimephasedValue first;
    first.type = schedule::TimephasedValue::RemainingWork; first.uniqueId = labor.uniqueId;
    first.start = split.segments.first().start; first.finish = split.segments.first().finish;
    first.unit = 1; first.value = QStringLiteral("PT4H0M0S");
    schedule::TimephasedValue second = first;
    second.start = split.segments.last().start; second.finish = split.segments.last().finish;
    second.value = QStringLiteral("PT16H0M0S");
    schedule::TimephasedValue baselineWork = first;
    baselineWork.type = schedule::TimephasedValue::BaselineWork;
    baselineWork.baselineNumber = 0; baselineWork.value = QStringLiteral("PT4H0M0S");
    schedule::TimephasedValue baselineCost = baselineWork;
    baselineCost.type = schedule::TimephasedValue::BaselineCost;
    baselineCost.unit = 2; baselineCost.value = QStringLiteral("400");
    labor.timephasedValues = {first, second, baselineWork, baselineCost};
    schedule::Baseline laborBaseline;
    laborBaseline.number = 0; laborBaseline.workMillis = labor.workMillis;
    laborBaseline.cost = 2400.0; laborBaseline.start = split.start;
    laborBaseline.finish = split.finish; laborBaseline.durationMillis = split.durationMillis;
    labor.baselines.append(laborBaseline);

    schedule::Assignment budgetAssignment;
    budgetAssignment.uniqueId = 101; budgetAssignment.taskUniqueId = 0;
    budgetAssignment.resourceUniqueId = budget.uniqueId; budgetAssignment.budget = true;
    budgetAssignment.budgetCost = budget.budgetCost;
    budgetAssignment.budgetWorkMillis = budget.budgetWorkMillis;

    schedule::Assignment costAssignment;
    costAssignment.uniqueId = 102; costAssignment.taskUniqueId = 6;
    costAssignment.resourceUniqueId = cost.uniqueId; costAssignment.units = 0.0;
    costAssignment.workMillis = 0; costAssignment.cost = 725.50;
    costAssignment.remainingCost = 725.50;
    project.assignments = {labor, budgetAssignment, costAssignment};
    project.budgetCost = budget.budgetCost;
    project.budgetWorkMillis = budget.budgetWorkMillis;

    schedule::CustomFieldLogic::recalculate(project);
    schedule::Scheduler::reschedule(project);
    schedule::Scheduler::computeSlack(project);
    schedule::ProjectReconciliation::reconcile(project);
    for (const schedule::Task &value : project.tasks)
        if (!project.finishDate.isValid() || value.finish > project.finishDate)
            project.finishDate = value.finish;

    MppIO io;
    io.setProject(project);
    if (!io.save(QString::fromLocal8Bit(argv[1]))) {
        std::fprintf(stderr, "save failed: %s\n", qPrintable(io.errorString()));
        return 1;
    }
    return 0;
}
