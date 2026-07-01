// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/resource.h"

namespace schedule {

bool Resource::operator==(const Resource &o) const
{
    return uniqueId == o.uniqueId
        && id == o.id
        && name == o.name
        && initials == o.initials
        && qFuzzyCompare(maxUnits + 1.0, o.maxUnits + 1.0)
        && notes == o.notes
        && cost == o.cost
        && actualCost == o.actualCost
        && remainingCost == o.remainingCost
        && costVariance == o.costVariance
        && baselines == o.baselines
        && customFields == o.customFields
        && costRates == o.costRates;
}

} // namespace schedule
