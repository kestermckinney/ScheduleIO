// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only
#include "exportselection.h"
#include "model/project.h"
#include "model/projectreconciliation.h"

#include <QHash>
#include <QList>
#include <QVector>
#include <algorithm>
#include <utility>

namespace schedule {

bool selectVaultExportTasks(const Project &source, const QSet<int> &taskUids,
                            Project &output, QString &error)
{
    error.clear();
    if (taskUids.isEmpty()) {
        error = QStringLiteral("Select at least one task UID");
        return false;
    }

    const qsizetype count = source.tasks.size();
    QList<qsizetype> outline;
    QVector<qsizetype> parent(count, -1);
    QVector<bool> keep(count, false);
    QSet<int> missing = taskUids;
    QSet<int> selectedScope;
    qsizetype subtreeLevel = -1;
    for (qsizetype i = 0; i < count; ++i) {
        const auto &task = source.tasks.at(i);
        while (!outline.isEmpty() && source.tasks.at(outline.constLast()).outlineLevel >= task.outlineLevel)
            outline.removeLast();
        if (!outline.isEmpty())
            parent[i] = outline.constLast();
        outline.append(i);

        if (subtreeLevel >= task.outlineLevel)
            subtreeLevel = -1;
        if (taskUids.contains(task.uniqueId) && task.uniqueId >= 0) {
            missing.remove(task.uniqueId);
            if (subtreeLevel < 0)
                subtreeLevel = task.outlineLevel;
        }
        keep[i] = subtreeLevel >= 0;
        if (keep.at(i) && task.uniqueId >= 0)
            selectedScope.insert(task.uniqueId);
    }
    if (!missing.isEmpty()) {
        error = QStringLiteral("At least one selected task UID is missing from this schedule");
        return false;
    }
    for (qsizetype i = count; i-- > 0;) {
        if (keep.at(i) && parent.at(i) >= 0)
            keep[parent.at(i)] = true;
    }

    Project filtered = source;
    filtered.tasks.clear();
    filtered.assignments.clear();
    filtered.resources.clear();
    filtered.relations.clear();

    QSet<int> includedTasks;
    for (qsizetype i = 0; i < count; ++i) {
        if (!keep.at(i))
            continue;
        const auto &task = source.tasks.at(i);
        filtered.tasks.append(task);
        if (task.uniqueId >= 0)
            includedTasks.insert(task.uniqueId);
    }

    QSet<int> includedResources;
    for (const auto &assignment : source.assignments) {
        if (!selectedScope.contains(assignment.taskUniqueId))
            continue;
        filtered.assignments.append(assignment);
        if (assignment.resourceUniqueId >= 0)
            includedResources.insert(assignment.resourceUniqueId);
    }
    for (const auto &resource : source.resources) {
        if (includedResources.contains(resource.uniqueId))
            filtered.resources.append(resource);
    }
    for (const auto &relation : source.relations) {
        if (includedTasks.contains(relation.predecessorTaskUid)
            && includedTasks.contains(relation.successorTaskUid))
            filtered.relations.append(relation);
    }

    // The source document's opaque and view data can contain omitted rows.
    filtered.mppSourceTemplate.clear();
    filtered.mppFontBases.clear();
    filtered.mppBarExceptions.clear();
    filtered.timelineView = {};
    filtered.ganttView = {};
    filtered.resourceUsageView = {};
    filtered.taskUsageView = {};
    filtered.teamPlannerView = {};
    filtered.resourceSheetView = {};
    filtered.calendarView = {};
    filtered.networkView = {};
    ProjectReconciliation::reconcile(filtered);
    output = std::move(filtered);
    return true;
}

void applyVaultExportFields(Project &project, const VaultExportFields &fields)
{
    if (!fields.baselines) {
        project.baselineForEarnedValue = 0;
        for (auto &task : project.tasks)
            task.baselines.clear();
        for (auto &resource : project.resources)
            resource.baselines.clear();
        for (auto &assignment : project.assignments) {
            assignment.baselines.clear();
            auto &values = assignment.timephasedValues;
            values.erase(std::remove_if(values.begin(), values.end(), [](const TimephasedValue &value) {
                return value.type == TimephasedValue::BaselineWork
                    || value.type == TimephasedValue::BaselineCost || value.baselineNumber != 0;
            }), values.end());
        }
    }

    if (!fields.customFields) {
        project.customFieldDefinitions.clear();
        for (auto &task : project.tasks)
            task.customFields.clear();
        for (auto &resource : project.resources)
            resource.customFields.clear();
        for (auto &assignment : project.assignments)
            assignment.customFields.clear();
    }

    if (!fields.calendars) {
        project.calendars.clear();
        project.calendarUniqueId = -1;
        for (auto &task : project.tasks) {
            task.calendarUniqueId = -1;
            task.ignoreResourceCalendar = false;
        }
        for (auto &resource : project.resources)
            resource.calendarUniqueId = -1;
    }
}

VaultActualsExportSummary applyVaultApprovedWork(Project &project, const QList<VaultApprovedWork> &work)
{
    QHash<int, QList<TimephasedValue>> actuals;
    QSet<int> assignments;
    for (const auto &assignment : std::as_const(project.assignments)) {
        if (!assignment.budget && assignment.uniqueId >= 0)
            assignments.insert(assignment.uniqueId);
    }

    VaultActualsExportSummary summary;
    for (const auto &row : work) {
        if (!assignments.contains(row.assignmentUid) || !row.from.isValid()
            || !row.to.isValid() || row.to <= row.from || row.workMillis <= 0) {
            summary.omittedEntries += row.entryCount;
            continue;
        }
        TimephasedValue value;
        value.type = TimephasedValue::ActualWork;
        value.uniqueId = row.assignmentUid;
        value.start = row.from;
        value.finish = row.to;
        value.unit = 1;
        value.value = QStringLiteral("PT%1.%2S").arg(row.workMillis / 1000)
            .arg(row.workMillis % 1000, 3, 10, QLatin1Char('0'));
        actuals[row.assignmentUid].append(std::move(value));
    }

    for (auto &assignment : project.assignments) {
        const auto buckets = actuals.constFind(assignment.uniqueId);
        if (buckets == actuals.constEnd())
            continue;
        ++summary.replacedAssignments;
        auto &values = assignment.timephasedValues;
        values.erase(std::remove_if(values.begin(), values.end(), [](const TimephasedValue &value) {
            return value.type == TimephasedValue::ActualWork
                || value.type == TimephasedValue::ActualOvertimeWork || value.type == TimephasedValue::ActualCost;
        }), values.end());
        values.append(*buckets);
        std::stable_sort(values.begin(), values.end(), [](const TimephasedValue &a, const TimephasedValue &b) {
            return a.start < b.start;
        });
        assignment.actualWorkMillis = 0;
        assignment.actualOvertimeWorkMillis = 0;
        assignment.overtimeWorkMillis = assignment.remainingOvertimeWorkMillis;
        assignment.workMillis = assignment.remainingWorkMillis;
        assignment.actualCost = 0;
        assignment.actualOvertimeCost = 0;
        assignment.cost = assignment.remainingCost;
        assignment.overtimeCost = assignment.remainingOvertimeCost;
    }
    if (summary.replacedAssignments > 0)
        ProjectReconciliation::reconcile(project);
    return summary;
}

} // namespace schedule
