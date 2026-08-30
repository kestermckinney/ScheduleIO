// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/project.h"

namespace schedule {

bool Project::operator==(const Project &o) const
{
    return formatVersion == o.formatVersion
        && title == o.title
        && author == o.author
        && startDate == o.startDate
        && finishDate == o.finishDate
        && scheduleFromStart == o.scheduleFromStart
        && multipleCriticalPaths == o.multipleCriticalPaths
        && statusDate == o.statusDate
        && calendarUniqueId == o.calendarUniqueId
        && budgetCost == o.budgetCost
        && budgetWorkMillis == o.budgetWorkMillis
        && tasks == o.tasks
        && resources == o.resources
        && assignments == o.assignments
        && calendars == o.calendars
        && relations == o.relations
        && customFieldDefinitions == o.customFieldDefinitions
        && viewStyles == o.viewStyles
        && resourceUsageStyles == o.resourceUsageStyles
        && teamPlannerStyles == o.teamPlannerStyles
        && calendarStyles == o.calendarStyles
        && reportAccentColor == o.reportAccentColor
        // Project options (File > Options)
        && newTasksManual == o.newTasksManual
        && newTaskStartIsProjectStart == o.newTaskStartIsProjectStart
        && defaultTaskType == o.defaultTaskType
        && defaultDurationUnits == o.defaultDurationUnits
        && defaultWorkUnits == o.defaultWorkUnits
        && newTasksEffortDriven == o.newTasksEffortDriven
        && autoLinkTasks == o.autoLinkTasks
        && splitInProgressTasks == o.splitInProgressTasks
        && honorConstraints == o.honorConstraints
        && criticalSlackLimit == o.criticalSlackLimit
        && weekStartDay == o.weekStartDay
        && fiscalYearStartMonth == o.fiscalYearStartMonth
        && fiscalYearUsesStartYear == o.fiscalYearUsesStartYear
        && defaultStartTime == o.defaultStartTime
        && defaultEndTime == o.defaultEndTime
        && minutesPerDay == o.minutesPerDay
        && minutesPerWeek == o.minutesPerWeek
        && daysPerMonth == o.daysPerMonth
        && moveCompletedEndsBack == o.moveCompletedEndsBack
        && moveRemainingStartsBack == o.moveRemainingStartsBack
        && moveRemainingStartsForward == o.moveRemainingStartsForward
        && moveCompletedEndsForward == o.moveCompletedEndsForward
        && statusUpdatesResource == o.statusUpdatesResource
        && currencySymbol == o.currencySymbol
        && currencySymbolPosition == o.currencySymbolPosition
        && currencyDigits == o.currencyDigits
        && currencyCode == o.currencyCode
        && defaultStandardRate == o.defaultStandardRate
        && defaultOvertimeRate == o.defaultOvertimeRate
        && defaultFixedCostAccrual == o.defaultFixedCostAccrual
        && defaultEarnedValueMethod == o.defaultEarnedValueMethod
        && baselineForEarnedValue == o.baselineForEarnedValue
        && showProjectSummaryTask == o.showProjectSummaryTask;
}

} // namespace schedule
