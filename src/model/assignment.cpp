// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/assignment.h"

#include <QtGlobal>

namespace schedule {

bool Assignment::operator==(const Assignment &o) const
{
    return uniqueId == o.uniqueId
        && taskUniqueId == o.taskUniqueId
        && resourceUniqueId == o.resourceUniqueId
        && qFuzzyCompare(units + 1.0, o.units + 1.0)
        && workMillis == o.workMillis
        && notes == o.notes
        && start == o.start
        && finish == o.finish
        && delayMillis == o.delayMillis
        && levelingDelayMillis == o.levelingDelayMillis
        && actualWorkMillis == o.actualWorkMillis
        && remainingWorkMillis == o.remainingWorkMillis
        && cost == o.cost
        && actualCost == o.actualCost
        && remainingCost == o.remainingCost
        && costVariance == o.costVariance
        && baselines == o.baselines
        && customFields == o.customFields;
}

} // namespace schedule
