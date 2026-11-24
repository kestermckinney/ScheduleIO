// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_SCHEDULINGCALENDAR_H
#define SCHEDULE_SCHEDULINGCALENDAR_H

#include "scheduleio_export.h"
#include "model/project.h"
#include "model/workcalendar.h"

namespace schedule::SchedulingCalendar {

// Task calendar when present, otherwise the project calendar.
SCHEDULEIO_EXPORT WorkCalendar taskBase(const Project &project, const Task &task);

// Calendar governing one assignment: task/project working time intersected
// with the assigned work resource's calendar. Material and cost resources do
// not affect scheduling. IgnoreResourceCalendar selects only the task calendar.
SCHEDULEIO_EXPORT WorkCalendar assignment(const Project &project, const Task &task,
                                           const Assignment &assignment);

// Task-level dates derived from its assignment calendars. With multiple work
// resources the task starts when its first assignment can work and finishes
// when its last assignment finishes.
SCHEDULEIO_EXPORT QDateTime nextStart(const Project &project, const Task &task,
                                      const QDateTime &candidate);
SCHEDULEIO_EXPORT QDateTime finish(const Project &project, const Task &task,
                                   const QDateTime &start);
SCHEDULEIO_EXPORT QDateTime startForFinish(const Project &project, const Task &task,
                                           const QDateTime &finish);

// Speeds up the functions above for one project during a whole scheduling pass.
// Without it, every call rescans project.assignments, project.resources and
// project.calendars, which makes a pass over N tasks cost N x assignments; on an
// 11k-task schedule that is most of an edit's recalculation time. While an
// IndexScope for `project` is alive on this thread, those lookups come from
// tables built once, and resolved calendars are reused.
//
// Only valid while the project's assignment, resource and calendar LISTS keep
// their shape (no appends or removals, no change to an assignment's resource,
// a resource's type or calendar, a task's calendar, or a calendar's contents).
// Task dates and assignment work/dates may change freely. A nested scope for
// the same project reuses the outer one.
class SCHEDULEIO_EXPORT IndexScope
{
public:
    explicit IndexScope(const Project &project);
    ~IndexScope();
    IndexScope(const IndexScope &) = delete;
    IndexScope &operator=(const IndexScope &) = delete;

    struct Data;

private:
    Data *m_data = nullptr;       // owned; null when an outer scope is reused
    Data *m_previous = nullptr;   // the scope active before this one
};

} // namespace schedule::SchedulingCalendar

#endif // SCHEDULE_SCHEDULINGCALENDAR_H
