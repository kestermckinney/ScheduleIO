// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_RESOURCELEVELING_H
#define SCHEDULE_RESOURCELEVELING_H

#include "scheduleio_export.h"
#include "model/project.h"

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
