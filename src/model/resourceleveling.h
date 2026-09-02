// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_RESOURCELEVELING_H
#define SCHEDULE_RESOURCELEVELING_H

#include "scheduleio_export.h"
#include "model/project.h"
#include "model/workcalendar.h"   // WorkProfile carries a resolved calendar

#include <QDateTime>
#include <QList>
#include <QSet>

namespace schedule {

// Resource load analysis and leveling, mirroring MS Project's Resource Leveling.
//
// A resource is *overallocated* when, at some instant, the units of its concurrently
// running assignments exceed the resource's available units (Max Units). Leveling
// removes overallocations by delaying an eligible task chosen by the requested leveling
// order (Task::levelingDelayMillis, which the forward pass honours so successors follow).
class SCHEDULEIO_EXPORT ResourceLeveling
{
public:
    enum class Order {
        IdOnly,
        Standard,
        PriorityStandard
    };

    struct Options
    {
        Order order = Order::PriorityStandard;
        bool levelOnlyWithinAvailableSlack = false;
        bool allowTaskSplitting = true;
        // Empty means all tasks. When populated, other tasks still contribute load,
        // but only tasks in this set may receive leveling delay.
        QSet<int> taskUniqueIds;
    };

    // A window during which one resource is loaded beyond its available units.
    struct Overallocation
    {
        int resourceUniqueId = 0;
        QDateTime start;
        QDateTime finish;
        double peakUnits = 0.0;   // greatest concurrent units in the window
        double maxUnits = 1.0;    // the resource's available units (capacity)
    };

    // Every overallocation window across the project, resource by resource, in time
    // order. Uses each assignment's scheduled span (falling back to its task's span).
    static QList<Overallocation> overallocations(const Project &project);

    // Whether a specific resource is overallocated anywhere.
    static bool isOverallocated(const Project &project, int resourceUniqueId);

    // Working milliseconds of `assignment`'s work that fall within [from, to) -- its
    // total work spread over its span in proportion to working time. Used by the
    // time-phased Resource Usage grid.
    static qint64 workInPeriod(const Project &project, const Assignment &assignment,
                               const QDateTime &from, const QDateTime &to);

    // Everything the calculation above needs that does not depend on which period
    // is being asked about: whether the file carries authoritative time-phased
    // buckets, and otherwise the assignment's span, the calendar it is worked
    // against and the working time that span contains.
    //
    // Resolving those means building a WorkCalendar and walking the span, which
    // costs the same for the first cell of a row as for the thousandth. A caller
    // filling a grid -- the Resource Usage and Task Usage views run one column per
    // day across the whole schedule -- should build this once per assignment and
    // sample it per column; the per-period overload below then only measures the
    // overlap. The single-shot overload above is this pair, called back to back.
    struct WorkProfile
    {
        const Assignment *assignment = nullptr;
        bool timephased = false;   // authoritative buckets exist; the rest is unused
        QDateTime start;
        QDateTime finish;
        WorkCalendar calendar;
        qint64 spanWork = 0;       // working ms between start and finish

        // Names the calendar above without comparing it: the task calendar it is
        // built on, and the resource calendar intersected into it (-1 when none
        // is -- a material or cost resource, IgnoreResourceCalendar, a manually
        // scheduled task). Two assignments whose keys match are worked against
        // identical calendars, which lets a caller filling a grid measure each
        // period column once per key instead of once per assignment. See
        // SchedulingCalendar::assignment(), whose inputs these are.
        int taskCalendarUid = -1;
        int resourceCalendarUid = -1;
        bool sameCalendarAs(const WorkProfile &other) const
        {
            return taskCalendarUid == other.taskCalendarUid
                && resourceCalendarUid == other.resourceCalendarUid;
        }
    };

    static WorkProfile workProfile(const Project &project, const Assignment &assignment);
    static qint64 workInPeriod(const WorkProfile &profile,
                               const QDateTime &from, const QDateTime &to);

    // The resource's total assigned work (milliseconds) over all its assignments.
    static qint64 resourceWork(const Project &project, int resourceUniqueId);

    // Level the project: add leveling delay until no resource is overallocated,
    // using Priority/Standard order. Does not move manual or already-started tasks.
    // Reschedules as it goes. Returns the number of tasks that were delayed.
    static int level(Project &project);
    static int level(Project &project, const Options &options);

    // Remove all leveling delay (MS Project's "Clear Leveling") and reschedule.
    static void clearLeveling(Project &project);
};

} // namespace schedule

#endif // SCHEDULE_RESOURCELEVELING_H
