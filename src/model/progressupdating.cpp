// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/progressupdating.h"

#include "model/duration.h"
#include "model/project.h"
#include "model/projectreconciliation.h"
#include "model/resource.h"
#include "model/schedulingcalendar.h"
#include "model/scheduler.h"
#include "model/timephasedvalue.h"

#include <QHash>
#include <QtGlobal>

#include <algorithm>
#include <cmath>
#include <utility>

namespace schedule {

namespace {

constexpr double kMinUnits = 0.01;

QString isoDuration(qint64 millis)
{
    quint64 seconds = quint64(qMax<qint64>(0, millis)) / 1000;
    const quint64 hours = seconds / 3600;
    seconds %= 3600;
    const quint64 minutes = seconds / 60;
    seconds %= 60;
    return QStringLiteral("PT%1H%2M%3S").arg(hours).arg(minutes).arg(seconds);
}

const Resource *resourceByUid(const Project &project, int uid)
{
    for (const Resource &resource : project.resources)
        if (resource.uniqueId == uid)
            return &resource;
    return nullptr;
}

bool isWorkAssignment(const Project &project, const Assignment &assignment)
{
    const Resource *resource = resourceByUid(project, assignment.resourceUniqueId);
    return resource && resource->type == Resource::Type::Work;
}

bool isProgressAssignment(const Project &project, const Assignment &assignment)
{
    const Resource *resource = resourceByUid(project, assignment.resourceUniqueId);
    return resource && resource->type != Resource::Type::Cost;
}

Task *taskByUid(Project &project, int uid)
{
    for (Task &task : project.tasks)
        if (task.uniqueId == uid)
            return &task;
    return nullptr;
}

double scheduledPercent(const Project &project, const Task &task,
                        const QDateTime &through)
{
    if (!task.start.isValid() || !task.finish.isValid() || through < task.start)
        return 0.0;
    if (task.milestone || task.durationMillis <= 0)
        return through >= task.finish ? 1.0 : 0.0;
    if (through >= task.finish)
        return 1.0;
    const qint64 elapsed = Duration::isElapsed(task.durationFormat)
        ? task.start.msecsTo(through)
        : SchedulingCalendar::taskBase(project, task).workBetween(task.start, through);
    return qBound(0.0, double(qMax<qint64>(0, elapsed))
                            / double(task.durationMillis), 1.0);
}

void appendProgressBucket(Assignment &assignment, int type,
                          const QDateTime &start, const QDateTime &finish,
                          qint64 value)
{
    if (value <= 0 || !start.isValid() || !finish.isValid() || finish <= start)
        return;
    TimephasedValue bucket;
    bucket.type = type;
    bucket.uniqueId = assignment.uniqueId;
    bucket.start = start;
    bucket.finish = finish;
    bucket.unit = 1;
    bucket.value = isoDuration(value);
    assignment.timephasedValues.append(bucket);
}

void updateAssignmentProgress(Project &project, const Task &task,
                              Assignment &assignment, double percent,
                              bool workOnly = false,
                              const QDateTime &boundaryHint = {})
{
    if (workOnly ? !isWorkAssignment(project, assignment)
                 : !isProgressAssignment(project, assignment))
        return;
    const qint64 total = qMax<qint64>(
        0, qMax(assignment.workMillis,
                assignment.actualWorkMillis + assignment.remainingWorkMillis));
    const qint64 actual = qBound<qint64>(
        0, qint64(std::llround(double(total) * percent)), total);
    const qint64 remaining = total - actual;
    const qint64 overtimeTotal = qMax<qint64>(
        0, qMax(assignment.overtimeWorkMillis,
                assignment.actualOvertimeWorkMillis
                    + assignment.remainingOvertimeWorkMillis));
    assignment.actualOvertimeWorkMillis = qBound<qint64>(
        0, qint64(std::llround(double(overtimeTotal) * percent)), overtimeTotal);
    assignment.remainingOvertimeWorkMillis = overtimeTotal
        - assignment.actualOvertimeWorkMillis;
    assignment.overtimeWorkMillis = overtimeTotal;

    QList<TimephasedValue> retained;
    for (const TimephasedValue &bucket : std::as_const(assignment.timephasedValues))
        if (bucket.type != TimephasedValue::ActualWork
            && bucket.type != TimephasedValue::ActualOvertimeWork
            && bucket.type != TimephasedValue::RemainingWork)
            retained.append(bucket);
    assignment.timephasedValues = retained;

    const WorkCalendar calendar = SchedulingCalendar::assignment(
        project, task, assignment);
    const QDateTime start = assignment.start.isValid() ? assignment.start : task.start;
    const QDateTime finish = assignment.finish.isValid() ? assignment.finish : task.finish;
    QDateTime boundary = start;
    if (percent >= 1.0) {
        boundary = finish;
    } else if (percent > 0.0 && boundaryHint.isValid()
               && start < boundaryHint && boundaryHint < finish) {
        boundary = boundaryHint;
    } else if (percent > 0.0) {
        const qint64 span = calendar.workBetween(start, finish);
        boundary = span > 0
            ? calendar.addWork(start, qint64(std::llround(double(span) * percent)))
            : start.addMSecs(qint64(std::llround(double(start.msecsTo(finish))
                                                 * percent)));
    }
    appendProgressBucket(assignment, TimephasedValue::ActualWork,
                         start, boundary, actual);
    appendProgressBucket(assignment, TimephasedValue::ActualOvertimeWork,
                         start, boundary, assignment.actualOvertimeWorkMillis);
    const QDateTime remainingStart = percent <= 0.0
        ? start : calendar.nextWorkStart(boundary);
    appendProgressBucket(assignment, TimephasedValue::RemainingWork,
                         remainingStart, finish, remaining);

    assignment.actualWorkMillis = actual;
    assignment.remainingWorkMillis = remaining;
    assignment.workMillis = total;
    assignment.stop = actual > 0 ? boundary : QDateTime();
    assignment.resume = remaining > 0 ? remainingStart : QDateTime();
}

qint64 remainingSpan(const Assignment &assignment)
{
    return qint64(std::llround(double(qMax<qint64>(0, assignment.remainingWorkMillis))
                               / qMax(kMinUnits, assignment.units)));
}

QDateTime firstActualStart(const Assignment &assignment)
{
    QDateTime result;
    for (const TimephasedValue &value : assignment.timephasedValues)
        if ((value.type == TimephasedValue::ActualWork
             || value.type == TimephasedValue::ActualOvertimeWork)
            && value.start.isValid() && (!result.isValid() || value.start < result))
            result = value.start;
    return result;
}

QDateTime lastActualFinish(const Assignment &assignment)
{
    QDateTime result;
    for (const TimephasedValue &value : assignment.timephasedValues)
        if ((value.type == TimephasedValue::ActualWork
             || value.type == TimephasedValue::ActualOvertimeWork)
            && value.finish.isValid() && (!result.isValid() || result < value.finish))
            result = value.finish;
    return result;
}

QDateTime firstRemainingStart(const Assignment &assignment)
{
    QDateTime result;
    for (const TimephasedValue &value : assignment.timephasedValues)
        if (value.type == TimephasedValue::RemainingWork && value.start.isValid()
            && (!result.isValid() || value.start < result))
            result = value.start;
    if (!result.isValid())
        result = assignment.resume.isValid() ? assignment.resume
                                              : assignment.start;
    return result;
}

// Move the complete remaining stream to `resume`, preserving every bucket's
// value and ordering. Calendar work spans are retained so a Friday move rolls
// the next bucket to Monday instead of placing it on Saturday.
void moveRemainingBuckets(Assignment &assignment, const WorkCalendar &calendar,
                          const QDateTime &resume)
{
    QList<int> indices;
    for (int i = 0; i < assignment.timephasedValues.size(); ++i)
        if (assignment.timephasedValues.at(i).type == TimephasedValue::RemainingWork)
            indices.append(i);
    std::sort(indices.begin(), indices.end(), [&](int left, int right) {
        return assignment.timephasedValues.at(left).start
            < assignment.timephasedValues.at(right).start;
    });

    QDateTime cursor = calendar.nextWorkStart(resume);
    for (int index : indices) {
        TimephasedValue &value = assignment.timephasedValues[index];
        qint64 span = value.start.isValid() && value.finish > value.start
            ? calendar.workBetween(value.start, value.finish) : 0;
        if (span <= 0)
            span = qint64(std::llround(double(qMax<qint64>(0, value.durationMillis()))
                                       / qMax(kMinUnits, assignment.units)));
        value.start = cursor;
        value.finish = span > 0 ? calendar.addWork(cursor, span) : cursor;
        cursor = calendar.nextWorkStart(value.finish);
    }
}

} // namespace

namespace {

bool finishTrackingEdit(Project &project)
{
    ProjectReconciliation::reconcile(project);
    Scheduler::computeSlack(project);
    return true;
}

void setDurationProgress(Project &project, Task &task, qint64 actual,
                         qint64 remaining)
{
    actual = qMax<qint64>(0, actual);
    remaining = qMax<qint64>(0, remaining);
    task.durationMillis = actual + remaining;
    task.actualDurationMillis = actual;
    if (task.start.isValid())
        task.finish = SchedulingCalendar::finish(project, task, task.start);
    const double percent = task.durationMillis > 0
        ? double(actual) / double(task.durationMillis)
        : (remaining == 0 ? 1.0 : 0.0);
    task.percentComplete = qBound(0.0, percent, 1.0);
    if (actual > 0 && !task.actualStart.isValid())
        task.actualStart = task.start;
    if (actual <= 0)
        task.actualStart = {};
    if (remaining == 0 && task.percentComplete >= 1.0)
        task.actualFinish = task.finish;
    else
        task.actualFinish = {};
    for (Assignment &assignment : project.assignments)
        if (assignment.taskUniqueId == task.uniqueId) {
            assignment.finish = task.finish;
            updateAssignmentProgress(project, task, assignment,
                                     task.percentComplete, true,
                                     project.statusDate);
        }
}

bool setWorkProgress(Project &project, Task &task, qint64 actual,
                     qint64 remaining)
{
    actual = qMax<qint64>(0, actual);
    remaining = qMax<qint64>(0, remaining);
    const qint64 total = actual + remaining;
    QList<Assignment *> workAssignments;
    qint64 oldTotal = 0;
    for (Assignment &assignment : project.assignments) {
        if (assignment.taskUniqueId != task.uniqueId
            || !isWorkAssignment(project, assignment))
            continue;
        workAssignments.append(&assignment);
        oldTotal += qMax<qint64>(0, assignment.workMillis);
    }

    if (workAssignments.isEmpty()) {
        task.workMillis = total;
        task.actualWorkMillis = actual;
    } else {
        qint64 allocated = 0;
        for (int i = 0; i < workAssignments.size(); ++i) {
            Assignment &assignment = *workAssignments.at(i);
            const qint64 share = i + 1 == workAssignments.size()
                ? total - allocated
                : (oldTotal > 0
                   ? qint64(std::llround(double(total)
                         * double(qMax<qint64>(0, assignment.workMillis))
                         / double(oldTotal)))
                   : total / workAssignments.size());
            assignment.workMillis = qMax<qint64>(0, share);
            assignment.actualWorkMillis = 0;
            assignment.remainingWorkMillis = assignment.workMillis;
            allocated += assignment.workMillis;
        }
        const double percent = total > 0 ? double(actual) / double(total) : 0.0;
        for (Assignment *assignment : workAssignments)
            updateAssignmentProgress(project, task, *assignment, percent, true,
                                     project.statusDate);
    }

    const double percent = total > 0 ? double(actual) / double(total) : 0.0;
    task.percentComplete = qBound(0.0, percent, 1.0);
    task.actualDurationMillis = qint64(std::llround(
        double(qMax<qint64>(0, task.durationMillis)) * task.percentComplete));
    if (actual > 0 && !task.actualStart.isValid())
        task.actualStart = task.start;
    if (remaining == 0 && total > 0)
        task.actualFinish = task.finish;
    else
        task.actualFinish = {};
    return finishTrackingEdit(project);
}

} // namespace

bool ProgressUpdating::setActualStart(Project &project, int taskUid,
                                      const QDateTime &actualStart)
{
    Task *task = taskByUid(project, taskUid);
    if (!task || task->summary || !actualStart.isValid())
        return false;
    task->actualStart = actualStart;
    task->start = actualStart;
    if (task->durationMillis > 0)
        task->finish = SchedulingCalendar::finish(project, *task, actualStart);
    for (Assignment &assignment : project.assignments)
        if (assignment.taskUniqueId == taskUid)
            assignment.start = actualStart;
    return finishTrackingEdit(project);
}

bool ProgressUpdating::setActualFinish(Project &project, int taskUid,
                                       const QDateTime &actualFinish)
{
    Task *task = taskByUid(project, taskUid);
    if (!task || task->summary || !actualFinish.isValid())
        return false;
    if (!task->actualStart.isValid())
        task->actualStart = task->start;
    task->actualFinish = actualFinish;
    task->finish = actualFinish;
    task->durationMillis = Duration::isElapsed(task->durationFormat)
        ? qMax<qint64>(0, task->start.msecsTo(actualFinish))
        : SchedulingCalendar::taskBase(project, *task)
              .workBetween(task->start, actualFinish);
    task->actualDurationMillis = task->durationMillis;
    task->percentComplete = 1.0;
    for (Assignment &assignment : project.assignments) {
        if (assignment.taskUniqueId != taskUid)
            continue;
        assignment.finish = actualFinish;
        updateAssignmentProgress(project, *task, assignment, 1.0);
    }
    return finishTrackingEdit(project);
}

bool ProgressUpdating::setActualDuration(Project &project, int taskUid,
                                         qint64 millis)
{
    Task *task = taskByUid(project, taskUid);
    if (!task || task->summary || millis < 0)
        return false;
    const qint64 total = qMax(task->durationMillis, millis);
    setDurationProgress(project, *task, millis, total - millis);
    return finishTrackingEdit(project);
}

bool ProgressUpdating::setRemainingDuration(Project &project, int taskUid,
                                            qint64 millis)
{
    Task *task = taskByUid(project, taskUid);
    if (!task || task->summary || millis < 0)
        return false;
    setDurationProgress(project, *task, task->actualDurationMillis, millis);
    return finishTrackingEdit(project);
}

bool ProgressUpdating::setActualWork(Project &project, int taskUid,
                                     qint64 millis)
{
    Task *task = taskByUid(project, taskUid);
    if (!task || task->summary || millis < 0)
        return false;
    const qint64 total = qMax(task->workMillis, millis);
    return setWorkProgress(project, *task, millis, total - millis);
}

bool ProgressUpdating::setRemainingWork(Project &project, int taskUid,
                                        qint64 millis)
{
    Task *task = taskByUid(project, taskUid);
    if (!task || task->summary || millis < 0)
        return false;
    return setWorkProgress(project, *task, task->actualWorkMillis, millis);
}

bool ProgressUpdating::setPercentWorkComplete(Project &project, int taskUid,
                                              double percent)
{
    if (!std::isfinite(percent) || percent < 0.0 || percent > 1.0)
        return false;
    Task *task = nullptr;
    for (Task &candidate : project.tasks)
        if (candidate.uniqueId == taskUid) {
            task = &candidate;
            break;
        }
    if (!task || task->summary)
        return false;

    bool hasWorkAssignment = false;
    for (Assignment &assignment : project.assignments) {
        if (assignment.taskUniqueId != taskUid
            || !isWorkAssignment(project, assignment))
            continue;
        hasWorkAssignment = true;
        updateAssignmentProgress(project, *task, assignment, percent, true,
                                 project.statusDate);
    }
    if (!hasWorkAssignment)
        task->actualWorkMillis = qint64(std::llround(
            double(qMax<qint64>(0, task->workMillis)) * percent));

    task->percentComplete = percent;
    task->actualDurationMillis = qint64(std::llround(
        double(qMax<qint64>(0, task->durationMillis)) * percent));
    if (percent > 0.0 && !task->actualStart.isValid())
        task->actualStart = task->start;
    else if (percent <= 0.0)
        task->actualStart = {};
    if (percent >= 1.0)
        task->actualFinish = task->finish;
    else
        task->actualFinish = {};

    ProjectReconciliation::reconcile(project);
    Scheduler::computeSlack(project);
    return true;
}

int ProgressUpdating::updateScheduledProgress(Project &project,
                                              const QDateTime &through,
                                              UpdateAction action,
                                              const QSet<int> &taskUids)
{
    if (!through.isValid())
        return 0;

    int changed = 0;
    for (Task &task : project.tasks) {
        if (task.summary || !task.active
            || (!taskUids.isEmpty() && !taskUids.contains(task.uniqueId)))
            continue;

        const double recorded = qBound(0.0, task.percentComplete, 1.0);
        const double percent = action == UpdateAction::ZeroOrOneHundred
            ? (task.finish.isValid() && task.finish <= through ? 1.0 : recorded)
            : qMax(recorded, scheduledPercent(project, task, through));
        // Update Project advances scheduled progress. It does not rewrite a task
        // when the requested action would leave its recorded percentage unchanged.
        if (percent <= recorded + 1e-9)
            continue;
        const QDateTime actualStart = percent > 0.0 ? task.start : QDateTime();
        const QDateTime actualFinish = percent >= 1.0 ? task.finish : QDateTime();
        const qint64 actualDuration = qBound<qint64>(
            0, qint64(std::llround(double(task.durationMillis) * percent)),
            task.durationMillis);
        task.percentComplete = percent;
        task.actualStart = actualStart;
        task.actualFinish = actualFinish;
        task.actualDurationMillis = actualDuration;

        bool hasWorkAssignment = false;
        for (Assignment &assignment : project.assignments) {
            if (assignment.taskUniqueId != task.uniqueId)
                continue;
            hasWorkAssignment |= isWorkAssignment(project, assignment);
            updateAssignmentProgress(project, task, assignment, percent);
        }
        if (!hasWorkAssignment)
            task.actualWorkMillis = qint64(std::llround(
                double(qMax<qint64>(0, task.workMillis)) * percent));
        ++changed;
    }

    ProjectReconciliation::reconcile(project);
    Scheduler::computeSlack(project);
    return changed;
}

int ProgressUpdating::rescheduleIncompleteWork(Project &project,
                                               const QDateTime &after,
                                               const QSet<int> &taskUids)
{
    if (!after.isValid())
        return 0;

    // Begin with the normal dependency/constraint result, then protect each
    // status-moved task while a final pass propagates its new finish downstream.
    Scheduler::reschedule(project);

    int changed = 0;
    QSet<int> protectUnstarted;
    for (Task &task : project.tasks) {
        if (task.summary || task.manual || !task.active || task.actualFinish.isValid()
            || task.percentComplete >= 1.0
            || (!taskUids.isEmpty() && !taskUids.contains(task.uniqueId)))
            continue;

        QList<Assignment *> assignments;
        bool hasRemaining = false;
        bool hasActual = task.actualStart.isValid() || task.actualWorkMillis > 0
            || task.actualDurationMillis > 0 || task.percentComplete > 0.0;
        for (Assignment &assignment : project.assignments) {
            if (assignment.taskUniqueId != task.uniqueId
                || !isWorkAssignment(project, assignment))
                continue;
            assignments.append(&assignment);
            hasRemaining |= assignment.remainingWorkMillis > 0;
            hasActual |= assignment.actualWorkMillis > 0;
        }
        if (!assignments.isEmpty() && !hasRemaining)
            continue;

        const QDateTime oldStart = task.start;
        const QDateTime oldFinish = task.finish;
        const WorkCalendar taskCalendar = SchedulingCalendar::taskBase(project, task);
        const QDateTime taskResume = SchedulingCalendar::nextStart(project, task, after);
        if (!taskResume.isValid())
            continue;

        QDateTime earliestStart;
        QDateTime latestFinish;
        qint64 maxRemainingSpan = 0;
        bool anyAssignmentMoved = false;
        for (Assignment *assignment : assignments) {
            if (assignment->remainingWorkMillis <= 0)
                continue;
            const WorkCalendar calendar = SchedulingCalendar::assignment(
                project, task, *assignment);
            const QDateTime resume = calendar.nextWorkStart(after);
            if (!resume.isValid())
                continue;
            const QDateTime existingRemaining = firstRemainingStart(*assignment);
            if (existingRemaining.isValid() && existingRemaining >= resume) {
                maxRemainingSpan = qMax(maxRemainingSpan, remainingSpan(*assignment));
                if (assignment->start.isValid()
                    && (!earliestStart.isValid() || assignment->start < earliestStart))
                    earliestStart = assignment->start;
                if (assignment->finish.isValid()
                    && (!latestFinish.isValid() || latestFinish < assignment->finish))
                    latestFinish = assignment->finish;
                continue;
            }
            anyAssignmentMoved = true;

            if (!hasActual) {
                assignment->start = resume;
                assignment->stop = QDateTime();
                assignment->resume = QDateTime();
            } else {
                if (!assignment->start.isValid())
                    assignment->start = task.actualStart.isValid()
                        ? task.actualStart : oldStart;
                QDateTime stop = lastActualFinish(*assignment);
                if (!stop.isValid())
                    stop = assignment->stop;
                if (!stop.isValid() && assignment->actualWorkMillis > 0)
                    stop = calendar.addWork(
                        assignment->start,
                        qint64(std::llround(double(assignment->actualWorkMillis)
                                           / qMax(kMinUnits, assignment->units))));
                assignment->stop = stop;
                assignment->resume = resume;
            }

            moveRemainingBuckets(*assignment, calendar, resume);
            const qint64 span = remainingSpan(*assignment);
            assignment->finish = span > 0 ? calendar.addWork(resume, span) : resume;
            maxRemainingSpan = qMax(maxRemainingSpan, span);
            if (!earliestStart.isValid() || assignment->start < earliestStart)
                earliestStart = assignment->start;
            if (!latestFinish.isValid() || latestFinish < assignment->finish)
                latestFinish = assignment->finish;
        }

        if (!assignments.isEmpty() && !anyAssignmentMoved)
            continue;
        if (assignments.isEmpty()) {
            QDateTime existingRemaining = task.start;
            if (hasActual && task.actualStart.isValid())
                existingRemaining = Duration::isElapsed(task.durationFormat)
                    ? task.actualStart.addMSecs(task.actualDurationMillis)
                    : taskCalendar.addWork(task.actualStart, task.actualDurationMillis);
            if (existingRemaining.isValid() && existingRemaining >= taskResume)
                continue;
        }

        if (hasActual) {
            if (!task.actualStart.isValid()) {
                for (const Assignment *assignment : assignments) {
                    QDateTime value = firstActualStart(*assignment);
                    if (!value.isValid())
                        value = assignment->start;
                    if (value.isValid() && (!task.actualStart.isValid()
                                            || value < task.actualStart))
                        task.actualStart = value;
                }
                if (!task.actualStart.isValid())
                    task.actualStart = oldStart;
            }
            if (task.actualDurationMillis <= 0 && task.durationMillis > 0)
                task.actualDurationMillis = qint64(std::llround(
                    double(task.durationMillis) * qBound(0.0, task.percentComplete, 1.0)));
            task.durationMillis = task.actualDurationMillis + maxRemainingSpan;
        } else {
            task.start = earliestStart.isValid() ? earliestStart : taskResume;
            protectUnstarted.insert(task.uniqueId);
        }

        if (latestFinish.isValid()) {
            task.finish = latestFinish;
        } else {
            const qint64 remainingDuration = hasActual
                ? qMax<qint64>(0, task.durationMillis - task.actualDurationMillis)
                : task.durationMillis;
            task.finish = Duration::isElapsed(task.durationFormat)
                ? taskResume.addMSecs(remainingDuration)
                : taskCalendar.addWork(taskResume, remainingDuration);
            if (!hasActual)
                task.start = taskResume;
        }

        if (task.start != oldStart || task.finish != oldFinish)
            ++changed;
    }

    // Scheduler normally moves unstarted tasks back to their predecessor date.
    // Mark just the status-moved roots as temporarily started so their new dates
    // remain fixed while successors are recalculated from their new finishes.
    QHash<int, QDateTime> temporaryActualStarts;
    for (Task &task : project.tasks) {
        if (!protectUnstarted.contains(task.uniqueId))
            continue;
        temporaryActualStarts.insert(task.uniqueId, task.actualStart);
        task.actualStart = task.start;
    }
    Scheduler::reschedule(project);
    for (Task &task : project.tasks)
        if (temporaryActualStarts.contains(task.uniqueId))
            task.actualStart = temporaryActualStarts.value(task.uniqueId);

    ProjectReconciliation::reconcile(project);
    Scheduler::computeSlack(project);
    return changed;
}

} // namespace schedule
