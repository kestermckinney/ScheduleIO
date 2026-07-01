// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/calendar.h"

namespace schedule {

bool Calendar::operator==(const Calendar &o) const
{
    return uniqueId == o.uniqueId
        && name == o.name
        && baseCalendarUniqueId == o.baseCalendarUniqueId
        && workingDayMask == o.workingDayMask
        && workingTimes == o.workingTimes
        && exceptions == o.exceptions;
}

} // namespace schedule
