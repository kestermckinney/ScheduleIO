// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/projectreconciliation.h"
#include "model/materialcosting.h"
#include "model/schedulingcalendar.h"

#include <QHash>
#include <QtGlobal>
#include <algorithm>
#include <cmath>
#include <utility>

namespace schedule {

namespace {

constexpr qint64 kMinute = 60LL * 1000LL;
constexpr qint64 kHour = 60LL * kMinute;

qint64 rateDenominator(int unit)
{
    switch (unit) {
    case 1: return kMinute;
    case 2: return kHour;
    case 3: return 8LL * kHour;
    case 4: return 40LL * kHour;
    case 5: return 160LL * kHour;
    case 7: return 1920LL * kHour;
    default: return kHour;
    }
}

const Resource *resourceByUid(const Project &project, int uid)
{
    for (const Resource &resource : project.resources)
        if (resource.uniqueId == uid)
            return &resource;
    return nullptr;
}

const Task *taskByUid(const Project &project, int uid)
{
    for (const Task &task : project.tasks)
        if (task.uniqueId == uid)
            return &task;
    return nullptr;
}

double baselineCost(const QList<Baseline> &baselines)
{
    for (const Baseline &baseline : baselines)
        if (baseline.number == 0)
            return baseline.cost;
    return 0.0;
}

const CostRate *rateAt(const Resource &resource, int table, const QDateTime &when)
{
    const CostRate *best = nullptr;
    for (const CostRate &rate : resource.costRates) {
        if (rate.table != table)
            continue;
        if (rate.startDate.isValid() && when.isValid() && when < rate.startDate)
            continue;
        if (rate.endDate.isValid() && when.isValid() && rate.endDate < when)
            continue;
        if (!best || (!best->startDate.isValid() && rate.startDate.isValid())
            || (rate.startDate.isValid() && best->startDate.isValid()
                && best->startDate < rate.startDate))
            best = &rate;
    }
    if (best)
        return best;
    for (const CostRate &rate : resource.costRates)
        if (rate.table == table
            && (!best || rate.startDate < best->startDate))
            best = &rate;
    return best;
}

qint64 bucketTotal(const Assignment &assignment, int type, bool *found,
                   QDateTime *first = nullptr, QDateTime *last = nullptr)
{
    qint64 total = 0;
    *found = false;
    for (const TimephasedValue &bucket : assignment.timephasedValues) {
        if (bucket.type != type)
            continue;
        *found = true;
        total += qMax<qint64>(0, bucket.durationMillis());
        if (first && (!first->isValid() || bucket.start < *first))
            *first = bucket.start;
        if (last && (!last->isValid() || *last < bucket.finish))
            *last = bucket.finish;
    }
    return total;
}

void reconcileAssignmentWork(Assignment &assignment)
{
    bool hasActual = false, hasRemaining = false, hasActualOvertime = false;
    QDateTime firstRemaining, lastActual;
    const qint64 actualBuckets = bucketTotal(
        assignment, TimephasedValue::ActualWork, &hasActual, nullptr, &lastActual);
    const qint64 remainingBuckets = bucketTotal(
        assignment, TimephasedValue::RemainingWork, &hasRemaining, &firstRemaining);
    const qint64 actualOvertimeBuckets = bucketTotal(
        assignment, TimephasedValue::ActualOvertimeWork, &hasActualOvertime);

    assignment.actualWorkMillis = hasActual
        ? actualBuckets : qMax<qint64>(0, assignment.actualWorkMillis);
    if (hasRemaining) {
        assignment.remainingWorkMillis = remainingBuckets;
    } else if (assignment.remainingWorkMillis > 0) {
        assignment.remainingWorkMillis = qMax<qint64>(0, assignment.remainingWorkMillis);
    } else {
        assignment.remainingWorkMillis = qMax<qint64>(
            0, assignment.workMillis - assignment.actualWorkMillis);
    }
    assignment.workMillis = assignment.actualWorkMillis + assignment.remainingWorkMillis;
    assignment.actualOvertimeWorkMillis = hasActualOvertime
        ? actualOvertimeBuckets : qMax<qint64>(0, assignment.actualOvertimeWorkMillis);
    if (assignment.remainingOvertimeWorkMillis > 0) {
        assignment.remainingOvertimeWorkMillis = qMax<qint64>(
            0, assignment.remainingOvertimeWorkMillis);
    } else {
        assignment.remainingOvertimeWorkMillis = qMax<qint64>(
            0, assignment.overtimeWorkMillis - assignment.actualOvertimeWorkMillis);
    }
    // Overtime is a subset of total work. Raising overtime therefore raises the
    // corresponding actual/remaining total when necessary; it never creates a
    // contradictory Overtime Work > Work state.
    assignment.actualWorkMillis = qMax(assignment.actualWorkMillis,
                                       assignment.actualOvertimeWorkMillis);
    assignment.remainingWorkMillis = qMax(assignment.remainingWorkMillis,
                                           assignment.remainingOvertimeWorkMillis);
    assignment.workMillis = assignment.actualWorkMillis + assignment.remainingWorkMillis;
    assignment.overtimeWorkMillis = assignment.actualOvertimeWorkMillis
        + assignment.remainingOvertimeWorkMillis;
    if (hasActual)
        assignment.stop = lastActual;
    if (hasRemaining)
        assignment.resume = firstRemaining;
}

double workCost(const Assignment &assignment, const Resource &resource, int type,
                qint64 aggregate, bool overtimeRate, bool *hasRate)
{
    double total = 0.0;
    bool hasBuckets = false;
    for (const TimephasedValue &bucket : assignment.timephasedValues) {
        if (bucket.type != type)
            continue;
        hasBuckets = true;
        const CostRate *rate = rateAt(resource, assignment.costRateTable, bucket.start);
        if (!rate)
            continue;
        *hasRate = true;
        total += double(qMax<qint64>(0, bucket.durationMillis()))
            / double(rateDenominator(overtimeRate ? rate->overtimeRateUnit
                                                  : rate->standardRateUnit))
            * (overtimeRate ? rate->overtimeRate : rate->standardRate);
    }
    if (!hasBuckets && aggregate > 0) {
        const CostRate *rate = rateAt(resource, assignment.costRateTable, assignment.start);
        if (rate) {
            *hasRate = true;
            total = double(aggregate)
                / double(rateDenominator(overtimeRate ? rate->overtimeRateUnit
                                                      : rate->standardRateUnit))
                * (overtimeRate ? rate->overtimeRate : rate->standardRate);
        }
    }
    return total;
}

void reconcileWorkCost(Assignment &assignment, const Resource &resource)
{
    bool hasRate = false;
    const double standardActual = workCost(
        assignment, resource, TimephasedValue::ActualWork,
        assignment.actualWorkMillis, false, &hasRate);
    const double standardRemaining = workCost(
        assignment, resource, TimephasedValue::RemainingWork,
        assignment.remainingWorkMillis, false, &hasRate);
    const double actualOvertimeAtStandard = workCost(
        assignment, resource, TimephasedValue::ActualOvertimeWork,
        assignment.actualOvertimeWorkMillis, false, &hasRate);
    const double actualOvertime = workCost(
        assignment, resource, TimephasedValue::ActualOvertimeWork,
        assignment.actualOvertimeWorkMillis, true, &hasRate);
    // Project has no separate remaining-overtime time-phased stream. Its aggregate
    // uses the rate effective at the assignment's current remaining-work anchor.
    bool ignored = false;
    const double remainingOvertimeAtStandard = workCost(
        assignment, resource, -1, assignment.remainingOvertimeWorkMillis,
        false, &ignored);
    const double remainingOvertime = workCost(
        assignment, resource, -1, assignment.remainingOvertimeWorkMillis,
        true, &ignored);
    hasRate = hasRate || ignored;
    double actual = standardActual - actualOvertimeAtStandard + actualOvertime;
    double remaining = standardRemaining - remainingOvertimeAtStandard + remainingOvertime;
    const CostRate *useRate = rateAt(resource, assignment.costRateTable, assignment.start);
    if (useRate) {
        hasRate = true;
        if (assignment.actualWorkMillis > 0)
            actual += useRate->costPerUse;
        else if (assignment.remainingWorkMillis > 0)
            remaining += useRate->costPerUse;
    }
    // Imported aggregate costs remain authoritative when no applicable rate table is
    // available. Once a rate exists, the dated work streams are authoritative.
    if (hasRate) {
        assignment.actualOvertimeCost = actualOvertime;
        assignment.remainingOvertimeCost = remainingOvertime;
        assignment.overtimeCost = actualOvertime + remainingOvertime;
        assignment.actualCost = actual;
        assignment.remainingCost = remaining;
        assignment.cost = actual + remaining;
    } else if (assignment.actualCost != 0.0 || assignment.remainingCost != 0.0) {
        assignment.cost = assignment.actualCost + assignment.remainingCost;
    } else if (assignment.cost != 0.0) {
        assignment.remainingCost = assignment.cost;
    }
    assignment.costVariance = assignment.cost - baselineCost(assignment.baselines);
}

bool moneyEqual(double a, double b)
{
    return qAbs(a - b) <= 0.005;
}

qint64 signedWorkBetween(const WorkCalendar &calendar, const QDateTime &baseline,
                         const QDateTime &current)
{
    if (!baseline.isValid() || !current.isValid())
        return 0;
    return baseline <= current ? calendar.workBetween(baseline, current)
                               : -calendar.workBetween(current, baseline);
}

void reconcileTaskVariance(const Project &project, Task &task)
{
    const Baseline *baseline = nullptr;
    for (const Baseline &candidate : std::as_const(task.baselines))
        if (candidate.number == 0) {
            baseline = &candidate;
            break;
        }
    if (!baseline)
        return;
    const WorkCalendar calendar = SchedulingCalendar::taskBase(project, task);
    task.startVarianceMillis = signedWorkBetween(calendar, baseline->start, task.start);
    task.finishVarianceMillis = signedWorkBetween(calendar, baseline->finish, task.finish);
    task.durationVarianceMillis = task.durationMillis - baseline->durationMillis;
    task.workVarianceMillis = task.workMillis - baseline->workMillis;
}

} // namespace

ProjectReconciliation::Totals ProjectReconciliation::taskTotals(
    const Project &project, int taskUniqueId)
{
    Totals totals;
    bool hasWorkAssignment = false;
    for (const Assignment &assignment : project.assignments) {
        if (assignment.taskUniqueId != taskUniqueId)
            continue;
        if (assignment.budget)
            continue;
        const Resource *resource = resourceByUid(project, assignment.resourceUniqueId);
        if (resource && resource->type == Resource::Type::Work) {
            hasWorkAssignment = true;
            totals.workMillis += assignment.workMillis;
            totals.actualWorkMillis += assignment.actualWorkMillis;
            totals.remainingWorkMillis += assignment.remainingWorkMillis;
            totals.overtimeWorkMillis += assignment.overtimeWorkMillis;
            totals.actualOvertimeWorkMillis += assignment.actualOvertimeWorkMillis;
            totals.remainingOvertimeWorkMillis += assignment.remainingOvertimeWorkMillis;
        }
        totals.cost += assignment.cost;
        totals.actualCost += assignment.actualCost;
        totals.remainingCost += assignment.remainingCost;
        totals.overtimeCost += assignment.overtimeCost;
    }
    const Task *task = taskByUid(project, taskUniqueId);
    if (task) {
        if (!hasWorkAssignment) {
            totals.workMillis = qMax<qint64>(0, task->workMillis);
            totals.actualWorkMillis = qBound<qint64>(0, task->actualWorkMillis,
                                                     totals.workMillis);
            totals.remainingWorkMillis = totals.workMillis - totals.actualWorkMillis;
        }
        totals.cost += task->fixedCost;
        if (task->actualFinish.isValid() || task->percentComplete >= 1.0)
            totals.actualCost += task->fixedCost;
        else
            totals.remainingCost += task->fixedCost;
    }
    return totals;
}

ProjectReconciliation::Totals ProjectReconciliation::resourceTotals(
    const Project &project, int resourceUniqueId)
{
    Totals totals;
    QHash<int, bool> activeTask;
    for (const Task &task : project.tasks)
        activeTask.insert(task.uniqueId, task.active && !task.summary);
    for (const Assignment &assignment : project.assignments) {
        if (assignment.resourceUniqueId != resourceUniqueId
            || assignment.budget || !activeTask.value(assignment.taskUniqueId, false))
            continue;
        totals.workMillis += assignment.workMillis;
        totals.actualWorkMillis += assignment.actualWorkMillis;
        totals.remainingWorkMillis += assignment.remainingWorkMillis;
        totals.overtimeWorkMillis += assignment.overtimeWorkMillis;
        totals.actualOvertimeWorkMillis += assignment.actualOvertimeWorkMillis;
        totals.remainingOvertimeWorkMillis += assignment.remainingOvertimeWorkMillis;
        totals.cost += assignment.cost;
        totals.actualCost += assignment.actualCost;
        totals.remainingCost += assignment.remainingCost;
        totals.overtimeCost += assignment.overtimeCost;
    }
    return totals;
}

void ProjectReconciliation::reconcile(Project &project)
{
    project.budgetCost = 0.0;
    project.budgetWorkMillis = 0;
    for (Resource &resource : project.resources) {
        resource.budgetCost = 0.0;
        resource.budgetWorkMillis = 0;
    }
    for (Assignment &assignment : project.assignments) {
        const Resource *resource = resourceByUid(project, assignment.resourceUniqueId);
        if (!assignment.budget && !(resource && resource->budget))
            continue;
        assignment.budget = true;
        assignment.workMillis = assignment.actualWorkMillis = assignment.remainingWorkMillis = 0;
        assignment.cost = assignment.actualCost = assignment.remainingCost = 0.0;
        project.budgetCost += assignment.budgetCost;
        project.budgetWorkMillis += assignment.budgetWorkMillis;
        for (Resource &r : project.resources)
            if (r.uniqueId == assignment.resourceUniqueId) {
                r.budgetCost += assignment.budgetCost;
                r.budgetWorkMillis += assignment.budgetWorkMillis;
                break;
            }
    }
    for (Assignment &assignment : project.assignments)
        if (!assignment.budget)
            reconcileAssignmentWork(assignment);

    // Material quantities use the assignment work-stream storage convention and their
    // own unit-cost rules. Preserve imported aggregates when the selected material rate
    // table is absent, just as we do for Work resources.
    struct StoredCost { double cost; double actual; double remaining; double variance; };
    QHash<int, StoredCost> materialImportedCost;
    for (const Assignment &assignment : std::as_const(project.assignments)) {
        const Resource *resource = resourceByUid(project, assignment.resourceUniqueId);
        if (resource && resource->type == Resource::Type::Material
            && !rateAt(*resource, assignment.costRateTable, assignment.start)) {
            materialImportedCost.insert(assignment.uniqueId,
                { assignment.cost, assignment.actualCost,
                  assignment.remainingCost, assignment.costVariance });
        }
    }
    MaterialCosting::recalculate(project);
    for (Assignment &assignment : project.assignments) {
        if (assignment.budget)
            continue;
        const Resource *resource = resourceByUid(project, assignment.resourceUniqueId);
        if (!resource)
            continue;
        if (resource->type == Resource::Type::Work)
            reconcileWorkCost(assignment, *resource);
        else if (resource->type == Resource::Type::Material
                 && materialImportedCost.contains(assignment.uniqueId)) {
            const StoredCost stored = materialImportedCost.value(assignment.uniqueId);
            assignment.cost = stored.cost;
            assignment.actualCost = stored.actual;
            assignment.remainingCost = stored.remaining;
            assignment.costVariance = stored.variance;
            if (assignment.actualCost != 0.0 || assignment.remainingCost != 0.0)
                assignment.cost = assignment.actualCost + assignment.remainingCost;
            else if (assignment.cost != 0.0)
                assignment.remainingCost = assignment.cost;
            assignment.costVariance = assignment.cost - baselineCost(assignment.baselines);
        }
        else if (resource->type == Resource::Type::Cost) {
            if (assignment.actualCost != 0.0 || assignment.remainingCost != 0.0)
                assignment.cost = assignment.actualCost + assignment.remainingCost;
            else if (assignment.cost != 0.0)
                assignment.remainingCost = assignment.cost;
            assignment.costVariance = assignment.cost - baselineCost(assignment.baselines);
        }
    }

    // Leaf tasks derive labor only from Work resources; Material and Cost resources
    // contribute cost but never task work or duration.
    for (Task &task : project.tasks) {
        if (task.summary || !task.active)
            continue;
        const Totals totals = taskTotals(project, task.uniqueId);
        task.workMillis = totals.workMillis;
        task.actualWorkMillis = totals.actualWorkMillis;
        task.cost = totals.cost;
        task.actualCost = totals.actualCost;
        task.remainingCost = totals.remainingCost;
        task.costVariance = task.cost - baselineCost(task.baselines);
        reconcileTaskVariance(project, task);
    }

    // Roll summaries from active leaf descendants in display-ID order without relying
    // on the storage order used by the binary file.
    QList<Task *> ordered;
    ordered.reserve(project.tasks.size());
    for (Task &task : project.tasks)
        ordered.append(&task);
    std::stable_sort(ordered.begin(), ordered.end(), [](const Task *a, const Task *b) {
        return a->id < b->id;
    });
    for (int i = ordered.size() - 1; i >= 0; --i) {
        Task &summary = *ordered.at(i);
        if (!summary.summary)
            continue;
        qint64 work = 0, actualWork = 0;
        double cost = 0.0, actualCost = 0.0, remainingCost = 0.0;
        for (int j = i + 1; j < ordered.size()
             && ordered.at(j)->outlineLevel > summary.outlineLevel; ++j) {
            const Task &leaf = *ordered.at(j);
            if (leaf.summary || !leaf.active)
                continue;
            work += leaf.workMillis;
            actualWork += leaf.actualWorkMillis;
            cost += leaf.cost;
            actualCost += leaf.actualCost;
            remainingCost += leaf.remainingCost;
        }
        summary.workMillis = work;
        summary.actualWorkMillis = actualWork;
        summary.cost = cost;
        summary.actualCost = actualCost;
        summary.remainingCost = remainingCost;
        summary.costVariance = cost - baselineCost(summary.baselines);
        reconcileTaskVariance(project, summary);
    }

    for (Resource &resource : project.resources) {
        const Totals totals = resourceTotals(project, resource.uniqueId);
        resource.cost = totals.cost;
        resource.actualCost = totals.actualCost;
        resource.remainingCost = totals.remainingCost;
        resource.costVariance = resource.cost - baselineCost(resource.baselines);
    }
}

QStringList ProjectReconciliation::invariantViolations(const Project &project)
{
    QStringList issues;
    for (const Assignment &assignment : project.assignments) {
        bool hasActual = false, hasRemaining = false;
        const qint64 actual = bucketTotal(
            assignment, TimephasedValue::ActualWork, &hasActual);
        const qint64 remaining = bucketTotal(
            assignment, TimephasedValue::RemainingWork, &hasRemaining);
        if (hasActual && actual != assignment.actualWorkMillis)
            issues.append(QStringLiteral("assignment %1 actual work buckets do not reconcile")
                              .arg(assignment.uniqueId));
        if (hasRemaining && remaining != assignment.remainingWorkMillis)
            issues.append(QStringLiteral("assignment %1 remaining work buckets do not reconcile")
                              .arg(assignment.uniqueId));
        if (assignment.workMillis
            != assignment.actualWorkMillis + assignment.remainingWorkMillis)
            issues.append(QStringLiteral("assignment %1 total work does not reconcile")
                              .arg(assignment.uniqueId));
        if (assignment.overtimeWorkMillis
            != assignment.actualOvertimeWorkMillis
                + assignment.remainingOvertimeWorkMillis
            || assignment.actualOvertimeWorkMillis > assignment.actualWorkMillis
            || assignment.remainingOvertimeWorkMillis > assignment.remainingWorkMillis)
            issues.append(QStringLiteral("assignment %1 overtime work does not reconcile")
                              .arg(assignment.uniqueId));
        if (!moneyEqual(assignment.cost,
                        assignment.actualCost + assignment.remainingCost))
            issues.append(QStringLiteral("assignment %1 cost does not reconcile")
                              .arg(assignment.uniqueId));
    }
    for (const Task &task : project.tasks) {
        if (task.summary || !task.active)
            continue;
        const Totals totals = taskTotals(project, task.uniqueId);
        if (task.workMillis != totals.workMillis
            || task.actualWorkMillis != totals.actualWorkMillis)
            issues.append(QStringLiteral("task %1 work does not reconcile").arg(task.uniqueId));
        if (!moneyEqual(task.cost, totals.cost)
            || !moneyEqual(task.actualCost, totals.actualCost)
            || !moneyEqual(task.remainingCost, totals.remainingCost))
            issues.append(QStringLiteral("task %1 cost does not reconcile").arg(task.uniqueId));
    }
    for (const Resource &resource : project.resources) {
        const Totals totals = resourceTotals(project, resource.uniqueId);
        if (!moneyEqual(resource.cost, totals.cost)
            || !moneyEqual(resource.actualCost, totals.actualCost)
            || !moneyEqual(resource.remainingCost, totals.remainingCost))
            issues.append(QStringLiteral("resource %1 cost does not reconcile")
                              .arg(resource.uniqueId));
    }

    // Reconcile a value-copy to validate derived rate calculations and summary rollups
    // without mutating the caller's project.
    Project canonical = project;
    reconcile(canonical);
    for (const Assignment &assignment : project.assignments) {
        for (const Assignment &expected : std::as_const(canonical.assignments)) {
            if (expected.uniqueId != assignment.uniqueId)
                continue;
            if (!moneyEqual(assignment.cost, expected.cost)
                || !moneyEqual(assignment.actualCost, expected.actualCost)
                || !moneyEqual(assignment.remainingCost, expected.remainingCost))
                issues.append(QStringLiteral("assignment %1 rate cost is stale")
                                  .arg(assignment.uniqueId));
            break;
        }
    }
    for (const Task &task : project.tasks) {
        if (!task.summary)
            continue;
        for (const Task &expected : std::as_const(canonical.tasks)) {
            if (expected.uniqueId != task.uniqueId)
                continue;
            if (task.workMillis != expected.workMillis
                || task.actualWorkMillis != expected.actualWorkMillis
                || !moneyEqual(task.cost, expected.cost)
                || !moneyEqual(task.actualCost, expected.actualCost)
                || !moneyEqual(task.remainingCost, expected.remainingCost))
                issues.append(QStringLiteral("summary task %1 rollup is stale")
                                  .arg(task.uniqueId));
            break;
        }
    }
    return issues;
}

} // namespace schedule
