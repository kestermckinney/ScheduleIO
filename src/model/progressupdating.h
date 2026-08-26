// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_PROGRESSUPDATING_H
#define SCHEDULE_PROGRESSUPDATING_H

#include "scheduleio_export.h"

#include <QDateTime>
#include <QSet>

namespace schedule {

class Project;

// Project > Update Project: move unfinished automatic work so it starts after
// an as-of boundary, retaining recorded actuals and time-phased work values.
class SCHEDULEIO_EXPORT ProgressUpdating
{
public:
    enum class UpdateAction {
        ZeroOrOneHundred = 0,
        ScheduledPercent = 1
    };

    // Project > Update Project progress actions. ScheduledPercent records the
    // scheduled duration/work completed through `through`; ZeroOrOneHundred
    // records only tasks whose scheduled finish is on or before the boundary.
    // Empty taskUids means the whole project.
    static int updateScheduledProgress(Project &project, const QDateTime &through,
                                       UpdateAction action,
                                       const QSet<int> &taskUids = {});

    // Set a leaf task's work-based progress. Actual/remaining work is
    // redistributed proportionally across Work-resource assignments while
    // Material and Cost assignments remain untouched.
    static bool setPercentWorkComplete(Project &project, int taskUid,
                                       double percent);

    // Direct task tracking-field edits. Duration/work values are milliseconds.
    // The paired actual/remaining value and percentage fields are kept canonical,
    // and task work is redistributed across Work-resource assignments only.
    static bool setActualStart(Project &project, int taskUid,
                               const QDateTime &actualStart);
    static bool setActualFinish(Project &project, int taskUid,
                                const QDateTime &actualFinish);
    static bool setActualDuration(Project &project, int taskUid, qint64 millis);
    static bool setRemainingDuration(Project &project, int taskUid, qint64 millis);
    static bool setActualWork(Project &project, int taskUid, qint64 millis);
    static bool setRemainingWork(Project &project, int taskUid, qint64 millis);

    // Empty taskUids means the whole project. Returns the number of leaf tasks
    // whose incomplete schedule changed.
    static int rescheduleIncompleteWork(Project &project, const QDateTime &after,
                                        const QSet<int> &taskUids = {});
};

} // namespace schedule

#endif // SCHEDULE_PROGRESSUPDATING_H
