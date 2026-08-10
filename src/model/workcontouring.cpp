// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/workcontouring.h"

#include "model/schedulingcalendar.h"
#include "model/scheduler.h"
#include "model/workcalendar.h"

#include <QtGlobal>
#include <QTimeZone>
#include <algorithm>
#include <cmath>

namespace schedule {

namespace {

QString isoDuration(qint64 millis)
{
    quint64 seconds = quint64(qMax<qint64>(0, millis)) / 1000;
    const quint64 hours = seconds / 3600;
    seconds %= 3600;
    const quint64 minutes = seconds / 60;
    seconds %= 60;
    return QStringLiteral("PT%1H%2M%3S").arg(hours).arg(minutes).arg(seconds);
}

Assignment *assignmentByUid(Project &project, int uid)
{
    for (Assignment &assignment : project.assignments)
        if (assignment.uniqueId == uid)
            return &assignment;
    return nullptr;
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

WorkCalendar assignmentCalendar(const Project &project, const Assignment &assignment)
{
    if (const Task *task = taskByUid(project, assignment.taskUniqueId))
        return SchedulingCalendar::assignment(project, *task, assignment);
    return Scheduler::projectCalendar(project);
}

struct Slice {
    QDateTime start;
    QDateTime finish;
    qint64 basis = 0;
    double weightedBasis = 0.0;
};

QList<Slice> workingSlices(const WorkCalendar &calendar,
                           const QDateTime &start, const QDateTime &finish)
{
    QList<Slice> slices;
    for (QDate date = start.date(); date <= finish.date(); date = date.addDays(1)) {
        for (const TimeRange &range : calendar.workingTimes(date)) {
            QDateTime sliceStart(date, range.start, start.timeZone());
            QDateTime sliceFinish(date.addDays(range.end == QTime(0, 0) ? 1 : 0),
                                  range.end, start.timeZone());
            sliceStart = qMax(sliceStart, start);
            sliceFinish = qMin(sliceFinish, finish);
            if (sliceFinish > sliceStart)
                slices.append({sliceStart, sliceFinish,
                               sliceStart.msecsTo(sliceFinish), 0.0});
        }
    }
    if (slices.isEmpty() && finish > start)
        slices.append({start, finish, start.msecsTo(finish), 0.0});
    return slices;
}

bool rebuild(Project &project, Assignment &assignment)
{
    if (assignment.workContour == WorkContouring::Contoured)
        return true;
    const QDateTime start = assignment.resume.isValid()
        ? assignment.resume : assignment.start;
    const QDateTime finish = assignment.finish;
    if (!start.isValid() || !finish.isValid() || finish <= start)
        return false;

    const qint64 total = qMax<qint64>(
        0, assignment.remainingWorkMillis > 0
               ? assignment.remainingWorkMillis
               : assignment.workMillis - assignment.actualWorkMillis);
    QList<TimephasedValue> retained;
    for (const TimephasedValue &value : assignment.timephasedValues)
        if (value.type != TimephasedValue::RemainingWork)
            retained.append(value);
    if (total == 0) {
        assignment.timephasedValues = retained;
        return true;
    }

    const WorkCalendar calendar = assignmentCalendar(project, assignment);
    QList<Slice> slices = workingSlices(calendar, start, finish);
    qint64 basisTotal = 0;
    for (const Slice &slice : slices)
        basisTotal += slice.basis;
    if (basisTotal <= 0)
        return false;

    const QVector<int> percentages =
        WorkContouring::segmentPercentages(assignment.workContour);
    qint64 elapsed = 0;
    double weightTotal = 0.0;
    for (Slice &slice : slices) {
        const double midpoint = double(elapsed) + double(slice.basis) / 2.0;
        const int segment = qBound(0, int(std::floor(midpoint * 10.0
                                                     / double(basisTotal))), 9);
        slice.weightedBasis = double(slice.basis) * percentages.value(segment, 100);
        weightTotal += slice.weightedBasis;
        elapsed += slice.basis;
    }
    if (weightTotal <= 0.0)
        return false;

    qint64 allocated = 0;
    for (int i = 0; i < slices.size(); ++i) {
        const Slice &slice = slices.at(i);
        const qint64 amount = i + 1 == slices.size()
            ? total - allocated
            : qint64(std::llround(double(total) * slice.weightedBasis / weightTotal));
        allocated += amount;
        if (amount <= 0)
            continue;
        TimephasedValue value;
        value.type = TimephasedValue::RemainingWork;
        value.uniqueId = assignment.uniqueId;
        value.start = slice.start;
        value.finish = slice.finish;
        value.unit = 1;
        value.value = isoDuration(amount);
        retained.append(value);
    }
    std::sort(retained.begin(), retained.end(), [](const auto &left, const auto &right) {
        if (left.start != right.start)
            return left.start < right.start;
        return left.type < right.type;
    });
    assignment.timephasedValues = retained;
    return true;
}

} // namespace

QString WorkContouring::name(int contour)
{
    switch (contour) {
    case BackLoaded: return QStringLiteral("Back Loaded");
    case FrontLoaded: return QStringLiteral("Front Loaded");
    case DoublePeak: return QStringLiteral("Double Peak");
    case EarlyPeak: return QStringLiteral("Early Peak");
    case LatePeak: return QStringLiteral("Late Peak");
    case Bell: return QStringLiteral("Bell");
    case Turtle: return QStringLiteral("Turtle");
    case Contoured: return QStringLiteral("Contoured");
    default: return QStringLiteral("Flat");
    }
}

QVector<int> WorkContouring::segmentPercentages(int contour)
{
    switch (contour) {
    case BackLoaded: return {10, 15, 25, 50, 50, 75, 75, 100, 100, 100};
    case FrontLoaded: return {100, 100, 100, 75, 75, 50, 50, 25, 15, 10};
    case DoublePeak: return {25, 50, 100, 50, 25, 25, 50, 100, 50, 25};
    case EarlyPeak: return {25, 50, 100, 100, 75, 50, 50, 25, 15, 10};
    case LatePeak: return {10, 15, 25, 50, 50, 75, 100, 100, 50, 25};
    case Bell: return {10, 20, 40, 80, 100, 100, 80, 40, 20, 10};
    case Turtle: return {25, 50, 75, 100, 100, 100, 100, 75, 50, 25};
    default: return QVector<int>(10, 100);
    }
}

bool WorkContouring::apply(Project &project, int assignmentUid, int contour)
{
    Assignment *assignment = assignmentByUid(project, assignmentUid);
    if (!assignment || contour < Flat || contour > Contoured)
        return false;
    const Resource *resource = resourceByUid(project, assignment->resourceUniqueId);
    if (!resource || resource->type != Resource::Type::Work)
        return false;
    assignment->workContour = contour;
    return contour == Contoured || rebuild(project, *assignment);
}

bool WorkContouring::regenerate(Project &project, int assignmentUid)
{
    Assignment *assignment = assignmentByUid(project, assignmentUid);
    return assignment && rebuild(project, *assignment);
}

} // namespace schedule
