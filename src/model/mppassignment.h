// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPASSIGNMENT_H
#define MPPASSIGNMENT_H

#include "mppio_export.h"
#include "model/mppbaseline.h"
#include "model/mppcustomfield.h"

#include <QList>

// Links a resource to a task (entity type 0xc / "Assignment").
class MPPIO_EXPORT MppAssignment
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

    QList<MppBaseline> baselines;        // assignment baselines: cost/work/start/finish
    QList<MppCustomField> customFields;

    bool operator==(const MppAssignment &o) const;
    bool operator!=(const MppAssignment &o) const { return !(*this == o); }
};

#endif // MPPASSIGNMENT_H
