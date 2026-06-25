// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPCALENDAR_H
#define MPPCALENDAR_H

#include "mppio_export.h"

#include <QString>

// A working-time calendar (entity type 0xb / "Calendar").
// Working days are represented as a 7-bit mask, Monday(bit0)..Sunday(bit6).
class MPPIO_EXPORT MppCalendar
{
public:
    int uniqueId = 0;
    QString name;
    int baseCalendarUniqueId = -1;   // -1 == none (this is a base calendar)
    quint8 workingDayMask = 0;

    bool operator==(const MppCalendar &o) const;
    bool operator!=(const MppCalendar &o) const { return !(*this == o); }
};

#endif // MPPCALENDAR_H
