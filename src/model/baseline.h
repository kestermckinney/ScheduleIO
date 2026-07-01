// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_BASELINE_H
#define SCHEDULE_BASELINE_H

#include "scheduleio_export.h"

#include <QDateTime>

namespace schedule {

// One saved baseline snapshot of an entity (task/resource/assignment). Microsoft
// Project keeps a current baseline (number 0) plus up to ten saved baselines
// (numbers 1..10). Fields not applicable to an entity stay at their defaults
// (e.g. resources have no baseline start/finish). Value type for round-tripping.
class SCHEDULEIO_EXPORT Baseline
{
public:
    int number = 0;                // 0 = current baseline, 1..10 = saved baselines
    double cost = 0.0;             // baseline cost, in the project's currency unit
    qint64 workMillis = 0;         // baseline work, normalised to milliseconds
    QDateTime start;               // baseline start (invalid if not set)
    QDateTime finish;              // baseline finish (invalid if not set)
    qint64 durationMillis = 0;     // baseline duration, normalised to milliseconds

    bool operator==(const Baseline &o) const;
    bool operator!=(const Baseline &o) const { return !(*this == o); }
};

} // namespace schedule

#endif // SCHEDULE_BASELINE_H
