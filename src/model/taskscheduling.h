// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_TASKSCHEDULING_H
#define SCHEDULE_TASKSCHEDULING_H

#include "scheduleio_export.h"
#include "model/project.h"

namespace schedule {

// The scheduling triangle (Work = Duration x Units, in working time) and the
// effort-driven resourcing rules, matching WINPROJ's five scheduling modes
// (task type 0/1/2 x effort-driven; Fixed Work is always effort-driven).
//
// Which side of the triangle an edit recalculates follows MS Project:
//
//   edit ->        | Fixed Units | Fixed Duration | Fixed Work
//   Duration       | Work        | Work           | Units
//   Work           | Duration    | Units          | Duration
//   Units          | Duration    | Work           | Duration
//
// Effort-driven changes only what happens when resources are ADDED to or
// REMOVED from the task: total work stays constant and is redistributed
// proportionally to units; non-effort-driven tasks gain/lose the added
// resource's own work (duration x units) instead.
//
// All functions operate on leaf tasks (summaries roll up elsewhere) and use
// the task's governing calendar (task calendar, else the project calendar).
// Resource calendars are not consulted yet. Callers are expected to run
// Scheduler::reschedule afterwards so successors follow a moved finish.
class SCHEDULEIO_EXPORT TaskScheduling
{
public:
    // Total assigned work on the task (sum over its real assignments).
    static qint64 taskWork(const Project &p, int taskUid);

    // The user edited the task's duration.
    static void setDuration(Project &p, int taskUid, qint64 durationMillis);

    // The user edited the task's total work.
    static void setWork(Project &p, int taskUid, qint64 workMillis);

    // The user edited one assignment's units (1.0 == 100%).
    static void setAssignmentUnits(Project &p, int assignmentUid, double units);

    // The user edited one assignment's work.
    static void setAssignmentWork(Project &p, int assignmentUid, qint64 workMillis);

    // Assign a resource to the task. Returns the new assignment's unique id,
    // or -1 when the task/resource is unknown or already assigned.
    static int addAssignment(Project &p, int taskUid, int resourceUid, double units = 1.0);

    // Remove an assignment (effort-driven tasks redistribute its work).
    static void removeAssignment(Project &p, int assignmentUid);

    // Re-derive the task's duration/finish/work bookkeeping and each
    // assignment's start/finish from the current assignment work + units.
    // Called by the edit functions; public so imports can normalise too.
    static void syncTask(Project &p, int taskUid);
};

} // namespace schedule

#endif // SCHEDULE_TASKSCHEDULING_H
