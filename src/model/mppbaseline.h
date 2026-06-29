// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef MPPBASELINE_H
#define MPPBASELINE_H

#include "mppio_export.h"

#include <QDateTime>

// One saved baseline snapshot of an entity (task/resource/assignment). Microsoft
// Project keeps a current baseline (number 0) plus up to ten saved baselines
// (numbers 1..10). Fields not applicable to an entity stay at their defaults
// (e.g. resources have no baseline start/finish). Value type for round-tripping.
class MPPIO_EXPORT MppBaseline
{
public:
    int number = 0;                // 0 = current baseline, 1..10 = saved baselines
    double cost = 0.0;             // baseline cost, in the project's currency unit
    qint64 workMillis = 0;         // baseline work, normalised to milliseconds
    QDateTime start;               // baseline start (invalid if not set)
    QDateTime finish;              // baseline finish (invalid if not set)
    qint64 durationMillis = 0;     // baseline duration, normalised to milliseconds

    bool operator==(const MppBaseline &o) const;
    bool operator!=(const MppBaseline &o) const { return !(*this == o); }
};

#endif // MPPBASELINE_H
