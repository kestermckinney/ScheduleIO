// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_SCHEDULER_H
#define SCHEDULE_SCHEDULER_H

#include "scheduleio_export.h"
#include "model/project.h"
#include "model/workcalendar.h"

#include <QDateTime>

namespace schedule {

// Dependency-driven scheduling over a Project, mirroring WINPROJ's
// TBkndRecalc forward pass ("Recalc-ForwardRecalc"): auto-scheduled tasks are
// pushed to the earliest start their predecessors + lag allow, then their
// finish is derived from the duration in working time.
//
// The static working-time helpers use MS Project's default Standard calendar
// (Monday-Friday, 08:00-12:00 and 13:00-17:00); reschedule() resolves each
// task's calendar (task calendar, else the project calendar) via WorkCalendar.
class SCHEDULEIO_EXPORT Scheduler
{
public:
    // The project's own calendar (project.calendarUniqueId, else the calendar
    // named "Standard", else the built-in Standard week).
    static WorkCalendar projectCalendar(const Project &project);

    // Roll forward to the next instant work can start (skips nights, the lunch
    // hour and weekends; a period *end* such as 17:00 rolls to the next 08:00).
    static QDateTime nextWorkStart(const QDateTime &dt);

    // Roll back to the previous instant work can end (a period *start* such as
    // 08:00 rolls back to the previous 17:00).
    static QDateTime prevWorkEnd(const QDateTime &dt);

    // Advance (or, for negative millis, rewind) by an amount of working time.
    // addWork(x, 0) == nextWorkStart(x).
    static QDateTime addWork(const QDateTime &from, qint64 millis);

    // Working milliseconds between two instants (0 when to <= from).
    static qint64 workBetween(const QDateTime &from, const QDateTime &to);

    // True when `toUid` is reachable from `fromUid` by following dependency
    // links predecessor -> successor. Used to veto edits that would create a
    // dependency cycle (adding pred P to task T is illegal iff P is reachable
    // FROM T).
    static bool reachable(const Project &project, int fromUid, int toUid);

    // The forward pass: recompute start/finish of every auto-scheduled,
    // non-summary task from its predecessors and lag (FS/SS/FF/SF), honouring
    // Must Start/Finish On and Start/Finish No Earlier Than constraints.
    // Tasks with no predecessors keep their current start (it anchors them);
    // manually scheduled tasks and summaries are never moved (summaries are
    // rolled up from their children by the caller). Cycles are broken by
    // leaving the tasks on the cycle untouched.
    static void reschedule(Project &project);

    // The backward pass (WINPROJ "Recalc-BackwardRecalc"): from the project
    // finish (latest task finish), walk successors-first computing each leaf
    // task's late start/finish, total and free slack, and the critical flag
    // (total slack <= 0). Deadlines and Must Start/Finish On constraints cap
    // the late dates. Summaries get the minimum slack of their children and
    // are critical when any child is. Call after reschedule().
    static void computeSlack(Project &project);
};

} // namespace schedule

#endif // SCHEDULE_SCHEDULER_H
