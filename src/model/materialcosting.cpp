// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/materialcosting.h"

#include <QHash>
#include <QSet>
#include <QtGlobal>
#include <cmath>
#include <utility>

namespace schedule {

namespace {

constexpr qint64 kMinute = 60LL * 1000LL;
constexpr qint64 kHour = 60LL * kMinute;

qint64 denominator(int unit)
{
    switch (unit) {
    case 1: return kMinute;
    case 2: return kHour;
    case 3: return 8LL * kHour;
    case 4: return 40LL * kHour;
    case 5: return 160LL * kHour;
    case 7: return 1920LL * kHour;
    default: return 0;
    }
}

const Resource *resourceByUid(const Project &project, int uid)
{
    for (const Resource &resource : project.resources)
        if (resource.uniqueId == uid)
            return &resource;
    return nullptr;
}

const CostRate *rateAt(const Resource &resource, int table, const QDateTime &when)
{
    const CostRate *best = nullptr;
    for (const CostRate &rate : resource.costRates) {
        if (rate.table != table)
            continue;
        if (rate.startDate.isValid() && when.isValid() && rate.startDate > when)
            continue;
        if (rate.endDate.isValid() && when.isValid() && rate.endDate < when)
            continue;
        if (!best || (!best->startDate.isValid() && rate.startDate.isValid())
            || (rate.startDate.isValid() && best->startDate.isValid()
                && rate.startDate > best->startDate))
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

double costForBuckets(const Assignment &assignment, const Resource &resource,
                      int type, qint64 aggregate)
{
    double total = 0.0;
    bool found = false;
    for (const TimephasedValue &bucket : assignment.timephasedValues) {
        if (bucket.type != type)
            continue;
        found = true;
        const CostRate *rate = rateAt(resource, assignment.costRateTable,
                                      bucket.start);
        if (rate)
            total += assignment.timephasedMaterialInPeriod(
                         type, bucket.start, bucket.finish) * rate->standardRate;
    }
    if (!found && aggregate > 0) {
        const CostRate *rate = rateAt(resource, assignment.costRateTable,
                                      assignment.start);
        if (rate)
            total = double(aggregate) / double(kHour) * rate->standardRate;
    }
    return total;
}

double baselineCost(const QList<Baseline> &baselines)
{
    for (const Baseline &baseline : baselines)
        if (baseline.number == 0)
            return baseline.cost;
    return 0.0;
}

} // namespace

qint64 MaterialCosting::quantityMillisForDuration(qint64 durationMillis,
                                                  double rate,
                                                  int variableRateUnits)
{
    const qint64 divisor = denominator(variableRateUnits);
    if (durationMillis <= 0 || rate < 0.0 || divisor <= 0)
        return 0;
    const double quantity = rate * double(durationMillis) / double(divisor);
    return qint64(std::llround(quantity * double(kHour)));
}

void MaterialCosting::recalculate(Project &project)
{
    QSet<int> materialResourceUids;
    QSet<int> materialTaskUids;
    for (Assignment &assignment : project.assignments) {
        if (assignment.budget)
            continue;
        const Resource *resource = resourceByUid(project, assignment.resourceUniqueId);
        if (!resource || resource->type != Resource::Type::Material)
            continue;
        materialResourceUids.insert(assignment.resourceUniqueId);
        materialTaskUids.insert(assignment.taskUniqueId);
        assignment.actualCost = costForBuckets(
            assignment, *resource, TimephasedValue::ActualWork,
            assignment.actualWorkMillis);
        assignment.remainingCost = costForBuckets(
            assignment, *resource, TimephasedValue::RemainingWork,
            assignment.remainingWorkMillis);
        const CostRate *useRate = rateAt(*resource, assignment.costRateTable,
                                         assignment.start);
        const double perUse = useRate ? useRate->costPerUse : 0.0;
        if (assignment.actualWorkMillis > 0)
            assignment.actualCost += perUse;
        else if (assignment.remainingWorkMillis > 0 || assignment.workMillis > 0)
            assignment.remainingCost += perUse;
        assignment.cost = assignment.actualCost + assignment.remainingCost;
        assignment.costVariance = assignment.cost - baselineCost(assignment.baselines);
    }

    QHash<int, double> costByResource, actualByResource, remainingByResource;
    QHash<int, double> costByTask, actualByTask, remainingByTask;
    for (const Assignment &assignment : std::as_const(project.assignments)) {
        if (assignment.budget)
            continue;
        costByResource[assignment.resourceUniqueId] += assignment.cost;
        actualByResource[assignment.resourceUniqueId] += assignment.actualCost;
        remainingByResource[assignment.resourceUniqueId] += assignment.remainingCost;
        costByTask[assignment.taskUniqueId] += assignment.cost;
        actualByTask[assignment.taskUniqueId] += assignment.actualCost;
        remainingByTask[assignment.taskUniqueId] += assignment.remainingCost;
    }
    for (Resource &resource : project.resources) {
        if (!materialResourceUids.contains(resource.uniqueId))
            continue;
        resource.cost = costByResource.value(resource.uniqueId);
        resource.actualCost = actualByResource.value(resource.uniqueId);
        resource.remainingCost = remainingByResource.value(resource.uniqueId);
        resource.costVariance = resource.cost - baselineCost(resource.baselines);
    }
    for (Task &task : project.tasks) {
        if (task.summary || !materialTaskUids.contains(task.uniqueId))
            continue;
        task.cost = task.fixedCost + costByTask.value(task.uniqueId);
        task.actualCost = actualByTask.value(task.uniqueId);
        task.remainingCost = task.fixedCost + remainingByTask.value(task.uniqueId);
        task.costVariance = task.cost - baselineCost(task.baselines);
    }
}

} // namespace schedule
