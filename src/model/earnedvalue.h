// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_EARNEDVALUE_H
#define SCHEDULE_EARNEDVALUE_H

#include "scheduleio_export.h"

#include <QDateTime>

namespace schedule {

class Project;

// Earned value exactly as Microsoft Project calculates it, written into every
// task's Task::evm (the BCWS/BCWP/ACWP/SV/CV/CPI/SPI/EAC/TCPI fields Project
// stores in the file). Verified field-by-field against Project 2016 on the
// same file (2026-09-27):
//  - BCWS: the time-phased baseline cost accrued by the status date.
//  - BCWP: the baseline cost that falls in the first `% complete` (or
//    Physical % Complete, per the task's EV method) of the baseline's
//    working-time span -- not % complete x BAC, which differs whenever the
//    baseline spread is uneven.
//  - ACWP: the task's actual cost.
//  - Summaries (the project summary included) sum their active leaves.
// The baseline is File > Options > "Baseline for Earned Value calculation".
class SCHEDULEIO_EXPORT EarnedValueCalculation
{
public:
    static void recalculate(Project &project);

    // The instant earned value is measured at: Project > Status Date, or the
    // current date (at the default start time) when none is set.
    static QDateTime statusDate(const Project &project);
};

} // namespace schedule

#endif // SCHEDULE_EARNEDVALUE_H
