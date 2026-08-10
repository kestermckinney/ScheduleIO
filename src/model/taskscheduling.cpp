// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/taskscheduling.h"
#include "model/duration.h"
#include "model/materialcosting.h"
#include "model/projectreconciliation.h"
#include "model/schedulingcalendar.h"
#include "model/scheduler.h"
#include "model/workcalendar.h"
#include "model/workcontouring.h"

#include <QtGlobal>
#include <cmath>

namespace schedule {

namespace {

constexpr double kMinUnits = 0.01;   // 1% floor keeps the triangle solvable
constexpr qint64 kHour = 3600LL * 1000LL;

Task *taskByUid(Project &p, int uid)
{
    for (Task &t : p.tasks)
        if (t.uniqueId == uid)
            return &t;
    return nullptr;
}

Assignment *assignmentByUid(Project &p, int uid)
{
    for (Assignment &a : p.assignments)
        if (a.uniqueId == uid)
            return &a;
    return nullptr;
}

const Resource *resourceByUid(const Project &p, int uid)
{
    for (const Resource &r : p.resources)
        if (r.uniqueId == uid)
            return &r;
    return nullptr;
}

bool isWorkAssignment(const Project &p, const Assignment &a)
{
    const Resource *resource = resourceByUid(p, a.resourceUniqueId);
    return a.resourceUniqueId >= 0 && resource
        && resource->type == Resource::Type::Work;
}

// Only Work resources participate in the duration/work/units scheduling
// triangle. Material quantities and Cost assignments are retained on the task
// but do not create labor or change its scheduled span.
QList<Assignment *> assignmentsOf(Project &p, int taskUid)
{
    QList<Assignment *> out;
    for (Assignment &a : p.assignments)
        if (a.taskUniqueId == taskUid && isWorkAssignment(p, a))
            out.append(&a);
    return out;
}

qint64 assignmentSpan(const Assignment &a)
{
    const double u = qMax(kMinUnits, a.units);
    return qint64(std::llround(double(a.workMillis) / u));
}

// Re-derive task duration/finish and assignment spans from work + units.
void sync(Project &p, Task &t)
{
    const QList<Assignment *> assns = assignmentsOf(p, t.uniqueId);
    const bool elapsed = Duration::isElapsed(t.durationFormat);

    if (!assns.isEmpty()) {
        qint64 span = 0;
        qint64 work = 0;
        for (const Assignment *a : assns) {
            span = qMax(span, assignmentSpan(*a));
            work += a->workMillis;
        }
        t.durationMillis = span;
        t.workMillis = work;   // keep the task-level total mirroring its assignments
    }
    if (t.start.isValid() && !assns.isEmpty() && !elapsed)
        t.start = SchedulingCalendar::nextStart(p, t, t.start);
    if (t.start.isValid() && assns.isEmpty()) {
        t.finish = SchedulingCalendar::finish(p, t, t.start);
    }

    QDateTime latestAssignmentFinish;
    for (Assignment *a : assns) {
        const WorkCalendar cal = SchedulingCalendar::assignment(p, t, *a);
        a->remainingWorkMillis = qMax<qint64>(0, a->workMillis - a->actualWorkMillis);
        if (!t.start.isValid())
            continue;
        QDateTime s = elapsed ? t.start : cal.nextWorkStart(t.start);
        if (a->delayMillis + a->levelingDelayMillis != 0)
            s = elapsed ? s.addMSecs(a->delayMillis + a->levelingDelayMillis)
                        : cal.addWork(s, a->delayMillis + a->levelingDelayMillis);
        a->start = s;
        a->finish = a->workMillis > 0
            ? (elapsed ? s.addMSecs(assignmentSpan(*a)) : cal.addWork(s, assignmentSpan(*a)))
            : s;
        if (!latestAssignmentFinish.isValid() || a->finish > latestAssignmentFinish)
            latestAssignmentFinish = a->finish;
        bool hasRemainingBuckets = false;
        for (const TimephasedValue &value : a->timephasedValues)
            if (value.type == TimephasedValue::RemainingWork) {
                hasRemainingBuckets = true;
                break;
            }
        if (a->workContour != WorkContouring::Contoured
            && (a->workContour != WorkContouring::Flat || hasRemainingBuckets))
            WorkContouring::regenerate(p, a->uniqueId);
    }
    if (latestAssignmentFinish.isValid())
        t.finish = latestAssignmentFinish;

    for (Assignment &a : p.assignments) {
        if (a.taskUniqueId != t.uniqueId || a.resourceUniqueId < 0
            || isWorkAssignment(p, a))
            continue;
        const Resource *resource = resourceByUid(p, a.resourceUniqueId);
        if (resource && resource->type == Resource::Type::Material
            && a.timephasedValues.isEmpty()) {
            a.workMillis = a.variableRateUnits == 0
                ? qint64(std::llround(a.units * double(kHour)))
                : MaterialCosting::quantityMillisForDuration(
                      t.durationMillis, a.units, a.variableRateUnits);
        }
        a.remainingWorkMillis = qMax<qint64>(0, a.workMillis - a.actualWorkMillis);
        a.start = t.start;
        a.finish = t.finish;
    }

    // With no real resources, any "unassigned" placeholder row read from a file
    // mirrors the task: MS Project carries the task's Work on it when one was
    // entered, otherwise duration x units, with the task's dates. Left stale,
    // its old work would re-derive the task's duration on import.
    if (assns.isEmpty()) {
        for (Assignment &a : p.assignments) {
            if (a.taskUniqueId != t.uniqueId || a.resourceUniqueId >= 0)
                continue;
            const double units = a.units > 0 ? a.units : 1.0;
            a.workMillis = t.workMillis > 0
                ? t.workMillis
                : qint64(std::llround(double(t.durationMillis) * units));
            a.remainingWorkMillis = qMax<qint64>(0, a.workMillis - a.actualWorkMillis);
            a.start = t.start;
            a.finish = t.finish;
        }
    }
    ProjectReconciliation::reconcile(p);
}

} // namespace

qint64 TaskScheduling::taskWork(const Project &p, int taskUid)
{
    qint64 total = 0;
    bool any = false;
    for (const Assignment &a : p.assignments)
        if (a.taskUniqueId == taskUid && isWorkAssignment(p, a)) {
            total += a.workMillis;
            any = true;
        }
    if (any)
        return total;
    // No assignments: the work is held on the task itself (see setWork).
    for (const Task &t : p.tasks)
        if (t.uniqueId == taskUid)
            return t.workMillis;
    return 0;
}

void TaskScheduling::syncTask(Project &p, int taskUid)
{
    if (Task *t = taskByUid(p, taskUid))
        sync(p, *t);
}

void TaskScheduling::setDuration(Project &p, int taskUid, qint64 durationMillis)
{
    Task *t = taskByUid(p, taskUid);
    if (!t)
        return;
    durationMillis = qMax<qint64>(0, durationMillis);
    const QList<Assignment *> assns = assignmentsOf(p, taskUid);

    if (t->taskType == 2) {
        // Fixed Work: the new duration re-derives units, work untouched.
        for (Assignment *a : assns)
            if (durationMillis > 0)
                a->units = qMax(kMinUnits, double(a->workMillis) / double(durationMillis));
    } else {
        // Fixed Units / Fixed Duration: work follows the duration.
        for (Assignment *a : assns)
            a->workMillis = qint64(std::llround(double(durationMillis) * qMax(kMinUnits, a->units)));
    }

    t->durationMillis = durationMillis;
    t->milestone = durationMillis == 0;
    sync(p, *t);
}

void TaskScheduling::setWork(Project &p, int taskUid, qint64 workMillis)
{
    Task *t = taskByUid(p, taskUid);
    if (!t)
        return;
    workMillis = qMax<qint64>(0, workMillis);
    const QList<Assignment *> assns = assignmentsOf(p, taskUid);
    if (assns.isEmpty()) {
        // No resources yet: the task itself holds the Work, like MS Project's
        // task-level Work. With an implied single 100%-units resource work equals
        // span, so the duration follows the work unless the duration is fixed. A
        // resource assigned later inherits this work (see addAssignment).
        t->workMillis = workMillis;
        if (t->taskType != 1)   // not Fixed Duration
            t->durationMillis = workMillis;
        t->milestone = t->durationMillis == 0;
        sync(p, *t);
        return;
    }

    // Distribute the new total proportionally to the current work, falling
    // back to units when the task had no work yet.
    qint64 oldTotal = 0;
    double unitsTotal = 0.0;
    for (const Assignment *a : assns) {
        oldTotal += a->workMillis;
        unitsTotal += qMax(kMinUnits, a->units);
    }
    qint64 assigned = 0;
    for (int i = 0; i < assns.size(); ++i) {
        Assignment *a = assns.at(i);
        qint64 share;
        if (i == assns.size() - 1) {
            share = workMillis - assigned;   // the remainder, so shares sum exactly
        } else if (oldTotal > 0) {
            share = qint64(std::llround(double(workMillis) * double(a->workMillis) / double(oldTotal)));
        } else {
            share = qint64(std::llround(double(workMillis) * qMax(kMinUnits, a->units) / unitsTotal));
        }
        a->workMillis = qMax<qint64>(0, share);
        assigned += a->workMillis;
    }

    if (t->taskType == 1) {
        // Fixed Duration: units absorb the change, the span stays put.
        for (Assignment *a : assns)
            if (t->durationMillis > 0)
                a->units = qMax(kMinUnits, double(a->workMillis) / double(t->durationMillis));
    }
    // Fixed Units / Fixed Work: duration follows (sync derives it).
    sync(p, *t);
}

void TaskScheduling::setAssignmentUnits(Project &p, int assignmentUid, double units)
{
    Assignment *a = assignmentByUid(p, assignmentUid);
    if (!a)
        return;
    Task *t = taskByUid(p, a->taskUniqueId);
    if (!t)
        return;
    units = qMax(kMinUnits, units);

    if (!isWorkAssignment(p, *a)) {
        a->units = units;
        sync(p, *t);
        return;
    }

    if (t->taskType == 1) {
        // Fixed Duration: work follows the new units over the fixed span.
        a->units = units;
        a->workMillis = qint64(std::llround(double(t->durationMillis) * units));
    } else {
        // Fixed Units / Fixed Work: work is kept, the span (duration) moves.
        a->units = units;
    }
    sync(p, *t);
}

void TaskScheduling::setAssignmentWork(Project &p, int assignmentUid, qint64 workMillis)
{
    Assignment *a = assignmentByUid(p, assignmentUid);
    if (!a)
        return;
    Task *t = taskByUid(p, a->taskUniqueId);
    if (!t)
        return;
    a->workMillis = qMax<qint64>(0, workMillis);

    if (!isWorkAssignment(p, *a)) {
        if (const Resource *resource = resourceByUid(p, a->resourceUniqueId))
            if (resource->type == Resource::Type::Material) {
                a->units = double(a->workMillis) / double(kHour);
                a->variableRateUnits = 0;
            }
        a->remainingWorkMillis = qMax<qint64>(
            0, a->workMillis - a->actualWorkMillis);
        sync(p, *t);
        return;
    }

    if (t->taskType == 1) {
        // Fixed Duration: units absorb the new work.
        if (t->durationMillis > 0)
            a->units = qMax(kMinUnits, double(a->workMillis) / double(t->durationMillis));
    }
    // Fixed Units / Fixed Work: the span moves instead (sync derives it).
    sync(p, *t);
}

void TaskScheduling::setMaterialRate(Project &p, int assignmentUid, double units,
                                     int rateUnits, int costRateTable)
{
    Assignment *a = assignmentByUid(p, assignmentUid);
    if (!a)
        return;
    const Resource *resource = resourceByUid(p, a->resourceUniqueId);
    Task *task = taskByUid(p, a->taskUniqueId);
    if (!resource || resource->type != Resource::Type::Material || !task)
        return;
    a->units = qMax(0.0, units);
    a->variableRateUnits = qBound(0, rateUnits, 7);
    a->costRateTable = qBound(0, costRateTable, 4);
    a->timephasedValues.clear();
    a->actualWorkMillis = 0;
    a->remainingWorkMillis = 0;
    sync(p, *task);
}

bool TaskScheduling::setWorkContour(Project &p, int assignmentUid, int contour)
{
    Assignment *assignment = assignmentByUid(p, assignmentUid);
    if (!assignment || !isWorkAssignment(p, *assignment))
        return false;
    return WorkContouring::apply(p, assignmentUid, contour);
}

int TaskScheduling::addAssignment(Project &p, int taskUid, int resourceUid, double units)
{
    Task *t = taskByUid(p, taskUid);
    if (!t)
        return -1;
    const Resource *resource = nullptr;
    for (const Resource &r : p.resources)
        if (r.uniqueId == resourceUid) { resource = &r; break; }
    if (!resource)
        return -1;
    for (const Assignment &a : p.assignments)
        if (a.taskUniqueId == taskUid && a.resourceUniqueId == resourceUid)
            return -1;   // already assigned

    units = qMax(kMinUnits, units);
    const qint64 oldTotal = taskWork(p, taskUid);
    const bool effortDriven = t->effortDriven || t->taskType == 2;
    const bool hadAssignments = !assignmentsOf(p, taskUid).isEmpty();

    // A real resource replaces the task's "unassigned" placeholder row (negative
    // resource uid, as read from files). MS Project drops it at this point too;
    // leaving it in would double-count its (stale) work on export.
    for (int i = p.assignments.size() - 1; i >= 0; --i)
        if (p.assignments.at(i).taskUniqueId == taskUid
            && p.assignments.at(i).resourceUniqueId < 0)
            p.assignments.removeAt(i);

    int nextUid = 1;
    for (const Assignment &a : p.assignments)
        nextUid = qMax(nextUid, a.uniqueId + 1);

    Assignment a;
    a.uniqueId = nextUid;
    a.taskUniqueId = taskUid;
    a.resourceUniqueId = resourceUid;
    a.units = units;
    p.assignments.append(a);
    Assignment *added = &p.assignments.last();

    if (resource->type != Resource::Type::Work) {
        if (resource->type == Resource::Type::Material)
            added->workMillis = qint64(std::llround(units * double(kHour)));
        added->start = t->start;
        added->finish = t->finish;
        sync(p, *t);
        return added->uniqueId;
    }

    if (effortDriven && hadAssignments && oldTotal > 0) {
        // Total work stays put; every assignment gets its units' share.
        const QList<Assignment *> assns = assignmentsOf(p, taskUid);
        double unitsTotal = 0.0;
        for (const Assignment *x : assns)
            unitsTotal += qMax(kMinUnits, x->units);
        qint64 assigned = 0;
        for (int i = 0; i < assns.size(); ++i) {
            Assignment *x = assns.at(i);
            const qint64 share = (i == assns.size() - 1)
                ? oldTotal - assigned
                : qint64(std::llround(double(oldTotal) * qMax(kMinUnits, x->units) / unitsTotal));
            x->workMillis = qMax<qint64>(0, share);
            assigned += x->workMillis;
        }
        if (t->taskType == 1) {
            // Fixed Duration + effort-driven: the span holds, units re-derive.
            for (Assignment *x : assns)
                if (t->durationMillis > 0)
                    x->units = qMax(kMinUnits, double(x->workMillis) / double(t->durationMillis));
        }
    } else {
        // Non-effort-driven (or the first resource): the newcomer brings its
        // own work for the task's current span.
        added->workMillis = qint64(std::llround(double(t->durationMillis) * units));
    }

    sync(p, *t);
    return added->uniqueId;
}

void TaskScheduling::removeAssignment(Project &p, int assignmentUid)
{
    int idx = -1;
    for (int i = 0; i < p.assignments.size(); ++i)
        if (p.assignments.at(i).uniqueId == assignmentUid) { idx = i; break; }
    if (idx < 0)
        return;
    const Assignment removed = p.assignments.at(idx);
    Task *t = taskByUid(p, removed.taskUniqueId);
    p.assignments.removeAt(idx);
    if (!t)
        return;

    const Resource *removedResource = resourceByUid(p, removed.resourceUniqueId);
    if (removedResource && removedResource->type != Resource::Type::Work) {
        sync(p, *t);
        return;
    }

    const bool effortDriven = t->effortDriven || t->taskType == 2;
    const QList<Assignment *> assns = assignmentsOf(p, t->uniqueId);
    if (assns.isEmpty())
        t->workMillis = 0;   // last resource gone: the work went with it (sync,
                             // which holds task-level work when empty, won't clear it)
    if (effortDriven && !assns.isEmpty() && removed.workMillis > 0) {
        // The survivors absorb the departed work, proportionally to units.
        double unitsTotal = 0.0;
        for (const Assignment *x : assns)
            unitsTotal += qMax(kMinUnits, x->units);
        qint64 assigned = 0;
        for (int i = 0; i < assns.size(); ++i) {
            Assignment *x = assns.at(i);
            const qint64 extra = (i == assns.size() - 1)
                ? removed.workMillis - assigned
                : qint64(std::llround(double(removed.workMillis)
                                      * qMax(kMinUnits, x->units) / unitsTotal));
            x->workMillis += qMax<qint64>(0, extra);
            assigned += qMax<qint64>(0, extra);
        }
        if (t->taskType == 1) {
            for (Assignment *x : assns)
                if (t->durationMillis > 0)
                    x->units = qMax(kMinUnits, double(x->workMillis) / double(t->durationMillis));
        }
    }
    sync(p, *t);
}

} // namespace schedule
