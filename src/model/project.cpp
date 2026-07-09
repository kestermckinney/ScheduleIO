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
        && statusDate == o.statusDate
        && calendarUniqueId == o.calendarUniqueId
        && tasks == o.tasks
        && resources == o.resources
        && assignments == o.assignments
        && calendars == o.calendars
        && relations == o.relations
        && viewStyles == o.viewStyles
        && resourceUsageStyles == o.resourceUsageStyles
        && teamPlannerStyles == o.teamPlannerStyles
        && calendarStyles == o.calendarStyles;
}

} // namespace schedule
