// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/schedulingcalendar.h"
#include "model/duration.h"
#include "model/scheduler.h"

#include <QHash>

#include <cmath>
#include <optional>

namespace schedule::SchedulingCalendar {

struct IndexScope::Data
{
    const Project *project = nullptr;
    QHash<int, int> resourceIndex;               // resource uid -> first index in resources
    QHash<int, QVector<int>> workAssignments;    // task uid -> its work assignments, list order
    QHash<int, WorkCalendar> calendars;          // calendar uid -> resolved calendar
    std::optional<WorkCalendar> projectCalendar;
    QHash<QPair<int, int>, WorkCalendar> intersections;   // (task cal, resource cal)
};

namespace {

constexpr double kMinUnits = 0.01;

thread_local IndexScope::Data *t_index = nullptr;

// The active index, when it was built for this project.
IndexScope::Data *indexFor(const Project &project)
{
    return t_index && t_index->project == &project ? t_index : nullptr;
}

const Resource *resourceByUid(const Project &project, int uid)
{
    if (const IndexScope::Data *index = indexFor(project)) {
        const auto it = index->resourceIndex.constFind(uid);
        return it == index->resourceIndex.cend() ? nullptr : &project.resources.at(it.value());
    }
    for (const Resource &resource : project.resources)
        if (resource.uniqueId == uid)
            return &resource;
    return nullptr;
}

QList<const Assignment *> workAssignments(const Project &project, const Task &task)
{
    QList<const Assignment *> out;
    if (const IndexScope::Data *index = indexFor(project)) {
        const auto it = index->workAssignments.constFind(task.uniqueId);
        if (it != index->workAssignments.cend())
            for (const int i : it.value())
                out.append(&project.assignments.at(i));
        return out;
    }
    for (const Assignment &a : project.assignments) {
        if (a.taskUniqueId != task.uniqueId)
            continue;
        const Resource *resource = resourceByUid(project, a.resourceUniqueId);
        if (resource && resource->type == Resource::Type::Work)
            out.append(&a);
    }
    return out;
}

WorkCalendar resolvedCalendar(const Project &project, int calendarUniqueId)
{
    IndexScope::Data *index = indexFor(project);
    if (!index)
        return WorkCalendar(project, calendarUniqueId);
    auto it = index->calendars.find(calendarUniqueId);
    if (it == index->calendars.end())
        it = index->calendars.insert(calendarUniqueId, WorkCalendar(project, calendarUniqueId));
    return it.value();
}

qint64 span(const Assignment &assignment)
{
    return qint64(std::llround(double(assignment.workMillis)
                               / qMax(kMinUnits, assignment.units)));
}

} // namespace

IndexScope::IndexScope(const Project &project)
    : m_previous(t_index)
{
    if (t_index && t_index->project == &project)
        return;   // an outer scope already indexes this project
    m_data = new Data;
    m_data->project = &project;
    const QList<Resource> &resources = project.resources;
    m_data->resourceIndex.reserve(resources.size());
    for (int i = 0; i < resources.size(); ++i)
        if (!m_data->resourceIndex.contains(resources.at(i).uniqueId))
            m_data->resourceIndex.insert(resources.at(i).uniqueId, i);
    const QList<Assignment> &assignments = project.assignments;
    for (int i = 0; i < assignments.size(); ++i) {
        const Assignment &a = assignments.at(i);
        const auto r = m_data->resourceIndex.constFind(a.resourceUniqueId);
        if (r != m_data->resourceIndex.cend()
            && resources.at(r.value()).type == Resource::Type::Work)
            m_data->workAssignments[a.taskUniqueId].append(i);
    }
    t_index = m_data;
}

IndexScope::~IndexScope()
{
    if (m_data) {
        t_index = m_previous;
        delete m_data;
    }
}

WorkCalendar taskBase(const Project &project, const Task &task)
{
    if (task.calendarUniqueId >= 0)
        return resolvedCalendar(project, task.calendarUniqueId);
    if (IndexScope::Data *index = indexFor(project)) {
        if (!index->projectCalendar)
            index->projectCalendar = Scheduler::projectCalendar(project);
        return *index->projectCalendar;
    }
    return Scheduler::projectCalendar(project);
}

WorkCalendar assignment(const Project &project, const Task &task,
                        const Assignment &workAssignment)
{
    const WorkCalendar base = taskBase(project, task);
    const Resource *resource = resourceByUid(project, workAssignment.resourceUniqueId);
    if (task.manual || task.ignoreResourceCalendar || !resource
        || resource->type != Resource::Type::Work || resource->calendarUniqueId < 0)
        return base;
    IndexScope::Data *index = indexFor(project);
    if (!index)
        return WorkCalendar::intersection({ base, WorkCalendar(project, resource->calendarUniqueId) });
    const QPair<int, int> key(task.calendarUniqueId, resource->calendarUniqueId);
    auto it = index->intersections.find(key);
    if (it == index->intersections.end())
        it = index->intersections.insert(key, WorkCalendar::intersection(
            { base, resolvedCalendar(project, resource->calendarUniqueId) }));
    return it.value();
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
