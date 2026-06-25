// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/mppproject.h"

bool MppProject::operator==(const MppProject &o) const
{
    return formatVersion == o.formatVersion
        && title == o.title
        && author == o.author
        && startDate == o.startDate
        && finishDate == o.finishDate
        && tasks == o.tasks
        && resources == o.resources
        && assignments == o.assignments
        && calendars == o.calendars;
}
