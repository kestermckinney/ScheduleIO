// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_COSTRATE_H
#define SCHEDULE_COSTRATE_H

#include "scheduleio_export.h"

#include <QDateTime>

namespace schedule {

// One time-phased entry of a resource cost-rate table. Microsoft Project gives a
// resource up to five cost-rate tables (A..E, `table` 0..4); each table holds a
// list of entries that take effect over a date range. Value type for round-tripping.
class SCHEDULEIO_EXPORT CostRate
{
public:
    int table = 0;               // cost-rate table 0..4 == A..E
    QDateTime startDate;         // effective from (invalid == open start)
    QDateTime endDate;           // effective to (invalid == open / "until further notice")
    double standardRate = 0.0;   // standard rate, expressed per `standardRateUnit`
    int standardRateUnit = 2;    // rate time unit (Microsoft format: 2 = per hour, 3 = per day, ...)
    double overtimeRate = 0.0;   // overtime rate, expressed per `overtimeRateUnit`
    int overtimeRateUnit = 2;
    double costPerUse = 0.0;     // per-use cost, in the project's currency unit

    bool operator==(const CostRate &o) const;
    bool operator!=(const CostRate &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_COSTRATE_H
