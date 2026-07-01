// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_ASSIGNMENT_H
#define SCHEDULE_ASSIGNMENT_H

#include "scheduleio_export.h"
#include "model/baseline.h"
#include "model/customfield.h"

#include <QList>

namespace schedule {

// Links a resource to a task (entity type 0xc / "Assignment").
class SCHEDULEIO_EXPORT Assignment
{
public:
    int uniqueId = 0;
    int taskUniqueId = 0;
    int resourceUniqueId = 0;
    double units = 1.0;            // assigned units (1.0 == 100%)
    qint64 workMillis = 0;         // assigned work, normalised to milliseconds
    QString notes;                 // raw RTF source of the assignment's notes (empty if none)

    // Cost (in the project's currency unit). Assignments have no fixed cost.
    double cost = 0.0;
    double actualCost = 0.0;
    double remainingCost = 0.0;
    double costVariance = 0.0;

    QList<Baseline> baselines;        // assignment baselines: cost/work/start/finish
    QList<CustomField> customFields;

    bool operator==(const Assignment &o) const;
    bool operator!=(const Assignment &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_ASSIGNMENT_H
