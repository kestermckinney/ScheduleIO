// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/schedulingcalendar.h"
#include "model/duration.h"
#include "model/scheduler.h"

#include <cmath>

namespace schedule::SchedulingCalendar {

namespace {

constexpr double kMinUnits = 0.01;

const Resource *resourceByUid(const Project &project, int uid)
{
    for (const Resource &resource : project.resources)
        if (resource.uniqueId == uid)
            return &resource;
    return nullptr;
}

QList<const Assignment *> workAssignments(const Project &project, const Task &task)
{
    QList<const Assignment *> out;
    for (const Assignment &a : project.assignments) {
        if (a.taskUniqueId != task.uniqueId)
            continue;
        const Resource *resource = resourceByUid(project, a.resourceUniqueId);
        if (resource && resource->type == Resource::Type::Work)
            out.append(&a);
    }
    return out;
}

qint64 span(const Assignment &assignment)
{
    return qint64(std::llround(double(assignment.workMillis)
                               / qMax(kMinUnits, assignment.units)));
}

} // namespace

WorkCalendar taskBase(const Project &project, const Task &task)
{
    return task.calendarUniqueId >= 0 ? WorkCalendar(project, task.calendarUniqueId)
                                      : Scheduler::projectCalendar(project);
}

WorkCalendar assignment(const Project &project, const Task &task,
                        const Assignment &workAssignment)
{
    const WorkCalendar base = taskBase(project, task);
    const Resource *resource = resourceByUid(project, workAssignment.resourceUniqueId);
    if (task.manual || task.ignoreResourceCalendar || !resource
        || resource->type != Resource::Type::Work || resource->calendarUniqueId < 0)
        return base;
    return WorkCalendar::intersection({ base, WorkCalendar(project, resource->calendarUniqueId) });
}

QDateTime nextStart(const Project &project, const Task &task, const QDateTime &candidate)
{
    // Elapsed durations run continuously and may start during nonworking time.
    if (Duration::isElapsed(task.durationFormat))
        return candidate;

    const QList<const Assignment *> assignments = workAssignments(project, task);
    if (assignments.isEmpty())
        return taskBase(project, task).nextWorkStart(candidate);

    QDateTime earliest;
    for (const Assignment *a : assignments) {
        const QDateTime value = assignment(project, task, *a).nextWorkStart(candidate);
        if (value.isValid() && (!earliest.isValid() || value < earliest))
            earliest = value;
    }
    return earliest.isValid() ? earliest : candidate;
}

QDateTime finish(const Project &project, const Task &task, const QDateTime &start)
{
    // Split portions are explicit dates. When logic moves the task, preserve
    // the portion spacing by translating the complete span to the new start.
    if (task.segments.size() >= 2 && task.segments.first().start.isValid()
        && task.segments.last().finish > task.segments.first().start) {
        return start.addMSecs(task.segments.first().start.msecsTo(
            task.segments.last().finish));
    }
    if (Duration::isElapsed(task.durationFormat))
        return task.durationMillis > 0 ? start.addMSecs(task.durationMillis) : start;

    const QList<const Assignment *> assignments = workAssignments(project, task);
    if (assignments.isEmpty())
        return task.durationMillis > 0
            ? taskBase(project, task).addWork(start, task.durationMillis) : start;

    QDateTime latest;
    for (const Assignment *a : assignments) {
        const WorkCalendar calendar = assignment(project, task, *a);
        QDateTime assignmentStart = calendar.nextWorkStart(start);
        const qint64 delay = a->delayMillis + a->levelingDelayMillis;
        if (delay != 0)
            assignmentStart = calendar.addWork(assignmentStart, delay);
        const QDateTime value = a->workMillis > 0
            ? calendar.addWork(assignmentStart, span(*a)) : assignmentStart;
        if (value.isValid() && (!latest.isValid() || value > latest))
            latest = value;
    }
    return latest.isValid() ? latest : start;
}

QDateTime startForFinish(const Project &project, const Task &task,
                         const QDateTime &finishDate)
{
    if (task.segments.size() >= 2 && task.segments.first().start.isValid()
        && task.segments.last().finish > task.segments.first().start) {
        return finishDate.addMSecs(-task.segments.first().start.msecsTo(
            task.segments.last().finish));
    }
    if (Duration::isElapsed(task.durationFormat))
        return task.durationMillis > 0 ? finishDate.addMSecs(-task.durationMillis)
                                       : finishDate;

    const QList<const Assignment *> assignments = workAssignments(project, task);
    if (assignments.isEmpty())
        return task.durationMillis > 0
            ? taskBase(project, task).addWork(finishDate, -task.durationMillis) : finishDate;

    QDateTime earliest;
    for (const Assignment *a : assignments) {
        const WorkCalendar calendar = assignment(project, task, *a);
        QDateTime value = a->workMillis > 0
            ? calendar.addWork(finishDate, -span(*a)) : finishDate;
        const qint64 delay = a->delayMillis + a->levelingDelayMillis;
        if (delay != 0)
            value = calendar.addWork(value, -delay);
        if (value.isValid() && (!earliest.isValid() || value < earliest))
            earliest = value;
    }
    return earliest.isValid() ? earliest : finishDate;
}

} // namespace schedule::SchedulingCalendar
