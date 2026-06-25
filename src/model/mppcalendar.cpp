// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/mppcalendar.h"

bool MppCalendar::operator==(const MppCalendar &o) const
{
    return uniqueId == o.uniqueId
        && name == o.name
        && baseCalendarUniqueId == o.baseCalendarUniqueId
        && workingDayMask == o.workingDayMask;
}
