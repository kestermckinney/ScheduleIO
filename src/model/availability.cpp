// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/availability.h"

namespace schedule {

bool AvailabilityPeriod::operator==(const AvailabilityPeriod &o) const
{
    return startDate == o.startDate
        && endDate == o.endDate
        && qFuzzyCompare(units + 1.0, o.units + 1.0);
}

} // namespace schedule
