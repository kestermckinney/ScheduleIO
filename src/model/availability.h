// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_AVAILABILITY_H
#define SCHEDULE_AVAILABILITY_H

#include "scheduleio_export.h"

#include <QDateTime>

namespace schedule {

// One time-phased row of a resource's Availability table (MS Project's Resource
// Information > General "Resource Availability" grid): the Max Units the
// resource can be booked at over a date range. Value type for round-tripping.
class SCHEDULEIO_EXPORT AvailabilityPeriod
{
public:
    QDateTime startDate;   // effective from (invalid == open start, MS Project's "NA")
    QDateTime endDate;     // effective to (invalid == open end, MS Project's "NA")
    double units = 1.0;    // Max Units for this period, 1.0 == 100%

    bool operator==(const AvailabilityPeriod &o) const;
    bool operator!=(const AvailabilityPeriod &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_AVAILABILITY_H
