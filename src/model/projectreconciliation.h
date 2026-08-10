// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_PROJECTRECONCILIATION_H
#define SCHEDULE_PROJECTRECONCILIATION_H

#include "scheduleio_export.h"
#include "model/project.h"

#include <QStringList>

namespace schedule {

// Canonical reconciliation for assignment time-phased values and their task/resource
// rollups. Actual and Remaining Work buckets are authoritative for their own stream;
// aggregate-only values remain authoritative when that stream has no buckets.
class SCHEDULEIO_EXPORT ProjectReconciliation
{
public:
    struct Totals
    {
        qint64 workMillis = 0;
        qint64 actualWorkMillis = 0;
        qint64 remainingWorkMillis = 0;
        qint64 overtimeWorkMillis = 0;
        qint64 actualOvertimeWorkMillis = 0;
        qint64 remainingOvertimeWorkMillis = 0;
        double cost = 0.0;
        double actualCost = 0.0;
        double remainingCost = 0.0;
        double overtimeCost = 0.0;
    };

    // Normalize assignments, apply available resource rates, and write task/resource
    // cost plus task labor rollups. Summary tasks are rolled up from active leaves.
    static void reconcile(Project &project);

    static Totals taskTotals(const Project &project, int taskUniqueId);
    static Totals resourceTotals(const Project &project, int resourceUniqueId);

    // Empty after reconcile(). Intended for tests, diagnostics, and save-time guards.
    static QStringList invariantViolations(const Project &project);
};

} // namespace schedule

#endif // SCHEDULE_PROJECTRECONCILIATION_H
