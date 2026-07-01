// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/costrate.h"

namespace schedule {

bool CostRate::operator==(const CostRate &o) const
{
    return table == o.table
        && startDate == o.startDate
        && endDate == o.endDate
        && standardRate == o.standardRate
        && standardRateUnit == o.standardRateUnit
        && overtimeRate == o.overtimeRate
        && overtimeRateUnit == o.overtimeRateUnit
        && costPerUse == o.costPerUse;
}

} // namespace schedule
