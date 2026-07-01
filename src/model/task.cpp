// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/task.h"

namespace schedule {

bool Task::operator==(const Task &o) const
{
    return uniqueId == o.uniqueId
        && id == o.id
        && outlineLevel == o.outlineLevel
        && name == o.name
        && start == o.start
        && finish == o.finish
        && durationMillis == o.durationMillis
        && qFuzzyCompare(percentComplete + 1.0, o.percentComplete + 1.0)
        && milestone == o.milestone
        && summary == o.summary
        && constraintType == o.constraintType
        && constraintDate == o.constraintDate
        && wbs == o.wbs
        && notes == o.notes
        && cost == o.cost
        && fixedCost == o.fixedCost
        && actualCost == o.actualCost
        && remainingCost == o.remainingCost
        && costVariance == o.costVariance
        && baselines == o.baselines
        && customFields == o.customFields;
}

} // namespace schedule
