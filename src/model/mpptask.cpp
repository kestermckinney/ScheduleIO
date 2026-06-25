// Copyright (C) 2026 Paul McKinney
// SPDX-License-Identifier: GPL-3.0-only

#include "model/mpptask.h"

bool MppTask::operator==(const MppTask &o) const
{
    return uniqueId == o.uniqueId
        && id == o.id
        && outlineLevel == o.outlineLevel
        && name == o.name
        && start == o.start
        && finish == o.finish
        && durationMillis == o.durationMillis
        && qFuzzyCompare(percentComplete + 1.0, o.percentComplete + 1.0)
        && milestone == o.milestone
        && summary == o.summary;
}
