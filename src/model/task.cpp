// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/task.h"

namespace schedule {

bool EarnedValue::operator==(const EarnedValue &o) const
{
    return pv == o.pv && ev == o.ev && ac == o.ac && cv == o.cv && sv == o.sv
        && cpi == o.cpi && spi == o.spi && eac == o.eac && tcpi == o.tcpi;
}

bool Task::operator==(const Task &o) const
{
    return uniqueId == o.uniqueId
        && id == o.id
        && outlineLevel == o.outlineLevel
        && name == o.name
        && start == o.start
        && finish == o.finish
        && durationMillis == o.durationMillis
        && durationFormat == o.durationFormat
        && qFuzzyCompare(percentComplete + 1.0, o.percentComplete + 1.0)
        && milestone == o.milestone
        && summary == o.summary
        && constraintType == o.constraintType
        && constraintDate == o.constraintDate
        && wbs == o.wbs
        && notes == o.notes
        && manual == o.manual
        && effortDriven == o.effortDriven
        && taskType == o.taskType
        && priority == o.priority
        && deadline == o.deadline
        && calendarUniqueId == o.calendarUniqueId
        && lateStart == o.lateStart
        && lateFinish == o.lateFinish
        && totalSlackMillis == o.totalSlackMillis
        && freeSlackMillis == o.freeSlackMillis
        && critical == o.critical
        && cost == o.cost
        && fixedCost == o.fixedCost
        && actualCost == o.actualCost
        && remainingCost == o.remainingCost
        && costVariance == o.costVariance
        && actualStart == o.actualStart
        && actualFinish == o.actualFinish
        && actualDurationMillis == o.actualDurationMillis
        && actualWorkMillis == o.actualWorkMillis
        && evm == o.evm
        && baselines == o.baselines
        && customFields == o.customFields;
}

} // namespace schedule
