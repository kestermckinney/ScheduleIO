// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/mppassignment.h"

#include <QtGlobal>

bool MppAssignment::operator==(const MppAssignment &o) const
{
    return uniqueId == o.uniqueId
        && taskUniqueId == o.taskUniqueId
        && resourceUniqueId == o.resourceUniqueId
        && qFuzzyCompare(units + 1.0, o.units + 1.0)
        && workMillis == o.workMillis
        && notes == o.notes
        && cost == o.cost
        && actualCost == o.actualCost
        && remainingCost == o.remainingCost
        && costVariance == o.costVariance
        && baselines == o.baselines
        && customFields == o.customFields;
}
