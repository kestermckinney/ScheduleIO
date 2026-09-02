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
        && workMillis == o.workMillis
        && qFuzzyCompare(percentComplete + 1.0, o.percentComplete + 1.0)
        && qFuzzyCompare(physicalPercentComplete + 1.0,
                         o.physicalPercentComplete + 1.0)
        && earnedValueMethod == o.earnedValueMethod
        && milestone == o.milestone
        && summary == o.summary
        && recurring == o.recurring
        && segments == o.segments
        && constraintType == o.constraintType
        && constraintDate == o.constraintDate
        && wbs == o.wbs
        && notes == o.notes
        && hyperlink == o.hyperlink
        && hyperlinkAddress == o.hyperlinkAddress
        && hyperlinkSubAddress == o.hyperlinkSubAddress
        && active == o.active
        && manual == o.manual
        && levelingDelayMillis == o.levelingDelayMillis
        && effortDriven == o.effortDriven
        && taskType == o.taskType
        && priority == o.priority
        && deadline == o.deadline
        && calendarUniqueId == o.calendarUniqueId
        && ignoreResourceCalendar == o.ignoreResourceCalendar
        && earlyStart == o.earlyStart
        && earlyFinish == o.earlyFinish
        && lateStart == o.lateStart
        && lateFinish == o.lateFinish
        && startSlackMillis == o.startSlackMillis
        && finishSlackMillis == o.finishSlackMillis
        && totalSlackMillis == o.totalSlackMillis
        && freeSlackMillis == o.freeSlackMillis
        && critical == o.critical
        && cost == o.cost
        && fixedCost == o.fixedCost
        && fixedCostAccrual == o.fixedCostAccrual
        && actualCost == o.actualCost
        && remainingCost == o.remainingCost
        && costVariance == o.costVariance
        && startVarianceMillis == o.startVarianceMillis
        && finishVarianceMillis == o.finishVarianceMillis
        && durationVarianceMillis == o.durationVarianceMillis
        && workVarianceMillis == o.workVarianceMillis
        && actualStart == o.actualStart
        && actualFinish == o.actualFinish
        && actualDurationMillis == o.actualDurationMillis
        && actualWorkMillis == o.actualWorkMillis
        && evm == o.evm
        && rowFormat == o.rowFormat
        && barColor == o.barColor
        && cellFormats == o.cellFormats
        && baselines == o.baselines
        && customFields == o.customFields;
}

} // namespace schedule
