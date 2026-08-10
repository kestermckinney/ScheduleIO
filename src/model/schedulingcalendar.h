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

} // namespace schedule::SchedulingCalendar

#endif // SCHEDULE_SCHEDULINGCALENDAR_H
