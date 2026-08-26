// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#ifndef SCHEDULE_MATERIALCOSTING_H
#define SCHEDULE_MATERIALCOSTING_H

#include "scheduleio_export.h"
#include "model/project.h"

namespace schedule {

class SCHEDULEIO_EXPORT MaterialCosting
{
public:
    static qint64 quantityMillisForDuration(qint64 durationMillis, double rate,
                                            int variableRateUnits);
    static void recalculate(Project &project);
};

} // namespace schedule

#endif // SCHEDULE_MATERIALCOSTING_H
